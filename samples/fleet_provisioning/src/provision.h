#ifndef PROVISION_H
#define PROVISION_H

#include <zephyr/net/mqtt.h>

/**
 * @brief Pre-generate the device key pair and CSR while modem is offline.
 *
 * Must be called BEFORE lte_lc_connect_async() — AT%KEYGEN requires
 * the modem to be in offline/AT-command mode (LTE not active).
 *
 * Generates an RSA-2048 key pair at CONFIG_STRATUS_DEVICE_CERT_TAG.
 * The private key stays in the modem's secure storage.
 * The CSR is cached internally for use by run_provisioning().
 *
 * Safe to call on every boot — if provisioning is already done,
 * run_provisioning() will not be called and the CSR is unused.
 *
 * @return 0 on success, negative errno on failure.
 */
int provision_prepare_csr(void);

/**
 * @brief Run the AWS Fleet Provisioning exchange.
 *
 * Requires provision_prepare_csr() to have been called first (while offline).
 * Connects to AWS IoT using claim credentials, calls
 * CreateCertificateFromCsr, then RegisterThing, then stores
 * the resulting device certificate via modem_key_mgmt.
 *
 * @param device_id  IMEI-derived device ID string.
 * @param client     Configured mqtt_client (claim creds, broker resolved).
 * @return 0 on success, negative errno on failure.
 */
int run_provisioning(const char *device_id, struct mqtt_client *client);

/** MQTT event handler for the provisioning session — call from client evt_cb. */
void provision_mqtt_evt_handler(struct mqtt_client *client,
                                const struct mqtt_evt *evt);

#endif /* PROVISION_H */
