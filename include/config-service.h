#ifndef CONFIG_SERVICE_H
#define CONFIG_SERVICE_H

/**
 * Start the configuration web service
 * Listens on localhost:port with microhttpd
 * Serves web UI and config API endpoints
 *
 * Returns: 0 on success, -1 on error
 */
int config_service_start(int port, const char *config_path);

/**
 * Stop the configuration web service
 * Cleans up microhttpd daemon
 */
void config_service_stop(void);

#endif // CONFIG_SERVICE_H
