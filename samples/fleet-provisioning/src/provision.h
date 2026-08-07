#ifndef PROVISION_H
#define PROVISION_H

/*
 * provision.h — AWS IoT Fleet Provisioning state machine
 *
 * Handles the full provisioning MQTT session:
 *   1. Connect to AWS IoT using claim (bootstrap) credentials
 *   2. Subscribe to the provisioning accepted/rejected response topics
 *   3. Publish a CreateKeysAndCertificate request
 *   4. Receive the new unique certificate + private key from AWS
 *   5. Publish a RegisterThing request (activates the new cert + creates Thing)
 *   6. Receive RegisterThing acceptance
 *   7. Write the unique cert + key into modem storage via cert_store
 *   8. Persist a "provisioned" flag in Settings so we skip this on next boot
 *
 * After run_provisioning() returns 0, the device must disconnect and reconnect
 * using the device credentials (tags 20-22) for normal MQTT operation.
 */

#include <zephyr/net/mqtt.h>

/**
 * @brief Execute the Fleet Provisioning flow.
 *
 * Blocking — drives the MQTT event loop internally until provisioning
 * completes or fails.
 *
 * @param device_id  The IMEI-derived device ID string. AWS uses this as the
 *                   Thing name in the provisioning template.
 * @param client     A pre-initialised (but not yet connected) mqtt_client.
 *                   Must be set up to use the CLAIM TLS tags (10-12).
 *
 * @return 0 on success.
 *         -ETIMEDOUT if AWS does not respond within the timeout.
 *         -EACCES    if AWS rejects the provisioning request.
 *         Other negative errno on MQTT/network failure.
 */
int run_provisioning(const char *device_id, struct mqtt_client *client);

#endif /* PROVISION_H */
