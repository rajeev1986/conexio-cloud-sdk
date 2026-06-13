#ifndef CERT_STORE_H
#define CERT_STORE_H

#include <stdbool.h>

/*
 * cert_store.h — Modem credential management helpers
 *
 * Abstracts modem_key_mgmt read/write/exists so the rest of the firmware
 * doesn't need to care about TLS tag numbers or credential types.
 *
 * Two credential sets live in modem secure storage:
 *
 *   CLAIM (tags 10-12)  — shared bootstrap certs embedded in firmware.
 *                          Used only during the provisioning MQTT session.
 *   DEVICE (tags 20-22) — unique certs issued by AWS Fleet Provisioning.
 *                          Written once, used for all subsequent connections.
 */

/* ── Write claim (bootstrap) credentials ──────────────────────────────────── */

/**
 * @brief Provision claim Root CA, certificate, and private key into modem.
 *
 * Safe to call on every boot — checks modem_key_mgmt_exists() first and
 * skips writes when the creds are already present.
 *
 * @return 0 on success, negative errno on failure.
 */
int cert_store_provision_claim_creds(void);

/* ── Write device credentials (called once after provisioning) ───────────── */

/**
 * @brief Store the unique device certificate issued by AWS Fleet Provisioning.
 *
 * @param cert  PEM string (null-terminated).
 * @param key   PEM string (null-terminated).
 * @return 0 on success, negative errno on failure.
 */
int cert_store_write_device_creds(const char *cert, const char *key);

/* ── Check state ──────────────────────────────────────────────────────────── */

/**
 * @brief Return true if the device certificate (tag 21) is present in modem.
 *
 * Used at startup to decide which phase to enter:
 *   false → fleet provisioning phase (use claim creds)
 *   true  → normal operation phase   (use device creds)
 */
bool cert_store_device_creds_exist(void);

#endif /* CERT_STORE_H */
