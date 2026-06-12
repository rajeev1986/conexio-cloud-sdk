#ifndef CONEXIO_CONFIG_FETCH_H
#define CONEXIO_CONFIG_FETCH_H

/** Runtime configuration fetched from the Conexio config service. */
struct conexio_cloud_config_t {
    char mqtt_host[128];       /**< MQTT broker hostname */
    char http_host[128];       /**< HTTP API Gateway hostname */
    char http_api_key[128];    /**< HTTP ingestor API key */
    char root_ca_url[256];     /**< URL to download AWS Root CA */
};

/**
 * @brief Fetch configuration from the Conexio config service.
 *
 * Uses the device IMEI to identify the workspace and returns
 * per-workspace MQTT/HTTP endpoints and credentials.
 * Results are cached for CONFIG_CONEXIO_CLOUD_CONFIG_CACHE_SEC seconds.
 *
 * @param imei  15-digit modem IMEI string.
 * @param out   Pointer to config struct to populate.
 * @return 0 on success, negative errno on failure.
 */
int config_fetch(const char *imei, struct conexio_cloud_config_t *out);

#endif
