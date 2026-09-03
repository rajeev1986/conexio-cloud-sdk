#ifndef CERT_STORE_H
#define CERT_STORE_H

#include <stdbool.h>

/*
 * cert_store.h — Modem credential management
 *
 * CLAIM creds (tags 10-12): shared bootstrap, embedded in firmware.
 * DEVICE creds (tags 100-102): unique per-device, written by AWS during provisioning.
 *   Tags 100-102 match CONFIG_CONEXIO_CLOUD_CA_TAG/CERT_TAG/KEY_TAG in the
 *   main SDK so the app sample finds them after flashing over this firmware.
 */

/** Provision claim Root CA + cert + key into modem. Skips if already present. */
int cert_store_provision_claim_creds(void);

/** Delete device cert (tag 101) and key (tag 102) if present. Always safe to call. */
void cert_store_clear_device_creds(void);

/**
 * Write unique device cert + key issued by AWS.
 * Always deletes existing entries first to avoid -EACCES on re-provisioning.
 */
int cert_store_write_device_creds(const char *cert, const char *key);

/** True if device cert (tag 101) exists in modem — indicates provisioning done. */
bool cert_store_device_creds_exist(void);

#endif /* CERT_STORE_H */
