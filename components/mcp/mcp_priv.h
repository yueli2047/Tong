/*
 * Internals of the mcp component.
 *
 *   mcp.c          UART transport (ESP-IDF): reads lines, writes messages
 *   mcp_server.c   JSON-RPC + MCP: both the per-request protocol (2026-07-28)
 *                  and the older initialize handshake (2025-11-25 and before)
 *   mcp_tools.c    the tools: schemas built from the face's own tables,
 *                  handlers that call face_* and oled_*
 *
 * mcp_server.c and mcp_tools.c have no ESP-IDF dependencies beyond the face
 * and oled APIs, so they are also built on the host for tests.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "mcp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCP_SERVER_NAME     "tong"
#define MCP_SERVER_TITLE    "Tong desk robot"
#define MCP_SERVER_VERSION  "0.2.2"

#define MCP_LINE_MAX        8192    /* longest message accepted */
#define MCP_DEPTH_MAX       32      /* deepest JSON nesting accepted */

/* Online: a host has sent something (the bridge pings every 10 s) this recently. */
#ifndef MCP_ONLINE_MS
#define MCP_ONLINE_MS       25000
#endif

/* Milliseconds since start-up; provided by the platform (mcp.c on the device). */
int64_t mcp_now_ms(void);

/* Sends one complete JSON message (no newline). */
typedef void (*mcp_write_fn)(const char *json);

/* Handle one received line; sends at most one reply through `write`. */
void mcp_server_handle(const char *line, mcp_write_fn write);

/* Feed raw received bytes: splits them into lines (CR or LF ends a line,
 * empty lines are skipped) and handles each. Lines longer than MCP_LINE_MAX
 * are dropped whole. Returns the number of lines dropped that way. */
int mcp_server_feed(const void *data, size_t n, mcp_write_fn write);

/* Sent once at start-up so the bridge knows the device (re)started.
 * The bridge consumes it; it never reaches the MCP client. Free with cJSON_free(). */
char *mcp_server_ready_message(void);

/* ---- tools (mcp_tools.c) ---- */

/* All tool definitions, in a fixed order. */
cJSON *mcp_tools_list(void);

/* Run tool `name` with `args` (an object, or NULL for none). Returns the
 * CallToolResult body ({ content, isError, ... }), or NULL if there is no
 * such tool. Invalid arguments come back as a result with isError = true,
 * worded so the model can correct itself. */
cJSON *mcp_tools_call(const char *name, const cJSON *args);

/* Guidance for the model on how to use the tools as a whole. */
const char *mcp_tools_instructions(void);

/* Run the timed behaviour (auto-return to idle, pat reactions). Call often
 * (every ~20 ms) from the same task that handles messages. */
void mcp_tools_tick(int64_t now_ms);

#ifdef __cplusplus
}
#endif
