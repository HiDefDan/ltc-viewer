#ifndef CONFIG_MANAGEMENT_H
#define CONFIG_MANAGEMENT_H

#include <time.h>

typedef struct {
    char timezone[256];           // e.g., "Europe/London"
    char ntp_server[256];         // Currently selected NTP server
    char custom_ntp_servers[2048]; // JSON array of custom servers
    int display_width;            // Target display width (mode width)
    int display_height;           // Target display height (mode height)
    float refresh_hz;             // Display refresh rate (e.g., 50.0, 59.94, 60.0)
    int timecode_x_offset;        // X offset in pixels from horizontal center (±)
    int timecode_y_offset;        // Y offset in pixels from vertical center (±)
    int color_r;                  // Red component
    int color_g;                  // Green component
    int color_b;                  // Blue component
    int bg_color_r;               // Background red
    int bg_color_g;               // Background green
    int bg_color_b;               // Background blue

    // LTC loss behavior per display
    // 0: hold last frame indefinitely
    // 1: restore system time after timeout
    int dsi_ltc_loss_behavior;
    int dsi_ltc_loss_timeout_sec;
    int hdmi_ltc_loss_behavior;
    int hdmi_ltc_loss_timeout_sec;
    int debug_overlay_enabled;      // 1 to show compact on-screen debug metrics
    
    // Network settings
    int admin_vlan_enabled;       // Enable VLAN 1 for admin access
    char admin_vlan_ip[64];       // Static IP with CIDR (e.g., "192.168.1.100/24")
    char admin_vlan_gateway[64];  // Gateway IP (optional)
    char web_ui_bind_address[64]; // Bind address: "0.0.0.0" for all, or specific IP
    int web_ui_bind_port;         // Web UI port (default 8080)
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
