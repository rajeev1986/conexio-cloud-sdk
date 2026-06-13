/*
 * main.c — Conexio Console HTTPS telemetry sample
 *
 * nRF Connect SDK v3.2.1 / nRF91xx
 *
 * Sends a JSON telemetry payload to POST /v1/ingest on the Conexio Console
 * backend using HTTPS. Authenticated with an x-api-key header.
 *
 * No device certificates required — just the API key from Secrets Manager.
 *
 * Commands:
 *   After each successful telemetry send, this sample polls
 *   GET /v1/device-commands?deviceId=<id> and ACKs executed commands via
 *   PUT /v1/device-commands/{commandId}/ack. This mirrors the real device
 *   wake cycle: wake → send telemetry → poll commands → execute → ACK → sleep.
 *
 * For full bidirectional (real-time) control, use the MQTT sample instead.
 */

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/http/client.h>
#include <modem/lte_lc.h>
#include <modem/modem_info.h>
#include <modem/modem_key_mgmt.h>
#include <date_time.h>
#include <cJSON.h>
#include <cJSON_os.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

/* ── Configuration ───────────────────────────────────────────────────────── */
#define DEVICE_ID   CONFIG_CONEXIO_DEVICE_ID
#define API_KEY     CONFIG_CONEXIO_API_KEY
#define API_HOST    CONFIG_CONEXIO_API_HOST
#define API_PORT    443
#define API_PATH    "/v1/ingest"

/* TLS tag for the AWS Root CA (provisioned at first boot) */
#define CA_SEC_TAG  1

/* ── Buffers ─────────────────────────────────────────────────────────────── */
static uint8_t recv_buf[512];
static uint8_t request_buf[1024];

/* ─────────────────────────────────────────────────────────────────────────── */
/* AWS Root CA — embedded for TLS verification of API Gateway                  */
/* Content rephrased for compliance with licensing restrictions                */
/* Source: https://www.amazontrust.com/repository/AmazonRootCA1.pem           */
/* ─────────────────────────────────────────────────────────────────────────── */
static const char aws_root_ca[] =
    "-----BEGIN CERTIFICATE-----\n"
    /* Paste AmazonRootCA1.pem content here, or use modem_key_mgmt to load it
     * from a file provisioned separately.
     *
     * Download from:
     *   curl -o AmazonRootCA1.pem \
     *     https://www.amazontrust.com/repository/AmazonRootCA1.pem
     *
     * Then paste the full PEM content between these markers.
     */
    "REPLACE_WITH_AMAZON_ROOT_CA1_PEM_CONTENT\n"
    "-----END CERTIFICATE-----\n";

/* ─────────────────────────────────────────────────────────────────────────── */
/* Reboot counter — NVS-persisted, incremented on every boot                  */
/* ─────────────────────────────────────────────────────────────────────────── */

#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>

#define REBOOT_NVS_PARTITION        storage_partition
#define REBOOT_NVS_PARTITION_DEVICE FIXED_PARTITION_DEVICE(REBOOT_NVS_PARTITION)
#define REBOOT_NVS_PARTITION_OFFSET FIXED_PARTITION_OFFSET(REBOOT_NVS_PARTITION)
#define REBOOT_CNT_NVS_ID           1U

static struct nvs_fs reboot_nvs;
static uint32_t      g_reboot_cnt = 0;

static void reboot_counter_init(void)
{
    struct flash_pages_info info;
    reboot_nvs.flash_device = REBOOT_NVS_PARTITION_DEVICE;
    reboot_nvs.offset       = REBOOT_NVS_PARTITION_OFFSET;

    if (!device_is_ready(reboot_nvs.flash_device)) {
        LOG_WRN("Reboot counter: flash not ready");
        return -1;
    }
    flash_get_page_info_by_offs(reboot_nvs.flash_device, reboot_nvs.offset, &info);
    reboot_nvs.sector_size  = info.size;
    reboot_nvs.sector_count = 2U;

    if (nvs_mount(&reboot_nvs) != 0) {
        LOG_WRN("Reboot counter: NVS mount failed");
        return -1;
    }

    nvs_read(&reboot_nvs, REBOOT_CNT_NVS_ID, &g_reboot_cnt, sizeof(g_reboot_cnt));
    g_reboot_cnt++;
    nvs_write(&reboot_nvs, REBOOT_CNT_NVS_ID, &g_reboot_cnt, sizeof(g_reboot_cnt));
    LOG_INF("Reboot counter: %u", g_reboot_cnt);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Sensor reading stubs — replace with your actual sensor drivers             */
/* ─────────────────────────────────────────────────────────────────────────── */

static float read_temperature(void)
{
    /* TODO: replace with your sensor driver, e.g.:
     *   struct sensor_value val;
     *   sensor_sample_fetch(dev);
     *   sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
     *   return sensor_value_to_double(&val);
     */
    return 22.5f;
}

static float read_humidity(void)
{
    return 61.0f; /* TODO: replace with sensor driver */
}

static int16_t read_rssi(void)
{
    struct modem_param_info modem_param;
    int ret = modem_info_params_get(&modem_param);
    if (ret) {
        return INT16_MIN;
    }
    return (int16_t)modem_param.network.rsrp.value;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* ISO-8601 timestamp                                                          */
/* ─────────────────────────────────────────────────────────────────────────── */

static void get_iso8601(char *buf, size_t buf_len)
{
    int64_t unix_ms;
    int ret = date_time_now(&unix_ms);
    if (ret) {
        snprintf(buf, buf_len, "1970-01-01T00:00:%02lld.000Z",
                 (long long)(k_uptime_get() / 1000));
        return -1;
    }
    time_t unix_sec = (time_t)(unix_ms / 1000);
    struct tm *t = gmtime(&unix_sec);
    snprintf(buf, buf_len,
             "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec,
             (int)(unix_ms % 1000));
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Provision AWS Root CA into modem (once)                                    */
/* ─────────────────────────────────────────────────────────────────────────── */

static int provision_root_ca(void)
{
    bool exists;
    int ret = modem_key_mgmt_exists(CA_SEC_TAG,
                                    MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                                    &exists);
    if (ret == 0 && exists) {
        LOG_INF("Root CA already provisioned (tag %d)", CA_SEC_TAG);
        return 0;
    }

    LOG_INF("Provisioning Root CA into modem...");
    ret = modem_key_mgmt_write(CA_SEC_TAG,
                               MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                               aws_root_ca,
                               strlen(aws_root_ca));
    if (ret) {
        LOG_ERR("Failed to provision Root CA (err %d)", ret);
        return ret;
    }
    LOG_INF("Root CA provisioned");
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* HTTP response handler                                                       */
/* ─────────────────────────────────────────────────────────────────────────── */

static void http_response_handler(struct http_response *rsp,
                                  enum http_final_call final_data,
                                  void *user_data)
{
    if (final_data == HTTP_DATA_FINAL) {
        LOG_INF("HTTP response: status=%d (%s)",
                rsp->http_status_code,
                rsp->http_status ? rsp->http_status : "");
        /* 202 Accepted is the normal success response from POST /v1/ingest */
        if (rsp->http_status_code != 200 &&
            rsp->http_status_code != 201 &&
            rsp->http_status_code != 202) {
            LOG_WRN("Unexpected status code: %d", rsp->http_status_code);
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Command poll response buffer                                                */
/* ─────────────────────────────────────────────────────────────────────────── */

static char cmd_response_body[2048];
static int  cmd_response_len   = 0;
static int  last_http_status   = 0;

static void cmd_poll_response_cb(struct http_response *rsp,
                                  enum http_final_call final_data,
                                  void *user_data)
{
    if (final_data == HTTP_DATA_FINAL) {
        last_http_status = rsp->http_status_code;
        if (rsp->http_status_code == 200 && rsp->body_frag_len > 0) {
            size_t copy = MIN(rsp->body_frag_len, sizeof(cmd_response_body) - 1);
            memcpy(cmd_response_body, rsp->body_frag_start, copy);
            cmd_response_body[copy] = '\0';
            cmd_response_len = (int)copy;
        } else {
            cmd_response_len = 0;
        }
    }
}

static void ack_response_cb(struct http_response *rsp,
                             enum http_final_call final_data,
                             void *user_data)
{
    (void)rsp; (void)final_data; (void)user_data;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Open a TLS socket to the API host                                           */
/* ─────────────────────────────────────────────────────────────────────────── */

static int open_tls_socket(void)
{
    int sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
    if (sock < 0) return -errno;

    sec_tag_t sec_tags[] = { CA_SEC_TAG };
    int ret = zsock_setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST,
                               sec_tags, sizeof(sec_tags));
    if (ret) { zsock_close(sock); return -errno; }

    ret = zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME,
                           API_HOST, strlen(API_HOST));
    if (ret) { zsock_close(sock); return -errno; }

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *result;
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", API_PORT);

    ret = getaddrinfo(API_HOST, port_str, &hints, &result);
    if (ret) { zsock_close(sock); return -ENOENT; }

    ret = zsock_connect(sock, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);
    if (ret) { zsock_close(sock); return -errno; }

    return sock;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Poll for queued commands after each telemetry send                         */
/*                                                                             */
/* Cloud endpoint: GET /v1/device-commands?deviceId=<id>   auth: x-api-key   */
/* ACK endpoint:   PUT /v1/device-commands/{commandId}/ack  auth: x-api-key   */
/*                                                                             */
/* Commands queued from the Conexio Console dashboard are delivered here.     */
/* ─────────────────────────────────────────────────────────────────────────── */

static void poll_and_execute_commands(void)
{
    char url[128];
    snprintf(url, sizeof(url), "/v1/device-commands?deviceId=%s", DEVICE_ID);

    int sock = open_tls_socket();
    if (sock < 0) {
        LOG_WRN("Command poll: socket failed (%d)", sock);
        return -1;
    }

    const char *headers[] = {
        "x-api-key: " API_KEY "\r\n",
        NULL,
    };

    cmd_response_len = 0;
    struct http_request req = {
        .method        = HTTP_GET,
        .url           = url,
        .host          = API_HOST,
        .protocol      = "HTTP/1.1",
        .header_fields = headers,
        .response      = cmd_poll_response_cb,
        .recv_buf      = recv_buf,
        .recv_buf_len  = sizeof(recv_buf),
    };

    int ret = http_client_req(sock, &req, K_SECONDS(10), NULL);
    zsock_close(sock);

    if (ret < 0 || cmd_response_len == 0) {
        return; /* no commands or error */
    }

    cJSON *root = cJSON_Parse(cmd_response_body);
    if (!root) return;

    const cJSON *commands = cJSON_GetObjectItem(root, "commands");
    if (!cJSON_IsArray(commands) || cJSON_GetArraySize(commands) == 0) {
        cJSON_Delete(root);
        return -1;
    }

    LOG_INF("Commands pending: %d", cJSON_GetArraySize(commands));

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, commands) {
        const char *command_id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "commandId"));
        const char *sk         = cJSON_GetStringValue(cJSON_GetObjectItem(item, "sk"));
        const char *cmd_name   = cJSON_GetStringValue(cJSON_GetObjectItem(item, "command"));
        const cJSON *payload   = cJSON_GetObjectItem(item, "payload");

        if (!command_id || !cmd_name) continue;

        LOG_INF("Command: %s (id=%s)", cmd_name, command_id);

        /* ── Execute command ─────────────────────────────────────────────── */
        if (strcmp(cmd_name, "REBOOT") == 0) {
            LOG_INF("Rebooting by remote command...");
            k_sleep(K_MSEC(500));
            sys_reboot(SYS_REBOOT_COLD);

        } else if (strcmp(cmd_name, "FAN_ON") == 0) {
            const cJSON *speed = payload ? cJSON_GetObjectItem(payload, "speed") : NULL;
            LOG_INF("FAN_ON speed=%d%%",
                    cJSON_IsNumber(speed) ? (int)speed->valuedouble : 100);
            /* TODO: gpio_pin_set(fan_dev, FAN_PIN, 1); */

        } else if (strcmp(cmd_name, "FAN_OFF") == 0) {
            LOG_INF("FAN_OFF");
            /* TODO: gpio_pin_set(fan_dev, FAN_PIN, 0); */

        } else if (strcmp(cmd_name, "SET_INTERVAL") == 0) {
            /* Interval value is in seconds (consistent with OTA Config page) */
            const cJSON *iv = payload ? cJSON_GetObjectItem(payload, "interval") : NULL;
            if (cJSON_IsNumber(iv)) {
                int new_sec = (int)iv->valuedouble;
                if (new_sec >= 10 && new_sec <= 3600) {
                    LOG_INF("Interval updated to %ds", new_sec);
                    /* Note: in the standalone sample the interval is used in
                     * the main loop via k_sleep(). Updating a shared variable
                     * here is left as an exercise for your application. */
                }
            }

        } else {
            LOG_WRN("Unknown command: %s", cmd_name);
        }

        /* ── ACK the command ─────────────────────────────────────────────── */
        char ack_url[128];
        snprintf(ack_url, sizeof(ack_url),
                 "/v1/device-commands/%s/ack", command_id);

        cJSON *ack_body = cJSON_CreateObject();
        cJSON_AddStringToObject(ack_body, "deviceId", DEVICE_ID);
        if (sk) cJSON_AddStringToObject(ack_body, "sk", sk);
        cJSON_AddStringToObject(ack_body, "result", "executed");

        char *ack_json = cJSON_PrintUnformatted(ack_body);
        cJSON_Delete(ack_body);

        if (ack_json) {
            int ack_sock = open_tls_socket();
            if (ack_sock >= 0) {
                const char *ack_headers[] = {
                    "Content-Type: application/json\r\n",
                    "x-api-key: " API_KEY "\r\n",
                    NULL,
                };
                struct http_request ack_req = {
                    .method        = HTTP_PUT,
                    .url           = ack_url,
                    .host          = API_HOST,
                    .protocol      = "HTTP/1.1",
                    .payload       = ack_json,
                    .payload_len   = strlen(ack_json),
                    .header_fields = ack_headers,
                    .response      = ack_response_cb,
                    .recv_buf      = recv_buf,
                    .recv_buf_len  = sizeof(recv_buf),
                };
                ret = http_client_req(ack_sock, &ack_req, K_SECONDS(10), NULL);
                zsock_close(ack_sock);
                if (ret < 0) {
                    LOG_WRN("Command ACK failed (%d)", ret);
                } else {
                    LOG_DBG("Command ACK sent: %s", command_id);
                }
            }
            cJSON_FreeString(ack_json);
        }
    }

    cJSON_Delete(root);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Poll for pending OTA config and apply + ACK it                             */
/*                                                                             */
/* Cloud endpoint: GET /v1/devices/<id>/config/pending   auth: x-api-key     */
/* ACK endpoint:   PUT /v1/devices/<id>/config/ack       auth: x-api-key     */
/*                                                                             */
/* Config pushes from the OTA Config page stay 'pending' until the device    */
/* calls the ACK endpoint. This is the HTTP pull path for OTA config.        */
/* ─────────────────────────────────────────────────────────────────────────── */

static void poll_pending_config(void)
{
    char url[96];
    snprintf(url, sizeof(url), "/v1/devices/%s/config/pending", DEVICE_ID);

    int sock = open_tls_socket();
    if (sock < 0) return;

    const char *headers[] = {
        "x-api-key: " API_KEY "\r\n",
        NULL,
    };

    cmd_response_len = 0;
    struct http_request req = {
        .method        = HTTP_GET,
        .url           = url,
        .host          = API_HOST,
        .protocol      = "HTTP/1.1",
        .header_fields = headers,
        .response      = cmd_poll_response_cb,
        .recv_buf      = recv_buf,
        .recv_buf_len  = sizeof(recv_buf),
    };

    int ret = http_client_req(sock, &req, K_SECONDS(10), NULL);
    zsock_close(sock);

    if (ret < 0 || cmd_response_len == 0) return;

    cJSON *root = cJSON_Parse(cmd_response_body);
    if (!root) return;

    const cJSON *pending   = cJSON_GetObjectItem(root, "pending");
    const char  *config_id = NULL;
    bool applied = false;

    if (cJSON_IsObject(pending)) {
        config_id           = cJSON_GetStringValue(cJSON_GetObjectItem(pending, "configId"));
        const cJSON *config = cJSON_GetObjectItem(pending, "config");
        const cJSON *ver    = cJSON_GetObjectItem(pending, "version");
        int version = cJSON_IsNumber(ver) ? (int)ver->valuedouble : 0;

        LOG_INF("Pending config: v%d id=%s", version, config_id ? config_id : "?");

        if (config && cJSON_IsObject(config)) {
            /* Apply each known config key */
            const cJSON *iv = cJSON_GetObjectItem(config, "telemetryIntervalSec");
            if (cJSON_IsNumber(iv)) {
                int new_sec = (int)iv->valuedouble;
                if (new_sec >= 10 && new_sec <= 3600) {
                    LOG_INF("Setting: telemetryIntervalSec → %ds", new_sec);
                    /* Note: updating the sleep interval from here requires a
                     * shared variable — left as an exercise for your app. */
                }
            }
            /* Add other config keys here as needed */
            applied = true;
        }

        /* ACK via PUT /v1/devices/<id>/config/ack */
        char ack_url[96];
        snprintf(ack_url, sizeof(ack_url), "/v1/devices/%s/config/ack", DEVICE_ID);

        cJSON *ack_body = cJSON_CreateObject();
        if (config_id) cJSON_AddStringToObject(ack_body, "configId", config_id);
        cJSON_AddBoolToObject(ack_body, "success", applied);

        char *ack_json = cJSON_PrintUnformatted(ack_body);
        cJSON_Delete(ack_body);

        if (ack_json) {
            int ack_sock = open_tls_socket();
            if (ack_sock >= 0) {
                const char *ack_headers[] = {
                    "Content-Type: application/json\r\n",
                    "x-api-key: " API_KEY "\r\n",
                    NULL,
                };
                struct http_request ack_req = {
                    .method        = HTTP_PUT,
                    .url           = ack_url,
                    .host          = API_HOST,
                    .protocol      = "HTTP/1.1",
                    .payload       = ack_json,
                    .payload_len   = strlen(ack_json),
                    .header_fields = ack_headers,
                    .response      = ack_response_cb,
                    .recv_buf      = recv_buf,
                    .recv_buf_len  = sizeof(recv_buf),
                };
                ret = http_client_req(ack_sock, &ack_req, K_SECONDS(10), NULL);
                zsock_close(ack_sock);
                LOG_DBG("Config ACK sent: %s", config_id ? config_id : "?");
            }
            cJSON_FreeString(ack_json);
        }
    }

    cJSON_Delete(root);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Send one telemetry POST                                                     */
/* ─────────────────────────────────────────────────────────────────────────── */

static int send_telemetry(void)
{
    char timestamp[32];
    get_iso8601(timestamp, sizeof(timestamp));

    float temperature = read_temperature();
    float humidity    = read_humidity();
    int16_t rssi      = read_rssi();

    /* Build JSON payload:
     * {
     *   "deviceId": "my-nrf-device",
     *   "timestamp": "2026-06-03T10:30:00.000Z",
     *   "metrics": { "temperature": 22.5, "humidity": 61.0, "_rssi": -72 }
     * }
     */
    cJSON *root    = cJSON_CreateObject();
    cJSON *metrics = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "deviceId",  DEVICE_ID);
    cJSON_AddStringToObject(root, "timestamp", timestamp);
    cJSON_AddItemToObject(root, "metrics", metrics);
    cJSON_AddNumberToObject(metrics, "temperature", (double)temperature);
    cJSON_AddNumberToObject(metrics, "humidity",    (double)humidity);
    if (rssi != INT16_MIN) {
        cJSON_AddNumberToObject(metrics, "_rssi", (double)rssi);
    }
    cJSON_AddNumberToObject(metrics, "_reboot_cnt", (double)g_reboot_cnt);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!body) {
        return -ENOMEM;
    }

    /* Build HTTP headers */
    static const char *headers[] = {
        "Content-Type: application/json\r\n",
        "x-api-key: " API_KEY "\r\n",
        NULL,
    };

    /* Open TLS socket to API Gateway */
    int sock = open_tls_socket();
    if (sock < 0) {
        LOG_ERR("Failed to open TLS socket (%d)", sock);
        cJSON_FreeString(body);
        return sock;
    }

    /* Send HTTP POST */
    struct http_request req = {
        .method          = HTTP_POST,
        .url             = API_PATH,
        .host            = API_HOST,
        .protocol        = "HTTP/1.1",
        .payload         = body,
        .payload_len     = strlen(body),
        .header_fields   = headers,
        .response        = http_response_handler,
        .recv_buf        = recv_buf,
        .recv_buf_len    = sizeof(recv_buf),
    };

    int ret = http_client_req(sock, &req, K_SECONDS(15), NULL);
    zsock_close(sock);

    if (ret < 0) {
        LOG_ERR("http_client_req failed (err %d)", ret);
    } else {
        LOG_INF("Telemetry sent: temp=%.1f hum=%.1f rssi=%d",
                (double)temperature, (double)humidity, (int)rssi);
        ret = 0;
    }

    cJSON_FreeString(body);
    return ret;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* LTE event handler                                                           */
/* ─────────────────────────────────────────────────────────────────────────── */

static K_SEM_DEFINE(lte_ready, 0, 1);

static void lte_event_handler(const struct lte_lc_evt *const evt)
{
    if (evt->type == LTE_LC_EVT_NW_REG_STATUS) {
        if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
            evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
            LOG_INF("LTE registered");
            k_sem_give(&lte_ready);
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Main                                                                        */
/* ─────────────────────────────────────────────────────────────────────────── */

int main(void)
{
    int ret;

    LOG_INF("=== Conexio Console HTTP sample ===");
    LOG_INF("Device ID  : %s", DEVICE_ID);
    LOG_INF("Endpoint   : https://%s%s", API_HOST, API_PATH);
    LOG_INF("Interval   : %ds", CONFIG_CONEXIO_TELEMETRY_INTERVAL_SEC);

    /* 1. Provision AWS Root CA into modem */
    ret = provision_root_ca();
    if (ret) {
        LOG_ERR("Root CA provisioning failed — halting");
        return -1;
    }

    /* 2. Initialise reboot counter (NVS) */
    reboot_counter_init();

    /* 3. Initialise modem info */
    modem_info_init();

    /* 3. Connect to LTE
     * NCS v3.2.1: lte_lc_init_and_connect_async() removed — use lte_lc_connect_async() */
    LOG_INF("Connecting to LTE...");
    ret = lte_lc_connect_async(lte_event_handler);
    if (ret) {
        LOG_ERR("LTE init failed (%d)", ret);
        return -1;
    }

    ret = k_sem_take(&lte_ready, K_SECONDS(90));
    if (ret) {
        LOG_ERR("LTE registration timed out");
        return -1;
    }

    /* 4. Sync time */
    date_time_update_async(NULL);
    k_sleep(K_SECONDS(3));

    /* 5. Main loop — send telemetry then poll commands every interval */
    while (1) {
        ret = send_telemetry();
        if (ret) {
            LOG_WRN("Telemetry send failed (%d) — will retry next interval", ret);
        } else {
            /*
             * After a successful ingest, run the full wake-cycle sequence:
             *   1. Poll command queue  — GET /v1/device-commands?deviceId=<id>
             *   2. Poll pending config — GET /v1/devices/<id>/config/pending
             *
             * Commands are queued indefinitely — delivered on every wake.
             * Config pushes stay pending until ACK'd — same principle.
             */
            poll_and_execute_commands();
            poll_pending_config();
        }
        k_sleep(K_SECONDS(CONFIG_CONEXIO_TELEMETRY_INTERVAL_SEC));
    }
}
