#ifndef CONFIG_MANAGEMENT_H
#define CONFIG_MANAGEMENT_H

#include <time.h>

typedef struct {
    char timezone[256];           // e.g., "Europe/London"
    char ntp_server[256];         // Currently selected NTP server
    char custom_ntp_servers[2048]; // JSON array of custom servers
    int refresh_hz;               // 50 or 60
    int timecode_x;               // X position
    int timecode_y;               // Y position
    int color_r;                  // Red component
    int color_g;                  // Green component
    int color_b;                  // Blue component
    int bg_color_r;               // Background red
    int bg_color_g;               // Background green
    int bg_color_b;               // Background blue
} ltc_config_t;

/**
 * Load configuration from JSON file
 * Returns 0 on success, -1 on error
 */
int config_load(const char *path, ltc_config_t *config);

/**
 * Save configuration to JSON file
 * Returns 0 on success, -1 on error
 */
int config_save(const char *path, const ltc_config_t *config);

/**
 * Get default configuration
 */
void config_default(ltc_config_t *config);

/**
 * Convert config to JSON string (caller must free)
 */
char* config_to_json(const ltc_config_t *config);

/**
 * Parse JSON string to config
 * Returns 0 on success, -1 on error
 */
int config_from_json(const char *json_str, ltc_config_t *config);

#endif // CONFIG_MANAGEMENT_H
