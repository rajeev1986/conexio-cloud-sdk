/*
 * uart_commissioning.c — WebSerial commissioning protocol over UART
 *
 * nRF Connect SDK v3.2.1 / nRF91xx
 *
 * Receive JSON commands from the Conexio Console dashboard over UART
 * (sent by the browser WebSerial API), and respond with device info
 * or write provisioning credentials into the modem.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <cJSON.h>
#include <cJSON_os.h>
#include <string.h>
#include <stdio.h>

#include "uart_commissioning.h"
#include "cert_store.h"

LOG_MODULE_REGISTER(uart_comm, LOG_LEVEL_INF);

/* ── UART device ─────────────────────────────────────────────────────────── */
/* Uses the same UART as RTT/logging on nRF91xx DK (UART0).                 */
/* On Thingy:91, UART1 is typically wired to the USB-UART bridge.           */
#define UART_DEV DEVICE_DT_GET(DT_NODELABEL(uart0))

/* ── Receive buffer ──────────────────────────────────────────────────────── */
/* The provision command payload can be ~4 KB (cert + key + endpoint).      */
#define RX_BUF_SIZE  4096
static char rx_buf[RX_BUF_SIZE];
static int  rx_pos = 0;

/* ── UART ISR callback ───────────────────────────────────────────────────── */
/* Accumulates characters into rx_buf until a '\n' is seen.                 */

static uart_comm_status_t pending_status = UART_COMM_IDLE;
static bool line_ready = false;

static void uart_cb(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);

    if (!uart_irq_update(dev) || !uart_irq_rx_ready(dev)) {
        return;
    }

    uint8_t c;
    while (uart_fifo_read(dev, &c, 1) == 1) {
        if (c == '\n' || c == '\r') {
            if (rx_pos > 0) {
                rx_buf[rx_pos] = '\0';
                line_ready = true;
                rx_pos = 0;
            }
        } else if (rx_pos < RX_BUF_SIZE - 1) {
            rx_buf[rx_pos++] = (char)c;
        } else {
            /* Overrun — discard buffer and start fresh */
            LOG_WRN("UART RX buffer overrun — discarding");
            rx_pos = 0;
        }
    }
}

/* ── Transmit a JSON response ────────────────────────────────────────────── */

static void uart_send_json(const struct device *dev, const char *json_str)
{
    /* Append newline so the dashboard knows the response is complete */
    size_t len = strlen(json_str);
    for (size_t i = 0; i < len; i++) {
        uart_poll_out(dev, json_str[i]);
    }
    uart_poll_out(dev, '\n');
}

static void send_ok(const struct device *dev, const char *message)
{
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"message\":\"%s\"}", message);
    uart_send_json(dev, resp);
}

static void send_error(const struct device *dev, const char *message)
{
    char resp[256];
    snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"%s\"}", message);
    uart_send_json(dev, resp);
    LOG_WRN("UART commissioning error: %s", message);
}

/* ── Handle "info" command ───────────────────────────────────────────────── */

static void handle_info(const struct device *dev,
                        const char *device_id,
                        const char *fw_version)
{
    bool already_provisioned = cert_store_device_creds_exist();

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\","
             "\"type\":\"nrf91xx\","
             "\"fw\":\"%s\","
             "\"provisioned\":%s,"
             "\"deviceId\":\"%s\"}",
             fw_version,
             already_provisioned ? "true" : "false",
             device_id);

    uart_send_json(dev, resp);
    LOG_INF("Sent device info: id=%s, fw=%s, provisioned=%d",
            device_id, fw_version, (int)already_provisioned);
}

/* ── Handle "provision" command ──────────────────────────────────────────── */
/*                                                                            */
/* Expected JSON:                                                             */
/* {                                                                          */
/*   "cmd": "provision",                                                      */
/*   "deviceId": "351358811234567",                                 */
/*   "endpoint": "xxx.iot.us-east-1.amazonaws.com",                          */
/*   "cert": "-----BEGIN CERTIFICATE-----\n...",                              */
/*   "key":  "-----BEGIN RSA PRIVATE KEY-----\n...",                          */
/*   "rootCaUrl": "https://www.amazontrust.com/..."                           */
/* }                                                                          */

/* Persistent storage for the endpoint — written to settings/flash */
#include <zephyr/settings/settings.h>
#define ENDPOINT_SETTINGS_KEY "comm/endpoint"

static int handle_provision(const struct device *dev,
                             const cJSON *msg,
                             uart_comm_status_t *out_status)
{
    const char *cert     = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "cert"));
    const char *key      = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "key"));
    const char *endpoint = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "endpoint"));
    const char *deviceId = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "deviceId"));

    if (!cert || !key || !endpoint || !deviceId) {
        send_error(dev, "missing required fields: cert, key, endpoint, deviceId");
        *out_status = UART_COMM_ERROR;
        return -EINVAL;
    }

    LOG_INF("Received provision command for device: %s", deviceId);
    LOG_INF("AWS IoT endpoint: %s", endpoint);

    /* Write cert + key into modem secure storage */
    int ret = cert_store_write_device_creds(cert, key);
    if (ret) {
        send_error(dev, "failed to write credentials to modem");
        *out_status = UART_COMM_ERROR;
        return ret;
    }

    /* Persist the endpoint so main.c can use it after reboot */
    ret = settings_save_one(ENDPOINT_SETTINGS_KEY, endpoint, strlen(endpoint) + 1);
    if (ret) {
        LOG_WRN("Failed to persist endpoint (err %d) — using prj.conf value", ret);
        /* Non-fatal — device will use the compiled-in default endpoint */
    }

    send_ok(dev, "credentials stored, rebooting");
    LOG_INF("Credentials stored successfully — signalling main to reboot");

    *out_status = UART_COMM_PROVISIONED;
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

static const struct device *uart_dev;

int uart_commissioning_init(void)
{
    uart_dev = UART_DEV;

    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART device not ready");
        return -ENODEV;
    }

    uart_irq_callback_user_data_set(uart_dev, uart_cb, NULL);
    uart_irq_rx_enable(uart_dev);

    LOG_INF("UART commissioning handler ready (115200 baud)");
    LOG_INF("Waiting for dashboard connection...");
    return 0;
}

uart_comm_status_t uart_commissioning_poll(const char *device_id,
                                           const char *fw_version)
{
    if (!line_ready) {
        return UART_COMM_IDLE;
    }

    /* Snapshot the line and clear the flag */
    char line[RX_BUF_SIZE];
    strncpy(line, rx_buf, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    line_ready = false;

    LOG_DBG("UART RX: %.*s", (int)MIN(strlen(line), 80), line);

    cJSON *msg = cJSON_Parse(line);
    if (!msg) {
        LOG_WRN("Failed to parse UART JSON: %.*s", (int)MIN(strlen(line), 80), line);
        send_error(uart_dev, "invalid JSON");
        return UART_COMM_ERROR;
    }

    const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "cmd"));
    if (!cmd) {
        cJSON_Delete(msg);
        send_error(uart_dev, "missing 'cmd' field");
        return UART_COMM_ERROR;
    }

    uart_comm_status_t result = UART_COMM_IN_PROGRESS;

    if (strcmp(cmd, "info") == 0) {
        handle_info(uart_dev, device_id, fw_version);
        result = UART_COMM_IDLE; /* info exchange is stateless */

    } else if (strcmp(cmd, "provision") == 0) {
        handle_provision(uart_dev, msg, &result);

    } else {
        send_error(uart_dev, "unknown command");
        result = UART_COMM_ERROR;
    }

    cJSON_Delete(msg);
    return result;
}
