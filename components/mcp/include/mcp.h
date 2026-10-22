#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MCP server: lets Claude (or any MCP client) drive the face, the head
 * gestures and the text display, through tools/tong_bridge.py on the PC.
 *
 * It runs on the console UART and shares it with the logs; the bridge
 * separates the two. Call after face_init() and oled_init(). If it fails,
 * the device keeps working without it.
 */
esp_err_t mcp_start(void);

/* True while a host is attached: the bridge pings every 10 s, so this turns
 * false about 25 s after the PC side goes away. Any task. */
bool mcp_host_online(void);

/* A pat on the head: a short happy reaction, then the face it interrupted
 * comes back. Counted, and reported to the model in its next get_status or
 * express result. Any task. */
void mcp_pat(void);

/* The face was set by hand (e.g. buttons while offline): forget a pending
 * return to idle so the choice stays. Any task. */
void mcp_cancel_auto_return(void);

#ifdef __cplusplus
}
#endif
