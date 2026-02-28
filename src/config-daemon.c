#include "config-service.h"
#include "config-management.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <syslog.h>

static volatile int running = 1;

static void signal_handler(int sig) {
    fprintf(stderr, "Received signal %d, shutting down\n", sig);
    running = 0;
}

int main(int argc, char **argv) {
    int port = 8080;
    const char *config_path = "/etc/ltc-viewer/config.json";

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else {
            fprintf(stderr, "LTC Viewer Configuration Service\n");
            fprintf(stderr, "Usage: %s [-p PORT] [-c CONFIG_PATH]\n", argv[0]);
            fprintf(stderr, "  -p PORT        Listen port (default 8080)\n");
            fprintf(stderr, "  -c CONFIG_PATH Path to config JSON (default /etc/ltc-viewer/config.json)\n");
            return 1;
        }
    }

    // Setup signal handlers
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    fprintf(stderr, "Starting LTC Config Service\n");
    fprintf(stderr, "Config file: %s\n", config_path);
    fprintf(stderr, "Listen port: %d\n", port);

    // Start the service
    if (config_service_start(port, config_path) < 0) {
        fprintf(stderr, "Failed to start config service\n");
        return 1;
    }

    // Keep running
    while (running) {
        sleep(1);
    }

    config_service_stop();
    fprintf(stderr, "Config service stopped\n");
    return 0;
}
