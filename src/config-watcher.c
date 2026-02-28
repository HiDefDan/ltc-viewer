#define _GNU_SOURCE
#include "config-watcher.h"
#include <sys/inotify.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

int config_watcher_init(config_watcher_t *watcher, const char *config_path) {
    strncpy(watcher->config_path, config_path, sizeof(watcher->config_path) - 1);

    // Create inotify instance
    watcher->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (watcher->inotify_fd < 0) {
        perror("inotify_init1");
        return -1;
    }

    // Get directory path by finding last '/'
    char dir_path[512];
    strncpy(dir_path, config_path, sizeof(dir_path) - 1);
    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
    } else {
        strcpy(dir_path, ".");
    }

    // Watch directory for file modifications (more reliable than watching file directly)
    watcher->watch_fd = inotify_add_watch(watcher->inotify_fd, dir_path, 
                                         IN_MODIFY | IN_CLOSE_WRITE);
    if (watcher->watch_fd < 0) {
        perror("inotify_add_watch");
        close(watcher->inotify_fd);
        return -1;
    }

    return 0;
}

int config_watcher_check(config_watcher_t *watcher) {
    if (watcher->inotify_fd < 0) return -1;

    // Buffer for inotify events
    char buf[4096];
    int len = read(watcher->inotify_fd, buf, sizeof(buf));
    
    if (len < 0) {
        // EAGAIN is expected on non-blocking read when no events
        return 0;
    }

    if (len == 0) return 0;

    // Get filename from config path
    const char *config_filename = strrchr(watcher->config_path, '/');
    if (!config_filename) config_filename = watcher->config_path;
    else config_filename++;

    // Process events
    int changed = 0;
    for (int i = 0; i < len; ) {
        struct inotify_event *event = (struct inotify_event *)&buf[i];
        
        // Check if this is our config file
        if (event->len > 0 && strcmp(event->name, config_filename) == 0) {
            if (event->mask & (IN_MODIFY | IN_CLOSE_WRITE)) {
                changed = 1;
            }
        }

        i += sizeof(struct inotify_event) + event->len;
    }

    return changed;
}

void config_watcher_cleanup(config_watcher_t *watcher) {
    if (watcher->watch_fd >= 0) {
        inotify_rm_watch(watcher->inotify_fd, watcher->watch_fd);
        watcher->watch_fd = -1;
    }
    if (watcher->inotify_fd >= 0) {
        close(watcher->inotify_fd);
        watcher->inotify_fd = -1;
    }
}
