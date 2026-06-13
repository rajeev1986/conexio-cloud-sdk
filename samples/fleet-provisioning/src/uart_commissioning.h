#ifndef UART_COMMISSIONING_H
#define UART_COMMISSIONING_H

/*
 * uart_commissioning.h — WebSerial commissioning protocol over UART
 *
 * Implements the server side (device side) of the JSON-over-serial protocol
 * that the Conexio Console dashboard uses to provision credentials.
 *
 * Protocol (115200 baud, newline-delimited JSON):
 *
 *   Dashboard → Device:  {"cmd":"info"}
 *   Device → Dashboard:  {"status":"ok","type":"nrf91xx","fw":"1.2.0",
 *                          "provisioned":false,"deviceId":"nrf-imei-..."}
 *
 *   Dashboard → Device:  {"cmd":"provision","deviceId":"...","endpoint":"...",
 *                          "cert":"-----BEGIN CERTIFICATE...","key":"-----BEGIN RSA...",
 *                          "rootCaUrl":"https://..."}
 *   Device → Dashboard:  {"status":"ok","message":"credentials stored, rebooting"}
 *
 * Call uart_commissioning_init() once at startup.
 * Call uart_commissioning_poll() in your main loop before the MQTT connect.
 * Returns UART_COMM_PROVISIONED when the full provisioning flow completes.
 */

#include <stdbool.h>

typedef enum {
    UART_COMM_IDLE,         /* no commissioning in progress */
    UART_COMM_IN_PROGRESS,  /* received a cmd, processing */
    UART_COMM_PROVISIONED,  /* credentials written to modem — reboot required */
    UART_COMM_ERROR,        /* parse or storage error */
} uart_comm_status_t;

/**
 * @brief Initialise the UART commissioning handler.
 *
 * Configures the UART device for 115200 baud, 8N1.
 * Must be called once at startup, before uart_commissioning_poll().
 *
 * @return 0 on success, negative errno on failure.
 */
int uart_commissioning_init(void);

/**
 * @brief Poll for and handle incoming commissioning commands.
 *
 * Non-blocking. Returns immediately if no complete JSON line is available.
 * Call this in your main loop.
 *
 * @param device_id   The device's IMEI-derived ID (used in the info response).
 * @param fw_version  Firmware version string (e.g. "1.2.0").
 *
 * @return UART_COMM_IDLE         if nothing happened.
 *         UART_COMM_IN_PROGRESS  if a command was received and is being processed.
 *         UART_COMM_PROVISIONED  if credentials were written successfully.
 *         UART_COMM_ERROR        on a parse or storage error.
 */
uart_comm_status_t uart_commissioning_poll(const char *device_id,
                                           const char *fw_version);

#endif /* UART_COMMISSIONING_H */
