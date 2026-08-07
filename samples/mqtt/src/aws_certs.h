#ifndef AWS_CERTS_H
#define AWS_CERTS_H

/*
 * TLS credential tag definitions.
 * These tags are used to register credentials with the modem's TLS stack
 * and reference them when creating a TLS socket.
 */

/** Tag for the AWS Root CA certificate. */
#define AWS_CA_TAG    1

/** Tag for the device client certificate. */
#define AWS_CERT_TAG  2

/** Tag for the device private key. */
#define AWS_KEY_TAG   3

/**
 * @brief Provision TLS credentials into the modem's security storage.
 *
 * Must be called once at startup, before any network connections are made.
 * The certificate content is embedded at build time from src/certs/*.pem.
 *
 * @return 0 on success, negative errno on failure.
 */
int aws_certs_provision(void);

#endif /* AWS_CERTS_H */
