#include "config-management.h"
#include "config.h"
#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void clamp_config_values(ltc_config_t *config) {
    if (config->display_width <= 0 || config->display_width > 7680) {
        config->display_width = DISPLAY_WIDTH;
    }
    if (config->display_height <= 0 || config->display_height > 4320) {
        config->display_height = DISPLAY_HEIGHT;
    }

    float scale_x = (float)config->display_width / (float)DISPLAY_WIDTH;
    float scale_y = (float)config->display_height / (float)DISPLAY_HEIGHT;
    float scale = (scale_x < scale_y) ? scale_x : scale_y;
    if (scale <= 0.0f) {
        scale = 1.0f;
    }

    int scaled_glyph_height = (int)(FONT_GLYPH_HEIGHT * scale + 0.5f);
    if (scaled_glyph_height < 1) {
        scaled_glyph_height = 1;
    }
    if (scaled_glyph_height > config->display_height) {
        scaled_glyph_height = config->display_height;
    }

    /* Allow Y offset up to ±(height - glyph_height) / 2 to keep text in bounds */
    int max_y_offset = (config->display_height - scaled_glyph_height) / 2;
    if (max_y_offset < 0) max_y_offset = 0;

    if (config->timecode_y_offset < -max_y_offset) {
        config->timecode_y_offset = -max_y_offset;
    }
    if (config->timecode_y_offset > max_y_offset) {
        config->timecode_y_offset = max_y_offset;
    }

    /*
     * Keep full background pattern in bounds.
     * Timecode is centered over 8 digits (HH.MM.SS.FF), while background is
     * "8.8.8.8.8.8.8.8." and starts one digit width to the left.
     */
    int scaled_digit_width = (int)(DISPLAY_DIGIT_WIDTH * scale + 0.5f);
    if (scaled_digit_width < 1) {
        scaled_digit_width = 1;
    }

    int scaled_timecode_width = 8 * scaled_digit_width;
    int center_x = (config->display_width > scaled_timecode_width) ?
                   ((config->display_width - scaled_timecode_width) / 2) : 0;

    int min_text_x = scaled_digit_width;
    int max_text_x = config->display_width - (9 * scaled_digit_width);

    int max_left = center_x - min_text_x;
    int max_right = max_text_x - center_x;

    int max_x_offset = (max_left < max_right) ? max_left : max_right;
    if (max_x_offset < 0) {
        max_x_offset = 0;
    }

    if (config->timecode_x_offset < -max_x_offset) {
        config->timecode_x_offset = -max_x_offset;
    }
    if (config->timecode_x_offset > max_x_offset) {
        config->timecode_x_offset = max_x_offset;
    }
}

void config_default(ltc_config_t *config) {
    strcpy(config->timezone, "Europe/London");
    strcpy(config->ntp_server, "pool.ntp.org");
    strcpy(config->custom_ntp_servers, "[]");
    config->display_width = DISPLAY_WIDTH;
    config->display_height = DISPLAY_HEIGHT;
    config->refresh_hz = TARGET_REFRESH_HZ;
    config->timecode_x_offset = 0; /* Centered horizontally */
    config->timecode_y_offset = 0; /* Centered vertically */
    config->color_r = 64;
    config->color_g = 255;
    config->color_b = 64;
    config->bg_color_r = 0;
    config->bg_color_g = 0;
    config->bg_color_b = 0;

    config->dsi_ltc_loss_behavior = LTC_LOSS_RESTORE_TOD;
    config->dsi_ltc_loss_timeout_sec = 3;
    config->hdmi_ltc_loss_behavior = LTC_LOSS_RESTORE_TOD;
    config->hdmi_ltc_loss_timeout_sec = 3;
    config->debug_overlay_enabled = 0;
    
    /* Network defaults */
    config->admin_vlan_enabled = 0;
    strcpy(config->admin_vlan_ip, "192.168.1.100/24");
    strcpy(config->admin_vlan_gateway, "");
    strcpy(config->web_ui_bind_address, "0.0.0.0");
    config->web_ui_bind_port = 8080;
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

static void clamp_ltc_loss_behavior(ltc_config_t *config) {
    if (config->dsi_ltc_loss_behavior != LTC_LOSS_HOLD_LAST_FRAME &&
        config->dsi_ltc_loss_behavior != LTC_LOSS_RESTORE_TOD) {
        config->dsi_ltc_loss_behavior = LTC_LOSS_HOLD_LAST_FRAME;
    }
    if (config->hdmi_ltc_loss_behavior != LTC_LOSS_HOLD_LAST_FRAME &&
        config->hdmi_ltc_loss_behavior != LTC_LOSS_RESTORE_TOD) {
        config->hdmi_ltc_loss_behavior = LTC_LOSS_RESTORE_TOD;
    }
    if (config->dsi_ltc_loss_timeout_sec < 0) {
        config->dsi_ltc_loss_timeout_sec = 0;
    }
    if (config->hdmi_ltc_loss_timeout_sec < 0) {
        config->hdmi_ltc_loss_timeout_sec = 0;
    }
}

// Simple JSON float extraction helper
static int json_extract_float(const char *json, const char *key, float *value) {
    char search_pattern[512];
    snprintf(search_pattern, sizeof(search_pattern), "\"%s\":", key);
    
    const char *pos = strstr(json, search_pattern);
    if (!pos) return -1;
    
    pos = strchr(pos, ':') + 1;
    while (*pos && isspace(*pos)) pos++;
    
    *value = atof(pos);
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
    json_extract_int(json_str, "display_width", &config->display_width);
    json_extract_int(json_str, "display_height", &config->display_height);
    json_extract_float(json_str, "refresh_hz", &config->refresh_hz);
    json_extract_int(json_str, "timecode_x_offset", &config->timecode_x_offset);
    json_extract_int(json_str, "timecode_y_offset", &config->timecode_y_offset);
    json_extract_int(json_str, "color_r", &config->color_r);
    json_extract_int(json_str, "color_g", &config->color_g);
    json_extract_int(json_str, "color_b", &config->color_b);
    json_extract_int(json_str, "bg_color_r", &config->bg_color_r);
    json_extract_int(json_str, "bg_color_g", &config->bg_color_g);
    json_extract_int(json_str, "bg_color_b", &config->bg_color_b);

    json_extract_int(json_str, "dsi_ltc_loss_behavior", &config->dsi_ltc_loss_behavior);
    json_extract_int(json_str, "dsi_ltc_loss_timeout_sec", &config->dsi_ltc_loss_timeout_sec);
    json_extract_int(json_str, "hdmi_ltc_loss_behavior", &config->hdmi_ltc_loss_behavior);
    json_extract_int(json_str, "hdmi_ltc_loss_timeout_sec", &config->hdmi_ltc_loss_timeout_sec);
    json_extract_int(json_str, "debug_overlay_enabled", &config->debug_overlay_enabled);
    
    /* Network settings */
    json_extract_int(json_str, "admin_vlan_enabled", &config->admin_vlan_enabled);
    json_extract_string(json_str, "admin_vlan_ip", config->admin_vlan_ip, sizeof(config->admin_vlan_ip));
    json_extract_string(json_str, "admin_vlan_gateway", config->admin_vlan_gateway, sizeof(config->admin_vlan_gateway));
    json_extract_string(json_str, "web_ui_bind_address", config->web_ui_bind_address, sizeof(config->web_ui_bind_address));
    json_extract_int(json_str, "web_ui_bind_port", &config->web_ui_bind_port);

    clamp_config_values(config);
    clamp_ltc_loss_behavior(config);
    
    return 0;
}

char* config_to_json(const ltc_config_t *config) {
    char *json = malloc(4096);
    if (!json) return NULL;
    
    snprintf(json, 4096,
        "{\n"
        "  \"timezone\": \"%s\",\n"
        "  \"ntp_server\": \"%s\",\n"
        "  \"custom_ntp_servers\": %s,\n"
        "  \"display_width\": %d,\n"
        "  \"display_height\": %d,\n"
        "  \"refresh_hz\": %.2f,\n"
        "  \"timecode_x_offset\": %d,\n"
        "  \"timecode_y_offset\": %d,\n"
        "  \"color_r\": %d,\n"
        "  \"color_g\": %d,\n"
        "  \"color_b\": %d,\n"
        "  \"bg_color_r\": %d,\n"
        "  \"bg_color_g\": %d,\n"
        "  \"bg_color_b\": %d,\n"
        "  \"dsi_ltc_loss_behavior\": %d,\n"
        "  \"dsi_ltc_loss_timeout_sec\": %d,\n"
        "  \"hdmi_ltc_loss_behavior\": %d,\n"
        "  \"hdmi_ltc_loss_timeout_sec\": %d,\n"
        "  \"debug_overlay_enabled\": %d,\n"
        "  \"admin_vlan_enabled\": %d,\n"
        "  \"admin_vlan_ip\": \"%s\",\n"
        "  \"admin_vlan_gateway\": \"%s\",\n"
        "  \"web_ui_bind_address\": \"%s\",\n"
        "  \"web_ui_bind_port\": %d\n"
        "}\n",
        config->timezone,
        config->ntp_server,
        config->custom_ntp_servers,
        config->display_width,
        config->display_height,
        config->refresh_hz,
        config->timecode_x_offset,
        config->timecode_y_offset,
        config->color_r,
        config->color_g,
        config->color_b,
        config->bg_color_r,
        config->bg_color_g,
        config->bg_color_b,
        config->dsi_ltc_loss_behavior,
        config->dsi_ltc_loss_timeout_sec,
        config->hdmi_ltc_loss_behavior,
        config->hdmi_ltc_loss_timeout_sec,
        config->debug_overlay_enabled,
        config->admin_vlan_enabled,
        config->admin_vlan_ip,
        config->admin_vlan_gateway,
        config->web_ui_bind_address,
        config->web_ui_bind_port
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

    if (ret == 0) {
        clamp_config_values(config);
        clamp_ltc_loss_behavior(config);
    }
    
    free(json);
    
    return ret;
}
