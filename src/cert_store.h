#ifndef CONEXIO_CERT_STORE_H
#define CONEXIO_CERT_STORE_H

#include "config_fetch.h"

/**
 * Download the Root CA from the URL in the fetched config and store it
 * in the modem. Verifies that the device cert + key are already present
 * (placed there by fleet provisioning).
 * @return 0 on success, -ENOENT if device cert/key not found.
 */
int cert_store_provision_from_config(const struct conexio_cloud_config_t *cfg);

#endif
