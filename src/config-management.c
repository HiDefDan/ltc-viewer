#include "config-management.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void config_default(ltc_config_t *config) {
    strcpy(config->timezone, "Europe/London");
    strcpy(config->ntp_server, "pool.ntp.org");
    strcpy(config->custom_ntp_servers, "[]");
    config->refresh_hz = 50;
    config->timecode_x = 333;      /* Original carefully calculated center position */
    config->timecode_y = 412;      /* Original carefully calculated center position */
    config->color_r = 64;
    config->color_g = 255;
    config->color_b = 64;
    config->bg_color_r = 0;
    config->bg_color_g = 0;
    config->bg_color_b = 0;
}

// Simple JSON string extraction helper
static int json_extract_string(const char *json, const char *key, char *value, size_t max_len) {
    char search_pattern[512];
    snprintf(search_pattern, sizeof(search_pattern), "\"%s\":", key);
    
    const char *pos = strstr(json, search_pattern);
    if (!pos) return -1;
    
    // Move past the key and colon
    pos = strchr(pos, ':') + 1;
    
    // Skip whitespace after colon
    while (*pos && isspace(*pos)) pos++;
    
    // Expect opening quote
    if (*pos != '"') return -1;
    pos++;  // Skip opening quote
    
    // Read string value until closing quote
    size_t i = 0;
    while (*pos && *pos != '"' && i < max_len - 1) {
        value[i++] = *pos++;
    }
    value[i] = '\0';
    return 0;
}

// Simple JSON integer extraction helper
static int json_extract_int(const char *json, const char *key, int *value) {
    char search_pattern[512];
    snprintf(search_pattern, sizeof(search_pattern), "\"%s\":", key);
    
    const char *pos = strstr(json, search_pattern);
    if (!pos) return -1;
    
    pos = strchr(pos, ':') + 1;
    while (*pos && isspace(*pos)) pos++;
    
    *value = atoi(pos);
    return 0;
}

int config_from_json(const char *json_str, ltc_config_t *config) {
    config_default(config);
    
    json_extract_string(json_str, "timezone", config->timezone, sizeof(config->timezone));
    json_extract_string(json_str, "ntp_server", config->ntp_server, sizeof(config->ntp_server));
    /* Extract custom_ntp_servers as JSON array (raw string) */
    const char *array_pos = strstr(json_str, "\"custom_ntp_servers\":");
    if (array_pos) {
        array_pos = strchr(array_pos, ':') + 1;
        while (*array_pos && isspace(*array_pos)) array_pos++;
        if (*array_pos == '[') {
            const char *array_end = strchr(array_pos, ']');
            if (array_end) {
                size_t len = array_end - array_pos + 1;
                if (len < sizeof(config->custom_ntp_servers)) {
                    strncpy(config->custom_ntp_servers, array_pos, len);
                    config->custom_ntp_servers[len] = '\0';
                }
            }
        }
    }
    json_extract_int(json_str, "refresh_hz", &config->refresh_hz);
    json_extract_int(json_str, "timecode_x", &config->timecode_x);
    json_extract_int(json_str, "timecode_y", &config->timecode_y);
    json_extract_int(json_str, "color_r", &config->color_r);
    json_extract_int(json_str, "color_g", &config->color_g);
    json_extract_int(json_str, "color_b", &config->color_b);
    json_extract_int(json_str, "bg_color_r", &config->bg_color_r);
    json_extract_int(json_str, "bg_color_g", &config->bg_color_g);
    json_extract_int(json_str, "bg_color_b", &config->bg_color_b);
    
    return 0;
}

char* config_to_json(const ltc_config_t *config) {
    char *json = malloc(3072);
    if (!json) return NULL;
    
    snprintf(json, 3072,
        "{\n"
        "  \"timezone\": \"%s\",\n"
        "  \"ntp_server\": \"%s\",\n"
        "  \"custom_ntp_servers\": %s,\n"
        "  \"refresh_hz\": %d,\n"
        "  \"timecode_x\": %d,\n"
        "  \"timecode_y\": %d,\n"
        "  \"color_r\": %d,\n"
        "  \"color_g\": %d,\n"
        "  \"color_b\": %d,\n"
        "  \"bg_color_r\": %d,\n"
        "  \"bg_color_g\": %d,\n"
        "  \"bg_color_b\": %d\n"
        "}\n",
        config->timezone,
        config->ntp_server,
        config->custom_ntp_servers,
        config->refresh_hz,
        config->timecode_x,
        config->timecode_y,
        config->color_r,
        config->color_g,
        config->color_b,
        config->bg_color_r,
        config->bg_color_g,
        config->bg_color_b
    );
    
    return json;
}

int config_save(const char *path, const ltc_config_t *config) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    
    char *json = config_to_json(config);
    if (!json) {
        fclose(f);
        return -1;
    }
    
    int ret = fputs(json, f) >= 0 ? 0 : -1;
    free(json);
    fclose(f);
    return ret;
}

int config_load(const char *path, ltc_config_t *config) {
    FILE *f = fopen(path, "r");
    if (!f) {
        config_default(config);
        return -1;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0 || size > 10000) {
        fclose(f);
        config_default(config);
        return -1;
    }
    
    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        config_default(config);
        return -1;
    }
    
    size_t read = fread(json, 1, size, f);
    fclose(f);
    
    json[read] = '\0';
    int ret = config_from_json(json, config);
    
    free(json);
    
    return ret;
}
