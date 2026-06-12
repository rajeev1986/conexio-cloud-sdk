/**
 * transport.h — Internal transport interface (Phase 2, private to SDK)
 */
#ifndef CONEXIO_TRANSPORT_H
#define CONEXIO_TRANSPORT_H

#include <stddef.h>
#include <stdbool.h>
#include "config_fetch.h"

typedef void (*transport_event_cb_t)(void);

/** Init transport using runtime-fetched config (host, API key, etc.) */
int  transport_init_with_config(const char *device_id,
                                const struct conexio_cloud_config_t *cfg);
int  transport_connect(void);
int  transport_disconnect(void);
int  transport_publish(const char *payload, size_t len);
void transport_poll(k_timeout_t timeout);
bool transport_is_connected(void);

void transport_on_connected(void);
void transport_on_disconnected(void);
void transport_on_message(const char *json_str, size_t len);

#endif
