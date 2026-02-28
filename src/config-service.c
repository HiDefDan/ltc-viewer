#define _POSIX_C_SOURCE 200809L
#include "config-service.h"
#include "config-management.h"
#include "config.h"
#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>

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

static int handle_get_config(struct MHD_Connection *conn) {
    pthread_mutex_lock(&config_mutex);
    
    ltc_config_t config;
    config_load(config_file_path, &config);
    char *json = config_to_json(&config);
    
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

        pthread_mutex_unlock(&config_mutex);
        free_connection_info(info);
        *con_cls = NULL;
        
        if (save_result == 0) {
            return send_response(conn, "{\"status\": \"saved\"}", 19, MHD_HTTP_OK, "application/json");
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
            strncpy(server_name, line + 11, sizeof(server_name) - 1);
            server_name[strcspn(server_name, "\n")] = 0;
        }
        else if (strncmp(line, "ServerAddress=", 14) == 0) {
            strncpy(server_address, line + 14, sizeof(server_address) - 1);
            server_address[strcspn(server_address, "\n")] = 0;
        }
        else if (strncmp(line, "Synchronized=", 13) == 0) {
            strncpy(synchronized, line + 13, sizeof(synchronized) - 1);
            synchronized[strcspn(synchronized, "\n")] = 0;
        }
        else if (strncmp(line, "Offset=", 7) == 0) {
            /* Extract offset value */
            char *val_start = line + 7;
            strncpy(offset, val_start, sizeof(offset) - 1);
            offset[strcspn(offset, "\n")] = 0;
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
    
    /* First pass: collect all unique resolutions and refresh rates */
    typedef struct {
        uint32_t width;
        uint32_t height;
        float refresh;
        int is_preferred;
    } mode_entry_t;

    typedef struct {
        uint32_t width;
        uint32_t height;
    } resolution_t;

    typedef struct {
        float refresh;
    } refresh_rate_t;

    mode_entry_t all_modes[512];
    int all_modes_count = 0;
    resolution_t unique_resolutions[128];
    int unique_resolutions_count = 0;
    refresh_rate_t unique_refresh_rates[64];
    int unique_refresh_rates_count = 0;

    /* Collect all modes from connector */
    for (int i = 0; i < conn_drm->count_modes && all_modes_count < 512; i++) {
        drmModeModeInfo *mode = &conn_drm->modes[i];
        float actual_refresh = (float)mode->clock * 1000.0f / 
                               ((float)mode->htotal * (float)mode->vtotal);
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

    /* Extract unique resolutions */
    for (int i = 0; i < all_modes_count && unique_resolutions_count < 128; i++) {
        int duplicate = 0;
        for (int j = 0; j < unique_resolutions_count; j++) {
            if (unique_resolutions[j].width == all_modes[i].width &&
                unique_resolutions[j].height == all_modes[i].height) {
                duplicate = 1;
                break;
            }
        }
        if (!duplicate) {
            unique_resolutions[unique_resolutions_count].width = all_modes[i].width;
            unique_resolutions[unique_resolutions_count].height = all_modes[i].height;
            unique_resolutions_count++;
        }
    }

    /* Extract unique refresh rates globally (if 50Hz exists anywhere, offer at all resolutions) */
    for (int i = 0; i < all_modes_count && unique_refresh_rates_count < 64; i++) {
        int duplicate = 0;
        for (int j = 0; j < unique_refresh_rates_count; j++) {
            if (fabsf(unique_refresh_rates[j].refresh - all_modes[i].refresh) < 0.01f) {
                duplicate = 1;
                break;
            }
        }
        if (!duplicate) {
            unique_refresh_rates[unique_refresh_rates_count].refresh = all_modes[i].refresh;
            unique_refresh_rates_count++;
        }
    }

    /* Sort refresh rates descending */
    for (int i = 0; i < unique_refresh_rates_count - 1; i++) {
        for (int j = i + 1; j < unique_refresh_rates_count; j++) {
            if (unique_refresh_rates[j].refresh > unique_refresh_rates[i].refresh) {
                float tmp = unique_refresh_rates[i].refresh;
                unique_refresh_rates[i].refresh = unique_refresh_rates[j].refresh;
                unique_refresh_rates[j].refresh = tmp;
            }
        }
    }

    /* Second pass: emit all combinations of resolution × refresh rate */
    char *pos = response;
    int remaining = sizeof(response);
    int written = snprintf(pos, remaining, "{\"modes\":[");
    pos += written;
    remaining -= written;
    
    int emitted = 0;
    int preferred_index = -1;

    /* For each combination, find if it exists in all_modes and use its preferred flag */
    for (int r = 0; r < unique_resolutions_count; r++) {
        for (int f = 0; f < unique_refresh_rates_count; f++) {
            uint32_t width = unique_resolutions[r].width;
            uint32_t height = unique_resolutions[r].height;
            float refresh = unique_refresh_rates[f].refresh;

            /* Find mode entry for this combination (if it exists) */
            int is_preferred = 0;
            for (int m = 0; m < all_modes_count; m++) {
                if (all_modes[m].width == width &&
                    all_modes[m].height == height &&
                    fabsf(all_modes[m].refresh - refresh) < 0.01f) {
                    is_preferred = all_modes[m].is_preferred;
                    break;
                }
            }

            /* Always emit the mode (even if not in DRM list, monitor usually supports it) */
            if (remaining > 100 && emitted < 512) {
                if (emitted > 0) {
                    written = snprintf(pos, remaining, ", ");
                    pos += written;
                    remaining -= written;
                }

                written = snprintf(pos, remaining,
                                   "{\"width\":%u,\"height\":%u,\"refresh\":%.2f,\"preferred\":%s}",
                                   width, height, refresh,
                                   is_preferred ? "true" : "false");
                pos += written;
                remaining -= written;

                if (is_preferred && preferred_index < 0) {
                    preferred_index = emitted;
                }
                emitted++;
            }
        }
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
    return 0;
}

void config_service_stop(void) {
    if (daemon) {
        MHD_stop_daemon(daemon);
        daemon = NULL;
    }
}
