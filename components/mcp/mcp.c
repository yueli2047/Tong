/*
 * MCP over the console UART, shared with the logs.
 *
 * The PC side (tools/tong_bridge.py) sends one JSON-RPC message per line;
 * replies go back the same way, mixed in with the normal log output. The
 * bridge tells them apart: a protocol line starts with {"jsonrpc":"2.0".
 *
 * Keeping a reply on a line of its own: the console writes log text into
 * the UART driver one byte per call, while a reply goes in with a single
 * uart_write_bytes() call, which holds the driver's TX lock until all of it
 * is queued. So a log line can be split by a reply but never get inside
 * one, and the newline sent before and after the reply keeps it on its own
 * line either way.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "mcp.h"
#include "mcp_priv.h"

#if !CONFIG_ESP_CONSOLE_UART
#error "mcp: the console must be on a UART (menuconfig: Component config > ESP-STDIO > Channel for console output)"
#endif

#define MCP_UART        ((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM)
#define RX_BUF_SIZE     4096
#define TX_BUF_SIZE     16384   /* a tools/list reply (~8 KB) fits without waiting for the wire */
#define TASK_STACK      8192
#define TASK_PRIO       3       /* below LVGL (4) and the servos (5): the face never waits on us */

static const char *TAG = "mcp";

int64_t mcp_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void write_message(const char *json)
{
    const size_t n = strlen(json);
    char *buf = malloc(n + 2);
    if (buf == NULL) {
        ESP_LOGE(TAG, "no memory for a %u-byte reply", (unsigned)n);
        return;
    }
    buf[0] = '\n';
    memcpy(buf + 1, json, n);
    buf[n + 1] = '\n';
    uart_write_bytes(MCP_UART, buf, n + 2);
    free(buf);
}

static void mcp_task(void *arg)
{
    (void)arg;
    uint8_t chunk[128];
    for (;;) {
        /* the timeout doubles as the tick for auto-return and pat reactions */
        const int n = uart_read_bytes(MCP_UART, chunk, sizeof(chunk), pdMS_TO_TICKS(20));
        if (n > 0 && mcp_server_feed(chunk, (size_t)n, write_message) > 0) {
            ESP_LOGW(TAG, "dropped a message longer than %d bytes", MCP_LINE_MAX);
        }
        mcp_tools_tick(mcp_now_ms());
    }
}

esp_err_t mcp_start(void)
{
    static bool started;
    if (started) {
        return ESP_OK;
    }

    /* Take the console UART over from the polling console driver, as the
     * IDF console REPL does: drain what is queued, install the interrupt
     * driver, and point the console at it. */
    fflush(stdout);
    fsync(fileno(stdout));
    if (!uart_is_driver_installed(MCP_UART)) {
        const esp_err_t err = uart_driver_install(MCP_UART, RX_BUF_SIZE, TX_BUF_SIZE, 0, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uart_driver_install(UART%d) failed: %s", (int)MCP_UART, esp_err_to_name(err));
            return err;
        }
    }
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);

    if (xTaskCreate(mcp_task, "mcp", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the mcp task");
        return ESP_ERR_NO_MEM;
    }
    started = true;

    char *ready = mcp_server_ready_message();
    if (ready != NULL) {
        write_message(ready);
        cJSON_free(ready);
    }
    ESP_LOGI(TAG, "MCP server \"%s\" %s on UART%d at %d baud", MCP_SERVER_NAME, MCP_SERVER_VERSION,
             (int)MCP_UART, CONFIG_ESP_CONSOLE_UART_BAUDRATE);
    return ESP_OK;
}
