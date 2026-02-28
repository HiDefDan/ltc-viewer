#ifndef CONFIG_WATCHER_H
#define CONFIG_WATCHER_H

#include "config-management.h"

typedef struct {
    int inotify_fd;
    int watch_fd;
    char config_path[512];
} config_watcher_t;

/**
 * Initialize config file watcher using inotify
 * Watches the config file and detects modifications
 * Returns 0 on success, -1 on error
 */
int config_watcher_init(config_watcher_t *watcher, const char *config_path);

/**
 * Check if config file has changed
 * Returns 1 if changed, 0 if not, -1 on error
 */
int config_watcher_check(config_watcher_t *watcher);

/**
 * Cleanup config watcher
 */
void config_watcher_cleanup(config_watcher_t *watcher);

#endif // CONFIG_WATCHER_H
