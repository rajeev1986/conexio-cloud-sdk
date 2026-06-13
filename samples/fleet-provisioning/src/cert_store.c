/*
 * cert_store.c — Modem credential management helpers
 *
 * nRF Connect SDK v3.2.1 / nRF91xx
 */

#include <zephyr/kernel.h>
#include <modem/modem_key_mgmt.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "cert_store.h"

LOG_MODULE_REGISTER(cert_store, LOG_LEVEL_INF);

/* ── Embedded claim (bootstrap) credentials ──────────────────────────────── */
/* These are generated at build time from src/certs/ by CMakeLists.txt.      */
/* They are SHARED across all devices — the same binary ships to every unit. */

static const unsigned char aws_root_ca[] = {
#include "aws_root_ca.pem.inc"
};

static const unsigned char claim_cert[] = {
#include "claim_cert.pem.inc"
};

static const unsigned char claim_key[] = {
#include "claim_key.pem.inc"
};

/* ── TLS tag constants (mirrors Kconfig defaults) ────────────────────────── */
#define CLAIM_CA_TAG    CONFIG_CONEXIO_CLAIM_CA_TAG
#define CLAIM_CERT_TAG  CONFIG_CONEXIO_CLAIM_CERT_TAG
#define CLAIM_KEY_TAG   CONFIG_CONEXIO_CLAIM_KEY_TAG
#define DEVICE_CA_TAG   CONFIG_CONEXIO_DEVICE_CA_TAG
#define DEVICE_CERT_TAG CONFIG_CONEXIO_DEVICE_CERT_TAG
#define DEVICE_KEY_TAG  CONFIG_CONEXIO_DEVICE_KEY_TAG

/* ─────────────────────────────────────────────────────────────────────────── */

static int write_if_absent(int tag,
                           enum modem_key_mgmt_cred_type type,
                           const unsigned char *data,
                           size_t len,
                           const char *label)
{
    bool exists = false;
    int ret = modem_key_mgmt_exists(tag, type, &exists);
    if (ret == 0 && exists) {
        LOG_INF("%s already in modem (tag %d) — skipping write", label, tag);
        return 0;
    }

    ret = modem_key_mgmt_write(tag, type, data, len);
    if (ret < 0) {
        LOG_ERR("Failed to write %s (tag %d, err %d)", label, tag, ret);
        return ret;
    }

    LOG_INF("%s provisioned (tag %d)", label, tag);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */

int cert_store_provision_claim_creds(void)
{
    int ret;

    LOG_INF("Provisioning claim credentials...");

    /* Root CA — shared tag for both claim and device phases */
    ret = write_if_absent(CLAIM_CA_TAG,
                          MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                          aws_root_ca,
                          sizeof(aws_root_ca) - 1,  /* exclude null terminator */
                          "Root CA (claim tag)");
    if (ret) return ret;

    /* Also write Root CA to the device tag so normal operation works
     * without an extra write after provisioning */
    ret = write_if_absent(DEVICE_CA_TAG,
                          MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                          aws_root_ca,
                          sizeof(aws_root_ca) - 1,
                          "Root CA (device tag)");
    if (ret) return ret;

    /* Claim certificate */
    ret = write_if_absent(CLAIM_CERT_TAG,
                          MODEM_KEY_MGMT_CRED_TYPE_PUBLIC_CERT,
                          claim_cert,
                          sizeof(claim_cert) - 1,
                          "Claim certificate");
    if (ret) return ret;

    /* Claim private key */
    ret = write_if_absent(CLAIM_KEY_TAG,
                          MODEM_KEY_MGMT_CRED_TYPE_PRIVATE_CERT,
                          claim_key,
                          sizeof(claim_key) - 1,
                          "Claim private key");
    if (ret) return ret;

    LOG_INF("Claim credentials ready");
    return 0;
}

int cert_store_write_device_creds(const char *cert, const char *key)
{
    int ret;

    if (!cert || !key) {
        return -EINVAL;
    }

    LOG_INF("Writing unique device certificate (tag %d)...", DEVICE_CERT_TAG);
    ret = modem_key_mgmt_write(DEVICE_CERT_TAG,
                               MODEM_KEY_MGMT_CRED_TYPE_PUBLIC_CERT,
                               cert,
                               strlen(cert));
    if (ret < 0) {
        LOG_ERR("Failed to write device cert (err %d)", ret);
        return ret;
    }

    LOG_INF("Writing unique device private key (tag %d)...", DEVICE_KEY_TAG);
    ret = modem_key_mgmt_write(DEVICE_KEY_TAG,
                               MODEM_KEY_MGMT_CRED_TYPE_PRIVATE_CERT,
                               key,
                               strlen(key));
    if (ret < 0) {
        LOG_ERR("Failed to write device key (err %d)", ret);
        return ret;
    }

    LOG_INF("Device credentials stored successfully");
    return 0;
}

bool cert_store_device_creds_exist(void)
{
    bool exists = false;
    int ret = modem_key_mgmt_exists(DEVICE_CERT_TAG,
                                    MODEM_KEY_MGMT_CRED_TYPE_PUBLIC_CERT,
                                    &exists);
    if (ret != 0) {
        /* Cannot determine — assume not provisioned, attempt provisioning */
        LOG_WRN("modem_key_mgmt_exists failed (err %d) — assuming not provisioned", ret);
        return false;
    }
    return exists;
}
