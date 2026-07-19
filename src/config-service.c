#define _POSIX_C_SOURCE 200809L
#include "config-service.h"
#include "config-management.h"
#include "config.h"
#include <microhttpd.h>
#include <libwebsockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <ctype.h>

/* DRM includes for display mode detection */
typedef unsigned int drm_handle_t;
typedef unsigned int drm_magic_t;
typedef unsigned int drm_context_t;
typedef unsigned int drm_drawable_t;
#include <libdrm/drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static struct MHD_Daemon *daemon = NULL;
static char config_file_path[512];
static pthread_mutex_t config_mutex = PTHREAD_MUTEX_INITIALIZER;

// Embedded web UI files
extern const char web_index_html[];
extern size_t web_index_html_size;
extern const char web_styles_css[];
extern size_t web_styles_css_size;
extern const char web_app_js[];
extern size_t web_app_js_size;

// Forward declarations
static int handle_get_config(struct MHD_Connection *conn);
static int handle_post_config(struct MHD_Connection *conn, const char *upload_data,
                              size_t *upload_data_size, void **con_cls);
static int handle_get_tzdata(struct MHD_Connection *conn);
static int handle_get_ntp_status(struct MHD_Connection *conn);
static int handle_get_ntp_servers(struct MHD_Connection *conn);
static int handle_get_display_modes(struct MHD_Connection *conn);
static int handle_get_hdmi_ports(struct MHD_Connection *conn);
static int handle_post_network_vlan(struct MHD_Connection *conn, const char *upload_data,
                                     size_t *upload_data_size, void **con_cls);

/* Stratum 0 NTP servers for precise atomic clock synchronization */
static const char *stratum0_servers[] = {
    "time.google.com",
    "time.cloudflare.com",
    "time.nist.gov",
    "time.apple.com",
    "ntp.ubuntu.com",
    "pool.ntp.org",
    NULL
};

/* Interface statistics tracking for bandwidth rate calculation */
typedef struct {
    char name[64];
    unsigned long long tx_bytes_last;
    unsigned long long rx_bytes_last;
    time_t last_update_time;
    double tx_rate_kbps;
    double rx_rate_kbps;
} interface_stats_t;

typedef struct {
    struct lws *wsi;
} ws_client_t;

#define MAX_INTERFACES 16
#define MAX_WS_CLIENTS 16

static interface_stats_t interface_stats[MAX_INTERFACES];
static int interface_stats_count = 0;
static pthread_mutex_t interface_stats_mutex = PTHREAD_MUTEX_INITIALIZER;

static ws_client_t ws_clients[MAX_WS_CLIENTS];
static pthread_mutex_t ws_clients_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct lws_context *ws_context = NULL;
static unsigned long long config_revision = 1;

struct connection_info {
    char *post_data;
    size_t post_data_size;
    size_t post_data_capacity;
};

static void free_connection_info(struct connection_info *info) {
    if (info) {
        if (info->post_data) free(info->post_data);
        free(info);
    }
}

static int send_response(struct MHD_Connection *conn, const char *data, size_t size,
                         int status_code, const char *content_type) {
    struct MHD_Response *response = MHD_create_response_from_buffer(
        size, (void *)data, MHD_RESPMEM_MUST_COPY);
    if (!response) return MHD_NO;
    
    MHD_add_response_header(response, "Content-Type", content_type);
    int ret = MHD_queue_response(conn, status_code, response);
    MHD_destroy_response(response);
    return ret;
}

static void copy_line_value(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t value_len = strcspn(src, "\n");
    if (value_len >= dst_size) {
        value_len = dst_size - 1;
    }
    memcpy(dst, src, value_len);
    dst[value_len] = '\0';
}

static void broadcast_ws_message(const char *json_data) {
    if (!ws_context || !json_data) {
        return;
    }

    pthread_mutex_lock(&ws_clients_mutex);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_clients[i].wsi) {
            size_t len = strlen(json_data);
            unsigned char buf[LWS_PRE + len];
            memcpy(&buf[LWS_PRE], json_data, len);
            lws_write(ws_clients[i].wsi, &buf[LWS_PRE], len, LWS_WRITE_TEXT);
        }
    }
    pthread_mutex_unlock(&ws_clients_mutex);
}

static void broadcast_config_update(unsigned long long revision) {
    char json_data[128];
    snprintf(json_data, sizeof(json_data),
             "{\"type\":\"config_update\",\"config_revision\":%llu}",
             revision);
    broadcast_ws_message(json_data);
}

static int extract_config_revision(const char *json, unsigned long long *revision_out) {
    if (!json || !revision_out) {
        return 0;
    }

    const char *key = strstr(json, "\"config_revision\"");
    if (!key) {
        return 0;
    }

    const char *colon = strchr(key, ':');
    if (!colon) {
        return 0;
    }

    char *endptr = NULL;
    unsigned long long revision = strtoull(colon + 1, &endptr, 10);
    if (endptr == colon + 1) {
        return 0;
    }

    *revision_out = revision;
    return 1;
}

static char *config_to_json_with_revision(const ltc_config_t *config, unsigned long long revision) {
    char *base_json = config_to_json((ltc_config_t *)config);
    if (!base_json) {
        return NULL;
    }

    size_t base_len = strlen(base_json);
    while (base_len > 0 && isspace((unsigned char)base_json[base_len - 1])) {
        base_len--;
    }

    if (base_len < 2 || base_json[base_len - 1] != '}') {
        return base_json;
    }

    size_t output_size = base_len + 64;
    char *output = malloc(output_size);
    if (!output) {
        return base_json;
    }

    int written = snprintf(output, output_size,
                           "%.*s,\"config_revision\":%llu}",
                           (int)(base_len - 1), base_json, revision);
    free(base_json);

    if (written < 0 || (size_t)written >= output_size) {
        free(output);
        return NULL;
    }

    return output;
}

/* WebSocket callback and helpers */
static int update_interface_stats(void) {
    pthread_mutex_lock(&interface_stats_mutex);
    
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ip link show | grep -E '^[0-9]+:' | awk '{print $2}' | sed 's/:$//'");
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        pthread_mutex_unlock(&interface_stats_mutex);
        return -1;
    }
    
    char ifname[64];
    time_t now = time(NULL);
    
    while (fgets(ifname, sizeof(ifname), fp)) {
        /* Remove newline */
        size_t len = strlen(ifname);
        if (len > 0 && ifname[len-1] == '\n') {
            ifname[len-1] = '\0';
        }
        
        /* Skip loopback and docker interfaces */
        if (strcmp(ifname, "lo") == 0 || strstr(ifname, "docker") != NULL) {
            continue;
        }
        
        /* Get current stats */
        char tx_path[128], rx_path[128];
        snprintf(tx_path, sizeof(tx_path), "/sys/class/net/%s/statistics/tx_bytes", ifname);
        snprintf(rx_path, sizeof(rx_path), "/sys/class/net/%s/statistics/rx_bytes", ifname);
        
        unsigned long long tx_bytes = 0, rx_bytes = 0;
        FILE *tx_fp = fopen(tx_path, "r");
        if (tx_fp) {
            fscanf(tx_fp, "%llu", &tx_bytes);
            fclose(tx_fp);
        }
        FILE *rx_fp = fopen(rx_path, "r");
        if (rx_fp) {
            fscanf(rx_fp, "%llu", &rx_bytes);
            fclose(rx_fp);
        }
        
        /* Find or create stats entry */
        int idx = -1;
        for (int i = 0; i < interface_stats_count; i++) {
            if (strcmp(interface_stats[i].name, ifname) == 0) {
                idx = i;
                break;
            }
        }
        
        if (idx >= 0) {
            /* Calculate rate (bytes per second, convert to kbps) */
            time_t elapsed = now - interface_stats[idx].last_update_time;
            if (elapsed > 0) {
                unsigned long long tx_delta = tx_bytes - interface_stats[idx].tx_bytes_last;
                unsigned long long rx_delta = rx_bytes - interface_stats[idx].rx_bytes_last;
                interface_stats[idx].tx_rate_kbps = (tx_delta * 8.0) / (elapsed * 1000.0);
                interface_stats[idx].rx_rate_kbps = (rx_delta * 8.0) / (elapsed * 1000.0);
            }
            interface_stats[idx].tx_bytes_last = tx_bytes;
            interface_stats[idx].rx_bytes_last = rx_bytes;
            interface_stats[idx].last_update_time = now;
        } else if (interface_stats_count < MAX_INTERFACES) {
            /* New interface */
            idx = interface_stats_count++;
            strcpy(interface_stats[idx].name, ifname);
            interface_stats[idx].tx_bytes_last = tx_bytes;
            interface_stats[idx].rx_bytes_last = rx_bytes;
            interface_stats[idx].last_update_time = now;
            interface_stats[idx].tx_rate_kbps = 0;
            interface_stats[idx].rx_rate_kbps = 0;
        }
    }
    pclose(fp);
    
    pthread_mutex_unlock(&interface_stats_mutex);
    return 0;
}

static void broadcast_network_stats(void) {
    if (!ws_context) return;
    
    update_interface_stats();
    
    pthread_mutex_lock(&interface_stats_mutex);
    
    /* Build JSON with current rates */
    char json_data[4096];
    char *pos = json_data;
    int remaining = sizeof(json_data);
    int written = snprintf(pos, remaining, "{\"interfaces\":[");
    pos += written;
    remaining -= written;
    
    int first = 1;
    for (int i = 0; i < interface_stats_count; i++) {
        if (!first) {
            written = snprintf(pos, remaining, ",");
            pos += written;
            remaining -= written;
        }
        
        written = snprintf(pos, remaining,
                   "{\"name\":\"%s\",\"tx_rate_kbps\":%.2f,\"rx_rate_kbps\":%.2f}",
                   interface_stats[i].name,
                   interface_stats[i].tx_rate_kbps,
                   interface_stats[i].rx_rate_kbps);
        pos += written;
        remaining -= written;
        
        first = 0;
    }
    
    written = snprintf(pos, remaining, "]}");
    
    pthread_mutex_unlock(&interface_stats_mutex);
    
    broadcast_ws_message(json_data);
}

static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len) {
    (void)user;
    (void)in;
    (void)len;

    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED:
            pthread_mutex_lock(&ws_clients_mutex);
            for (int i = 0; i < MAX_WS_CLIENTS; i++) {
                if (!ws_clients[i].wsi) {
                    ws_clients[i].wsi = wsi;
                    break;
                }
            }
            pthread_mutex_unlock(&ws_clients_mutex);
            break;
        case LWS_CALLBACK_CLOSED:
            pthread_mutex_lock(&ws_clients_mutex);
            for (int i = 0; i < MAX_WS_CLIENTS; i++) {
                if (ws_clients[i].wsi == wsi) {
                    ws_clients[i].wsi = NULL;
                    break;
                }
            }
            pthread_mutex_unlock(&ws_clients_mutex);
            break;
        default:
            break;
    }
    return 0;
}

static int handle_get_config(struct MHD_Connection *conn) {
    pthread_mutex_lock(&config_mutex);
    
    ltc_config_t config;
    config_load(config_file_path, &config);
    char *json = config_to_json_with_revision(&config, config_revision);
    if (!json) {
        pthread_mutex_unlock(&config_mutex);
        return send_response(conn, "{\"error\": \"Memory error\"}", 25,
                           MHD_HTTP_INTERNAL_SERVER_ERROR, "application/json");
    }
    
    int ret = send_response(conn, json, strlen(json), MHD_HTTP_OK, "application/json");
    
    free(json);
    pthread_mutex_unlock(&config_mutex);
    return ret;
}

static int handle_post_config(struct MHD_Connection *conn, const char *upload_data,
                              size_t *upload_data_size, void **con_cls) {
    struct connection_info *info = (struct connection_info *)*con_cls;
    
    // First call: initialize connection info
    if (!info) {
        info = malloc(sizeof(struct connection_info));
        if (!info) {
            return send_response(conn, "{\"error\": \"Memory error\"}", 26, 
                               MHD_HTTP_INTERNAL_SERVER_ERROR, "application/json");
        }
        info->post_data = NULL;
        info->post_data_size = 0;
        info->post_data_capacity = 0;
        *con_cls = info;
        return MHD_YES;  // Wait for POST data
    }
    
    // Accumulate POST data
    if (*upload_data_size > 0) {
        // Resize buffer if needed (with 1MB cap)
        if (info->post_data_size + *upload_data_size > 1048576) {
            free_connection_info(info);
            *con_cls = NULL;
            return send_response(conn, "{\"error\": \"POST data too large\"}", 35,
                               MHD_HTTP_CONTENT_TOO_LARGE, "application/json");
        }
        
        if (info->post_data_size + *upload_data_size > info->post_data_capacity) {
            size_t new_capacity = (info->post_data_capacity == 0) ? 4096 : info->post_data_capacity * 2;
            while (new_capacity < info->post_data_size + *upload_data_size) {
                new_capacity *= 2;
            }
            char *new_data = realloc(info->post_data, new_capacity);
            if (!new_data) {
                free_connection_info(info);
                *con_cls = NULL;
                return send_response(conn, "{\"error\": \"Memory error\"}", 26,
                                   MHD_HTTP_INTERNAL_SERVER_ERROR, "application/json");
            }
            info->post_data = new_data;
            info->post_data_capacity = new_capacity;
        }
        
        memcpy(info->post_data + info->post_data_size, upload_data, *upload_data_size);
        info->post_data_size += *upload_data_size;
        *upload_data_size = 0;
        return MHD_YES;  // Ready for more data
    }
    
    // Final call: process the complete POST data
    if (*upload_data_size == 0 && info->post_data_size > 0) {
        unsigned long long new_revision = 0;
        pthread_mutex_lock(&config_mutex);
        
        // Null-terminate the JSON
        if (info->post_data_capacity > info->post_data_size) {
            info->post_data[info->post_data_size] = '\0';
        } else {
            // This shouldn't happen, but handle it safely
            char *temp = realloc(info->post_data, info->post_data_size + 1);
            if (!temp) {
                pthread_mutex_unlock(&config_mutex);
                free_connection_info(info);
                *con_cls = NULL;
                return send_response(conn, "{\"error\": \"Memory error\"}", 26,
                                   MHD_HTTP_INTERNAL_SERVER_ERROR, "application/json");
            }
            info->post_data = temp;
            info->post_data[info->post_data_size] = '\0';
        }

        // Load current config, update from JSON, and save
        ltc_config_t config;
        config_load(config_file_path, &config);

        unsigned long long client_revision = 0;
        if (extract_config_revision(info->post_data, &client_revision) &&
            client_revision != config_revision) {
            char response[128];
            snprintf(response, sizeof(response),
                     "{\"error\":\"Config changed\",\"config_revision\":%llu}",
                     config_revision);
            pthread_mutex_unlock(&config_mutex);
            free_connection_info(info);
            *con_cls = NULL;
            return send_response(conn, response, strlen(response), MHD_HTTP_CONFLICT, "application/json");
        }
        
        // Store old values to detect changes
        char old_timezone[256];
        char old_ntp_server[256];
        strcpy(old_timezone, config.timezone);
        strcpy(old_ntp_server, config.ntp_server);
        
        config_from_json(info->post_data, &config);
        int save_result = config_save(config_file_path, &config);

        // If timezone changed, apply it via timedatectl
        if (save_result == 0 && strcmp(old_timezone, config.timezone) != 0) {
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "timedatectl set-timezone '%s' 2>&1", config.timezone);
            int tz_result = system(cmd);
            if (tz_result != 0) {
                fprintf(stderr, "[CONFIG] Warning: Failed to apply timezone %s (exit code %d)\n", 
                       config.timezone, tz_result);
            } else {
                fprintf(stderr, "[CONFIG] Timezone changed to %s\n", config.timezone);
            }
        }

        // If NTP server changed, update systemd-timesyncd config
        if (save_result == 0 && strcmp(old_ntp_server, config.ntp_server) != 0) {
            // Create systemd-timesyncd override drop-in with custom NTP server
            char cmd[512];
            snprintf(cmd, sizeof(cmd), 
                    "mkdir -p /etc/systemd/timesyncd.conf.d && "
                    "echo '[Time]' > /etc/systemd/timesyncd.conf.d/99-custom-ntp.conf && "
                    "echo 'NTP=%s' >> /etc/systemd/timesyncd.conf.d/99-custom-ntp.conf && "
                    "systemctl restart systemd-timesyncd 2>&1",
                    config.ntp_server);
            int ntp_result = system(cmd);
            if (ntp_result != 0) {
                fprintf(stderr, "[CONFIG] Warning: Failed to apply NTP server %s (exit code %d)\n", 
                       config.ntp_server, ntp_result);
            } else {
                fprintf(stderr, "[CONFIG] NTP server changed to %s\n", config.ntp_server);
            }
        }

        if (save_result == 0) {
            config_revision++;
            new_revision = config_revision;
        }

        pthread_mutex_unlock(&config_mutex);
        free_connection_info(info);
        *con_cls = NULL;
        
        if (save_result == 0) {
            broadcast_config_update(new_revision);
            char response[128];
            snprintf(response, sizeof(response),
                     "{\"status\":\"saved\",\"config_revision\":%llu}", new_revision);
            return send_response(conn, response, strlen(response), MHD_HTTP_OK, "application/json");
        } else {
            return send_response(conn, "{\"error\": \"Failed to save config\"}", 35,
                               MHD_HTTP_INTERNAL_SERVER_ERROR, "application/json");
        }
    }
    
    // Empty POST or already processed
    if (*upload_data_size == 0 && info->post_data_size == 0) {
        free_connection_info(info);
        *con_cls = NULL;
        return send_response(conn, "{\"error\": \"Empty POST data\"}", 29, MHD_HTTP_BAD_REQUEST, "application/json");
    }
    
    return MHD_YES;
}

// Timezone data embedded in binary
typedef struct {
    const char *regions[10];
    const char *zones[20];
} tzdata_t;

static const tzdata_t tzdata[] = {
    {{"Europe"}, {
        "Europe/London", "Europe/Dublin", "Europe/Berlin", "Europe/Paris", 
        "Europe/Amsterdam", "Europe/Brussels", "Europe/Vienna", "Europe/Prague",
        "Europe/Rome", "Europe/Madrid", "Europe/Athens", "Europe/Istanbul",
        "Europe/Moscow", "Europe/Lisbon", "Europe/Stockholm", "Europe/Oslo",
        "Europe/Zurich", "Europe/Budapest", "Europe/Warsaw", "Europe/Bucharest"
    }},
    {{"Asia"}, {
        "Asia/Tokyo", "Asia/Shanghai", "Asia/Hong_Kong", "Asia/Bangkok",
        "Asia/Singapore", "Asia/Kolkata", "Asia/Dubai", "Asia/Bangkok",
        "Asia/Manila", "Asia/Ho_Chi_Minh", "Asia/Jakarta", "Asia/Malaysia",
        "Asia/Seoul", "Asia/Taipei", "Asia/Almaty", "Asia/Tehran",
        "Asia/Karachi", "Asia/Kabul", "Asia/Yangon", "Asia/Phnom_Penh"
    }},
    {{"Americas"}, {
        "America/New_York", "America/Chicago", "America/Denver", "America/Los_Angeles",
        "America/Anchorage", "America/Toronto", "America/Mexico_City",
        "America/Argentina/Buenos_Aires", "America/Sao_Paulo", "America/Lima",
        "America/Caracas", "America/Puerto_Rico", "America/Jamaica", "America/Belize",
        "America/El_Salvador", "America/Guatemala", "America/Honduras", "America/Nicaragua",
        "America/Costa_Rica", "America/Panama"
    }},
    {{"Africa"}, {
        "Africa/Cairo", "Africa/Nairobi", "Africa/Lagos", "Africa/Johannesburg",
        "Africa/Casablanca", "Africa/Algiers", "Africa/Tunis", "Africa/Addis_Ababa",
        "Africa/Khartoum", "Africa/Dar_es_Salaam", "Africa/Kampala", "Africa/Accra",
        "Africa/Abidjan", "Africa/Freetown", "Africa/Dakar", "Africa/Monrovia",
        "Africa/Maputo", "Africa/Windhoek", "Africa/Gaborone", "Africa/Harare"
    }},
    {{"Pacific"}, {
        "Pacific/Auckland", "Pacific/Fiji", "Pacific/Honolulu", "Pacific/Pago_Pago",
        "Pacific/Kiritimati", "Pacific/Tongatapu", "Pacific/Nauru", "Pacific/Palau",
        "Pacific/Port_Moresby", "Pacific/Guam", "Pacific/Saipan", "Pacific/Truk",
        "Pacific/Ponape", "Pacific/Majuro", "Pacific/Kwajalein", "Pacific/Wake",
        "Pacific/Tarawa", "Pacific/Midway", "Pacific/Chatham", "Pacific/Gambier"
    }},
    {{"Australia"}, {
        "Australia/Sydney", "Australia/Melbourne", "Australia/Brisbane", "Australia/Perth",
        "Australia/Adelaide", "Australia/Darwin", "Australia/Hobart", "Australia/Canberra",
        "Australia/Eucla", "Australia/Lord_Howe", "Australia/Currie", "Australia/Lindeman",
        "Australia/Broken_Hill", "Pacific/Norfolk", "Indian/Christmas", "Indian/Cocos"
    }}
};

static int handle_get_tzdata(struct MHD_Connection *conn) {
    char response[8192] = "{\"regions\": {";
    
    for (int i = 0; i < 6; i++) {
        if (i > 0) strcat(response, ", ");
        strcat(response, "\"");
        strcat(response, tzdata[i].regions[0]);
        strcat(response, "\": [");
        
        for (int j = 0; j < 20 && tzdata[i].zones[j]; j++) {
            if (j > 0) strcat(response, ", ");
            strcat(response, "\"");
            strcat(response, tzdata[i].zones[j]);
            strcat(response, "\"");
        }
        strcat(response, "]");
    }
    strcat(response, "}}");
    
    return send_response(conn, response, strlen(response), MHD_HTTP_OK, "application/json");
}

static int handle_get_ntp_status(struct MHD_Connection *conn) {
    /* Query systemd-timesyncd for NTP sync status */
    FILE *fp = popen("timedatectl show-timesync 2>/dev/null", "r");
    if (!fp) {
        return send_response(conn, "{\"status\": \"error\", \"message\": \"Unable to query NTP status\"}", 62,
                           MHD_HTTP_INTERNAL_SERVER_ERROR, "application/json");
    }
    
    char line[256];
    char server_name[256] = "";
    char server_address[256] = "";
    char synchronized[32] = "";
    char offset[64] = "N/A";
    
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "ServerName=", 11) == 0) {
            copy_line_value(server_name, sizeof(server_name), line + 11);
        }
        else if (strncmp(line, "ServerAddress=", 14) == 0) {
            copy_line_value(server_address, sizeof(server_address), line + 14);
        }
        else if (strncmp(line, "Synchronized=", 13) == 0) {
            copy_line_value(synchronized, sizeof(synchronized), line + 13);
        }
        else if (strncmp(line, "Offset=", 7) == 0) {
            /* Extract offset value */
            char *val_start = line + 7;
            copy_line_value(offset, sizeof(offset), val_start);
        }
    }
    pclose(fp);
    
    /* Determine sync status */
    const char *status = "unknown";
    
    /* If Synchronized field is present, use it */
    if (strlen(synchronized) > 0) {
        if (strcmp(synchronized, "yes") == 0) {
            status = "synced";
        } else if (strcmp(synchronized, "no") == 0) {
            status = "error";
        }
    } else if (strlen(server_name) > 0 && strlen(server_address) > 0) {
        /* If ServerName is populated, assume synced (older systemd-timesyncd versions) */
        status = "synced";
    }
    
    /* Build JSON response */
    char response[512];
    snprintf(response, sizeof(response),
            "{\"status\": \"%s\", \"server\": \"%s\", \"server_ip\": \"%s\", \"offset\": \"%s\"}",
            status, server_name, server_address, offset);
    
    return send_response(conn, response, strlen(response), MHD_HTTP_OK, "application/json");
}

static int handle_get_ntp_servers(struct MHD_Connection *conn) {
    pthread_mutex_lock(&config_mutex);
    
    ltc_config_t config;
    config_load(config_file_path, &config);
    
    /* Build JSON response with built-in and custom servers */
    char response[4096];
    char *pos = response;
    int remaining = sizeof(response);
    
    int written = snprintf(pos, remaining, "{\"servers\": [");
    pos += written;
    remaining -= written;
    
    int first = 1;
    
    /* Add built-in stratum 0 servers */
    for (int i = 0; stratum0_servers[i] != NULL && remaining > 20; i++) {
        if (!first) {
            written = snprintf(pos, remaining, ", ");
            pos += written;
            remaining -= written;
        }
        written = snprintf(pos, remaining, "{\"name\": \"%s\", \"type\": \"builtin\"}", 
                          stratum0_servers[i]);
        pos += written;
        remaining -= written;
        first = 0;
    }
    
    /* Add custom servers from config */
    const char *custom = config.custom_ntp_servers;
    if (custom && strlen(custom) > 2) {
        /* Simple array parsing: look for quoted strings between [ and ] */
        const char *p = custom + 1;  /* Skip opening [ */
        while (*p && *p != ']' && remaining > 20) {
            if (*p == '"') {
                p++;
                char server_name[256] = "";
                int idx = 0;
                while (*p && *p != '"' && idx < 255) {
                    server_name[idx++] = *p++;
                }
                server_name[idx] = 0;
                
                if (!first) {
                    written = snprintf(pos, remaining, ", ");
                    pos += written;
                    remaining -= written;
                }
                written = snprintf(pos, remaining, "{\"name\": \"%s\", \"type\": \"custom\"}", 
                                  server_name);
                pos += written;
                remaining -= written;
                first = 0;
                
                if (*p == '"') p++;
            }
            p++;
        }
    }
    
    written = snprintf(pos, remaining, "], \"current\": \"%s\"}", config.ntp_server);
    pos += written;
    
    pthread_mutex_unlock(&config_mutex);
    
    return send_response(conn, response, strlen(response), MHD_HTTP_OK, "application/json");
}

static int is_allowed_broadcast_resolution(uint32_t width, uint32_t height) {
    return (width == 1280 && height == 720) ||
           (width == 1920 && height == 1080) ||
           (width == 3840 && height == 2160);
}

static float mode_refresh_hz(const drmModeModeInfo *mode) {
    if (mode->clock > 0 && mode->htotal > 0 && mode->vtotal > 0) {
        return (float)mode->clock * 1000.0f / ((float)mode->htotal * (float)mode->vtotal);
    }
    return (float)mode->vrefresh;
}

static int handle_get_display_modes(struct MHD_Connection *conn) {
    /* Query DRM device for available display modes */
    int fd = -1;
    char response[16384];
    
    /* Open DRM device */
    for (int i = 0; i < 16; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        
        fd = open(path, O_RDWR);
        if (fd < 0) continue;
        
        /* Test if this device has DRM resources */
        drmModeRes *res = drmModeGetResources(fd);
        if (!res) {
            close(fd);
            fd = -1;
            continue;
        }
        drmModeFreeResources(res);
        break;
    }
    
    if (fd < 0) {
        snprintf(response, sizeof(response), 
                "{\"error\": \"Failed to open DRM device\", \"modes\": []}");
        return send_response(conn, response, strlen(response), 
                           MHD_HTTP_INTERNAL_SERVER_ERROR, "application/json");
    }
    
    /* Find HDMI connector */
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        close(fd);
        snprintf(response, sizeof(response), 
                "{\"error\": \"Failed to get DRM resources\", \"modes\": []}");
        return send_response(conn, response, strlen(response), 
                           MHD_HTTP_INTERNAL_SERVER_ERROR, "application/json");
    }
    
    drmModeConnector *conn_drm = NULL;
    
    for (int i = 0; i < res->count_connectors; i++) {
        conn_drm = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn_drm) continue;
        
        if ((conn_drm->connector_type == DRM_MODE_CONNECTOR_HDMIA ||
             conn_drm->connector_type == DRM_MODE_CONNECTOR_HDMIB) &&
            conn_drm->connection == DRM_MODE_CONNECTED) {
            break;
        }
        drmModeFreeConnector(conn_drm);
        conn_drm = NULL;
    }
    
    if (!conn_drm) {
        drmModeFreeResources(res);
        close(fd);
        snprintf(response, sizeof(response), 
                "{\"error\": \"No connected HDMI display found\", \"modes\": []}");
        return send_response(conn, response, strlen(response), 
                           MHD_HTTP_NOT_FOUND, "application/json");
    }
    
    /* Collect DRM modes, then select only LTC-relevant CEA-nearest timings */
    typedef struct {
        uint32_t width;
        uint32_t height;
        float refresh;
        int is_preferred;
    } mode_entry_t;

    mode_entry_t all_modes[512];
    int all_modes_count = 0;

    static const float target_rates[] = {
        24.00f, 25.00f, 30.00f,
        48.00f, 50.00f, 59.94f, 60.00f
    };
    const float pick_tolerance_hz = 0.25f;

    /* Collect all modes from connector */
    for (int i = 0; i < conn_drm->count_modes && all_modes_count < 512; i++) {
        drmModeModeInfo *mode = &conn_drm->modes[i];

        if (!is_allowed_broadcast_resolution(mode->hdisplay, mode->vdisplay)) {
            continue;
        }

        float actual_refresh = mode_refresh_hz(mode);
        int is_preferred = (mode->type & DRM_MODE_TYPE_PREFERRED) ? 1 : 0;

        /* De-duplicate exact mode entries */
        int duplicate = 0;
        for (int j = 0; j < all_modes_count; j++) {
            if (all_modes[j].width == mode->hdisplay &&
                all_modes[j].height == mode->vdisplay &&
                fabsf(all_modes[j].refresh - actual_refresh) < 0.01f) {
                duplicate = 1;
                break;
            }
        }

        if (!duplicate) {
            all_modes[all_modes_count].width = mode->hdisplay;
            all_modes[all_modes_count].height = mode->vdisplay;
            all_modes[all_modes_count].refresh = actual_refresh;
            all_modes[all_modes_count].is_preferred = is_preferred;
            all_modes_count++;
        }
    }

    /* Build unique resolution list, sorted smallest to largest */
    uint32_t resolutions[128][2];
    int resolution_count = 0;

    for (int i = 0; i < all_modes_count && resolution_count < 128; i++) {
        int duplicate = 0;
        for (int j = 0; j < resolution_count; j++) {
            if (resolutions[j][0] == all_modes[i].width &&
                resolutions[j][1] == all_modes[i].height) {
                duplicate = 1;
                break;
            }
        }
        if (!duplicate) {
            resolutions[resolution_count][0] = all_modes[i].width;
            resolutions[resolution_count][1] = all_modes[i].height;
            resolution_count++;
        }
    }

    for (int i = 0; i < resolution_count - 1; i++) {
        for (int j = i + 1; j < resolution_count; j++) {
            uint64_t area_i = (uint64_t)resolutions[i][0] * (uint64_t)resolutions[i][1];
            uint64_t area_j = (uint64_t)resolutions[j][0] * (uint64_t)resolutions[j][1];
            if (area_j < area_i) {
                uint32_t tmp_w = resolutions[i][0];
                uint32_t tmp_h = resolutions[i][1];
                resolutions[i][0] = resolutions[j][0];
                resolutions[i][1] = resolutions[j][1];
                resolutions[j][0] = tmp_w;
                resolutions[j][1] = tmp_h;
            }
        }
    }

    mode_entry_t selected_modes[512];
    int selected_modes_count = 0;

    for (int r = 0; r < resolution_count; r++) {
        uint32_t width = resolutions[r][0];
        uint32_t height = resolutions[r][1];

        for (size_t t = 0; t < sizeof(target_rates) / sizeof(target_rates[0]); t++) {
            float target = target_rates[t];
            int best_index = -1;
            float best_delta = 999.0f;

            for (int i = 0; i < all_modes_count; i++) {
                if (all_modes[i].width != width || all_modes[i].height != height) {
                    continue;
                }
                float delta = fabsf(all_modes[i].refresh - target);
                if (delta < best_delta) {
                    best_delta = delta;
                    best_index = i;
                }
            }

            if (best_index >= 0 && best_delta <= pick_tolerance_hz && selected_modes_count < 512) {
                int duplicate = 0;
                for (int i = 0; i < selected_modes_count; i++) {
                    if (selected_modes[i].width == all_modes[best_index].width &&
                        selected_modes[i].height == all_modes[best_index].height &&
                        fabsf(selected_modes[i].refresh - all_modes[best_index].refresh) < 0.01f) {
                        duplicate = 1;
                        break;
                    }
                }
                if (!duplicate) {
                    selected_modes[selected_modes_count++] = all_modes[best_index];
                }
            }
        }
    }

    /* Emit only selected real DRM-backed combinations */
    char *pos = response;
    int remaining = sizeof(response);
    int written = snprintf(pos, remaining, "{\"modes\":[");
    pos += written;
    remaining -= written;
    
    int emitted = 0;
    int preferred_index = -1;

    for (int i = 0; i < selected_modes_count; i++) {
        if (remaining <= 100 || emitted >= 512) {
            break;
        }

        if (emitted > 0) {
            written = snprintf(pos, remaining, ", ");
            pos += written;
            remaining -= written;
        }

        written = snprintf(pos, remaining,
                           "{\"width\":%u,\"height\":%u,\"refresh\":%.2f,\"preferred\":%s}",
                           selected_modes[i].width,
                           selected_modes[i].height,
                           selected_modes[i].refresh,
                           selected_modes[i].is_preferred ? "true" : "false");
        pos += written;
        remaining -= written;

        if (selected_modes[i].is_preferred && preferred_index < 0) {
            preferred_index = emitted;
        }
        emitted++;
    }

    if (preferred_index >= 0) {
        written = snprintf(pos, remaining, "],\"preferred_index\":%d}", preferred_index);
    } else {
        written = snprintf(pos, remaining, "]}");
    }
    
    drmModeFreeConnector(conn_drm);
    drmModeFreeResources(res);
    close(fd);
    
    return send_response(conn, response, strlen(response), MHD_HTTP_OK, "application/json");
}

#define MAX_HDMI_PORTS_REPORTED 8

typedef struct {
    char name[32];
    uint32_t width;
    uint32_t height;
    float refresh;
} hdmi_port_info_t;

/* Scans every /dev/dri/cardN with usable KMS resources for currently
 * CONNECTED HDMI-type connectors. DSI and HDMI can live on entirely
 * separate DRM devices on Pi5/CM5-class hardware (RP1's own DSI controller
 * vs the main SoC's vc4-drm), so this can't stop at the first working card
 * the way handle_get_display_modes does. */
static int scan_connected_hdmi_ports(hdmi_port_info_t *ports, int max_ports) {
    int found = 0;
    for (int i = 0; i < 16 && found < max_ports; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR);
        if (fd < 0) continue;

        drmModeRes *res = drmModeGetResources(fd);
        if (!res) {
            close(fd);
            continue;
        }

        for (int c = 0; c < res->count_connectors && found < max_ports; c++) {
            drmModeConnector *conn_drm = drmModeGetConnector(fd, res->connectors[c]);
            if (!conn_drm) continue;

            if ((conn_drm->connector_type == DRM_MODE_CONNECTOR_HDMIA ||
                 conn_drm->connector_type == DRM_MODE_CONNECTOR_HDMIB) &&
                conn_drm->connection == DRM_MODE_CONNECTED &&
                conn_drm->count_modes > 0) {
                drm_connector_name(conn_drm->connector_type, conn_drm->connector_type_id,
                                    ports[found].name, sizeof(ports[found].name));

                drmModeModeInfo *mode = &conn_drm->modes[0];
                for (int m = 0; m < conn_drm->count_modes; m++) {
                    if (conn_drm->modes[m].type & DRM_MODE_TYPE_PREFERRED) {
                        mode = &conn_drm->modes[m];
                        break;
                    }
                }
                ports[found].width = mode->hdisplay;
                ports[found].height = mode->vdisplay;
                ports[found].refresh = mode_refresh_hz(mode);
                found++;
            }
            drmModeFreeConnector(conn_drm);
        }

        drmModeFreeResources(res);
        close(fd);
    }
    return found;
}

static int handle_get_hdmi_ports(struct MHD_Connection *conn) {
    hdmi_port_info_t ports[MAX_HDMI_PORTS_REPORTED];
    int count = scan_connected_hdmi_ports(ports, MAX_HDMI_PORTS_REPORTED);

    char response[2048];
    char *pos = response;
    int remaining = sizeof(response);
    int written = snprintf(pos, remaining, "{\"ports\":[");
    pos += written;
    remaining -= written;

    for (int i = 0; i < count; i++) {
        written = snprintf(pos, remaining,
                           "%s{\"name\":\"%s\",\"width\":%u,\"height\":%u,\"refresh\":%.2f}",
                           i > 0 ? "," : "",
                           ports[i].name, ports[i].width, ports[i].height, ports[i].refresh);
        pos += written;
        remaining -= written;
    }
    snprintf(pos, remaining, "]}");

    return send_response(conn, response, strlen(response), MHD_HTTP_OK, "application/json");
}

static char last_hdmi_ports_signature[512] = "";

/* Called periodically from broadcast_thread_func (same thread/cadence as
 * broadcast_network_stats). Only pushes a WS message when the connected
 * HDMI port set actually changes, so the web UI's per-port position
 * sections update live without polling. */
static void broadcast_hdmi_ports_if_changed(void) {
    hdmi_port_info_t ports[MAX_HDMI_PORTS_REPORTED];
    int count = scan_connected_hdmi_ports(ports, MAX_HDMI_PORTS_REPORTED);

    char signature[512];
    char *sp = signature;
    int sremaining = sizeof(signature);
    for (int i = 0; i < count; i++) {
        int written = snprintf(sp, sremaining, "%s:%ux%u@%.2f;",
                               ports[i].name, ports[i].width, ports[i].height, ports[i].refresh);
        if (written < 0 || written >= sremaining) break;
        sp += written;
        sremaining -= written;
    }

    if (strcmp(signature, last_hdmi_ports_signature) == 0) {
        return;
    }
    strncpy(last_hdmi_ports_signature, signature, sizeof(last_hdmi_ports_signature) - 1);
    last_hdmi_ports_signature[sizeof(last_hdmi_ports_signature) - 1] = '\0';

    char json_data[2048];
    char *jp = json_data;
    int jremaining = sizeof(json_data);
    int written = snprintf(jp, jremaining, "{\"type\":\"hdmi_ports\",\"ports\":[");
    jp += written;
    jremaining -= written;
    for (int i = 0; i < count; i++) {
        written = snprintf(jp, jremaining,
                           "%s{\"name\":\"%s\",\"width\":%u,\"height\":%u,\"refresh\":%.2f}",
                           i > 0 ? "," : "",
                           ports[i].name, ports[i].width, ports[i].height, ports[i].refresh);
        jp += written;
        jremaining -= written;
    }
    snprintf(jp, jremaining, "]}");

    broadcast_ws_message(json_data);
}

static int handle_post_network_vlan(struct MHD_Connection *conn, const char *upload_data,
                                    size_t *upload_data_size, void **con_cls) {
    struct connection_info *con_info = *con_cls;
    
    if (con_info == NULL) {
        con_info = calloc(1, sizeof(struct connection_info));
        if (!con_info) {
            return MHD_NO;
        }
        con_info->post_data = NULL;
        con_info->post_data_size = 0;
        con_info->post_data_capacity = 0;
        *con_cls = con_info;
        return MHD_YES;
    }
    
    if (*upload_data_size > 0) {
        if (con_info->post_data_size + *upload_data_size + 1 > con_info->post_data_capacity) {
            size_t new_capacity = con_info->post_data_capacity + *upload_data_size + 1024;
            char *new_data = realloc(con_info->post_data, new_capacity);
            if (!new_data) {
                return MHD_NO;
            }
            con_info->post_data = new_data;
            con_info->post_data_capacity = new_capacity;
        }
        
        memcpy(con_info->post_data + con_info->post_data_size, upload_data, *upload_data_size);
        con_info->post_data_size += *upload_data_size;
        con_info->post_data[con_info->post_data_size] = '\0';
        *upload_data_size = 0;
        return MHD_YES;
    }
    
    if (!con_info->post_data) {
        return send_response(conn, "{\"error\":\"No data received\"}", 29,
                           MHD_HTTP_BAD_REQUEST, "application/json");
    }
    
    /* Check if this is an action-based request (e.g., disable interface) */
    if (strstr(con_info->post_data, "\"action\"") != NULL) {
        /* Simple action parser */
        char action[32] = {0};
        char interface[64] = {0};
        
        const char *action_pos = strstr(con_info->post_data, "\"action\"");
        if (action_pos) {
            const char *val_start = strchr(action_pos + 8, '\"');
            if (val_start) {
                val_start++;
                const char *val_end = strchr(val_start, '\"');
                if (val_end) {
                    size_t value_len = (size_t)(val_end - val_start);
                    if (value_len < sizeof(action)) {
                        memcpy(action, val_start, value_len);
                        action[value_len] = '\0';
                    }
                }
            }
        }
        
        const char *iface_pos = strstr(con_info->post_data, "\"interface\"");
        if (iface_pos) {
            const char *val_start = strchr(iface_pos + 11, '\"');
            if (val_start) {
                val_start++;
                const char *val_end = strchr(val_start, '\"');
                if (val_end) {
                    size_t value_len = (size_t)(val_end - val_start);
                    if (value_len < sizeof(interface)) {
                        memcpy(interface, val_start, value_len);
                        interface[value_len] = '\0';
                    }
                }
            }
        }
        
        if (strcmp(action, "disable") == 0 && strlen(interface) > 0) {
            /* Disable interface by bringing down and optionally deleting connection */
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "sudo nmcli device disconnect %s 2>&1", interface);
            system(cmd);
            
            /* For WiFi, also disable autoconnect */
            if (strncmp(interface, "wlan", 4) == 0 || strncmp(interface, "wlp", 3) == 0) {
                char conn_cmd[512];
                snprintf(conn_cmd, sizeof(conn_cmd), 
                        "sudo nmcli -t -f NAME connection show | xargs -I {} sudo nmcli connection modify {} connection.autoconnect no 2>&1");
                system(conn_cmd);
            }
            
            return send_response(conn, "{\"status\":\"success\"}", 21, MHD_HTTP_OK, "application/json");
        }
        
        return send_response(conn, "{\"error\":\"Invalid action\"}", 27,
                           MHD_HTTP_BAD_REQUEST, "application/json");
    }
    
    /* Apply VLAN configuration using nmcli */
    ltc_config_t config;
    if (config_from_json(con_info->post_data, &config) != 0) {
        return send_response(conn, "{\"error\":\"Invalid JSON\"}", 25,
                           MHD_HTTP_BAD_REQUEST, "application/json");
    }
    
    /* Apply VLAN 1 configuration if enabled */
    if (config.admin_vlan_enabled) {
        char cmd[1024];
        
        /* Check if VLAN connection already exists */
        int exists = system("nmcli connection show eth0.1 >/dev/null 2>&1") == 0;
        
        if (exists) {
            /* Modify existing VLAN */
            snprintf(cmd, sizeof(cmd),
                    "sudo nmcli connection modify eth0.1 ipv4.addresses '%s' ipv4.method manual",
                    config.admin_vlan_ip);
            
            if (strlen(config.admin_vlan_gateway) > 0) {
                char gw_cmd[256];
                snprintf(gw_cmd, sizeof(gw_cmd), " ipv4.gateway '%s'", config.admin_vlan_gateway);
                strncat(cmd, gw_cmd, sizeof(cmd) - strlen(cmd) - 1);
            }
            
            if (system(cmd) != 0) {
                return send_response(conn, "{\"error\":\"Failed to modify VLAN\"}", 32,
                                   MHD_HTTP_INTERNAL_SERVER_ERROR, "application/json");
            }
            
            /* Bring connection up */
            system("sudo nmcli connection up eth0.1 2>&1");
        } else {
            /* Create new VLAN connection */
            snprintf(cmd, sizeof(cmd),
                    "sudo nmcli connection add type vlan con-name eth0.1 ifname eth0.1 dev eth0 id 1 "
                    "ipv4.addresses '%s' ipv4.method manual",
                    config.admin_vlan_ip);
            
            if (strlen(config.admin_vlan_gateway) > 0) {
                char gw_cmd[256];
                snprintf(gw_cmd, sizeof(gw_cmd), " ipv4.gateway '%s'", config.admin_vlan_gateway);
                strncat(cmd, gw_cmd, sizeof(cmd) - strlen(cmd) - 1);
            }
            
            if (system(cmd) != 0) {
                return send_response(conn, "{\"error\":\"Failed to create VLAN\"}", 32,
                                   MHD_HTTP_INTERNAL_SERVER_ERROR, "application/json");
            }
        }
    } else {
        /* Disable VLAN - bring down and delete */
        system("sudo nmcli connection down eth0.1 2>&1");
        system("sudo nmcli connection delete eth0.1 2>&1");
    }
    
    /* Save configuration */
    pthread_mutex_lock(&config_mutex);
    int save_result = config_save(config_file_path, &config);
    pthread_mutex_unlock(&config_mutex);
    
    if (save_result != 0) {
        return send_response(conn, "{\"error\":\"Failed to save config\"}", 33,
                           MHD_HTTP_INTERNAL_SERVER_ERROR, "application/json");
    }
    
    return send_response(conn, "{\"status\":\"success\"}", 21, MHD_HTTP_OK, "application/json");
}

static int request_handler(void *cls, struct MHD_Connection *conn,
                          const char *url, const char *method,
                          const char *version, const char *upload_data,
                          size_t *upload_data_size, void **con_cls) {
    (void) cls; (void) version;
    
    if (strcmp(url, "/") == 0) {
        return send_response(conn, web_index_html, web_index_html_size, 
                           MHD_HTTP_OK, "text/html; charset=utf-8");
    }
    if (strcmp(url, "/styles.css") == 0) {
        return send_response(conn, web_styles_css, web_styles_css_size,
                           MHD_HTTP_OK, "text/css");
    }
    if (strcmp(url, "/app.js") == 0) {
        return send_response(conn, web_app_js, web_app_js_size,
                           MHD_HTTP_OK, "application/javascript");
    }
    if (strcmp(url, "/api/config") == 0) {
        if (strcmp(method, "GET") == 0) {
            return handle_get_config(conn);
        }
        if (strcmp(method, "POST") == 0) {
            return handle_post_config(conn, upload_data, upload_data_size, con_cls);
        }
    }
    if (strcmp(url, "/api/tzdata") == 0) {
        return handle_get_tzdata(conn);
    }
    if (strcmp(url, "/api/ntp-status") == 0) {
        return handle_get_ntp_status(conn);
    }
    if (strcmp(url, "/api/ntp-servers") == 0) {
        return handle_get_ntp_servers(conn);
    }
    if (strcmp(url, "/api/display-modes") == 0) {
        return handle_get_display_modes(conn);
    }
    if (strcmp(url, "/api/hdmi-ports") == 0) {
        return handle_get_hdmi_ports(conn);
    }
    if (strcmp(url, "/api/network/vlan") == 0) {
        if (strcmp(method, "POST") == 0) {
            return handle_post_network_vlan(conn, upload_data, upload_data_size, con_cls);
        }
    }
    
    return send_response(conn, "Not Found", 9, MHD_HTTP_NOT_FOUND, "text/plain");
}

static void request_completed(void *cls, struct MHD_Connection *conn,
                            void **con_cls, enum MHD_RequestTerminationCode code) {
    (void) cls; (void) conn; (void) code;
    
    if (*con_cls) {
        free_connection_info((struct connection_info *)*con_cls);
        *con_cls = NULL;
    }
}

static volatile int ws_running = 0;

static void *ws_service_thread_func(void *arg) {
    struct lws_context *context = (struct lws_context *)arg;
    while (ws_running) {
        lws_service(context, 50);
    }
    return NULL;
}

static void *broadcast_thread_func(void *arg) {
    (void)arg;
    while (ws_running) {
        sleep(2);
        if (ws_running) {
            broadcast_network_stats();
            broadcast_hdmi_ports_if_changed();
        }
    }
    return NULL;
}

int config_service_start(int port, const char *config_path) {
    strncpy(config_file_path, config_path, sizeof(config_file_path) - 1);
    
    daemon = MHD_start_daemon(
        MHD_USE_SELECT_INTERNALLY,
        port,
        NULL, 
        NULL,
        (MHD_AccessHandlerCallback)&request_handler, 
        NULL,
        MHD_OPTION_NOTIFY_COMPLETED, &request_completed, NULL,
        MHD_OPTION_END);
    
    if (!daemon) return -1;
    
    fprintf(stderr, "Config service started on port %d\n", port);
    
    /* Start WebSocket server on port+1 */
    struct lws_context_creation_info info = {};
    info.port = port + 1;
    info.protocols = (struct lws_protocols[]) {
        {
            "network-stream",
            ws_callback,
            0,
            1024,
            0, NULL, 0
        },
        { NULL, NULL, 0, 0, 0, NULL, 0 }
    };
    info.gid = -1;
    info.uid = -1;
    
    ws_context = lws_create_context(&info);
    if (!ws_context) {
        fprintf(stderr, "Failed to create WebSocket context\n");
        return -1;
    }
    
    fprintf(stderr, "WebSocket server started on port %d\n", port + 1);
    
    /* Start WebSocket service thread */
    pthread_t ws_thread_id;
    if (pthread_create(&ws_thread_id, NULL, ws_service_thread_func, (void *)ws_context) != 0) {
        fprintf(stderr, "Failed to create WebSocket service thread\n");
        ws_running = 0;
        lws_context_destroy(ws_context);
        ws_context = NULL;
        return -1;
    }
    pthread_detach(ws_thread_id);
    
    /* Start broadcast thread */
    ws_running = 1;
    pthread_t broadcast_thread_id;
    if (pthread_create(&broadcast_thread_id, NULL, broadcast_thread_func, NULL) != 0) {
        ws_running = 0;
        lws_context_destroy(ws_context);
        ws_context = NULL;
        return -1;
    }
    
    return 0;
}

void config_service_stop(void) {
    if (daemon) {
        MHD_stop_daemon(daemon);
        daemon = NULL;
    }
    
    ws_running = 0;
    if (ws_context) {
        lws_context_destroy(ws_context);
        ws_context = NULL;
    }
}
