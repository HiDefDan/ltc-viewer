#ifndef CONFIG_MANAGEMENT_H
#define CONFIG_MANAGEMENT_H

#include <time.h>
#include <stddef.h>
#include <stdint.h>

/* DSI is permanently locked to true center (no user-adjustable offset).
 * HDMI is repositionable per physical port, keyed by a stable name like
 * "HDMI-A-1" (see drm_connector_name()) so a saved offset survives
 * unplug/replug and reboot, and different ports can have different
 * offsets. Raspberry Pi CM5-class hardware exposes at most a couple of
 * independent HDMI ports, so this is generous headroom. */
#define LTC_MAX_HDMI_POSITIONS 4

typedef struct {
    char name[32];   // e.g. "HDMI-A-1", matches drm_connector_name()
    int x_offset;
    int y_offset;
} hdmi_position_t;

typedef struct {
    char timezone[256];           // e.g., "Europe/London"
    char ntp_server[256];         // Currently selected NTP server
    char custom_ntp_servers[2048]; // JSON array of custom servers
    int display_width;            // Target display width (mode width)
    int display_height;           // Target display height (mode height)
    float refresh_hz;             // Display refresh rate (e.g., 50.0, 59.94, 60.0)
    int hdmi_position_count;
    hdmi_position_t hdmi_positions[LTC_MAX_HDMI_POSITIONS];
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
    int transition_fade_ms;        // LTC<->ToD transition fade duration in milliseconds
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

/**
 * Look up a saved HDMI port's offset by name. Writes 0,0 and returns 0 if
 * the port has no saved entry yet; returns 1 if found.
 */
int config_get_hdmi_offset(const ltc_config_t *config, const char *name, int *x_offset, int *y_offset);

/**
 * Save (or update) a named HDMI port's offset. Finds an existing entry by
 * name, or creates one in the first free slot; if the table is full,
 * overwrites slot 0 (logged to stderr) rather than silently dropping data.
 */
void config_set_hdmi_offset(ltc_config_t *config, const char *name, int x_offset, int y_offset);

/**
 * Builds a stable per-port name (e.g. "HDMI-A-1") from a DRM connector's
 * type + type_id, matching the naming convention already used under
 * /sys/class/drm/cardX-HDMI-A-N/status. connector_type values are the
 * DRM_MODE_CONNECTOR_* constants from <xf86drmMode.h>.
 */
void drm_connector_name(uint32_t connector_type, uint32_t connector_type_id, char *out, size_t out_size);

#endif // CONFIG_MANAGEMENT_H
