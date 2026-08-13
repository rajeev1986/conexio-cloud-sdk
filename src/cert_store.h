#ifndef CONEXIO_CERT_STORE_H
#define CONEXIO_CERT_STORE_H

#include "config_fetch.h"

/**
 * Ensure all TLS credentials required for MQTT are present in the modem.
 *
 * Called once by conexio_cloud_init() before LTE connects.
 *
 * What it does:
 *   1. Writes the AWS Root CA to modem tag CONFIG_CONEXIO_CLOUD_CA_TAG (100)
 *      if it is not already present (write-once from embedded firmware).
 *   2. Verifies that the device certificate is present at tag
 *      CONFIG_CONEXIO_CLOUD_CERT_TAG (101).
 *   3. Verifies that the device private key is present at tag
 *      CONFIG_CONEXIO_CLOUD_KEY_TAG (102).
 *
 * The device certificate and private key are NOT written by this function.
 * They must already be in the modem, placed there by the
 * conexio-stratus-provision sample via AWS IoT Fleet Provisioning
 * (CreateCertificateFromCsr + RegisterThing).  If either is missing this
 * function returns -ENOENT and logs a clear error directing the developer
 * to re-flash the provisioning firmware first.
 *
 * @param cfg  Fetched cloud config (accepted for API compatibility; unused —
 *             the Root CA is embedded at compile time).
 * @return     0 on success.
 *             -ENOENT if device cert or key is absent from the modem.
 *             Negative errno on modem key management failure.
 */
int cert_store_provision_from_config(const struct conexio_cloud_config_t *cfg);

#endif /* CONEXIO_CERT_STORE_H */
