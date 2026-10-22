/*
 * JSON-RPC 2.0 + MCP, serving both protocol eras over one stream:
 *
 *   modern (2026-07-28): every request carries its protocol version and
 *       client capabilities in params._meta; there is no handshake.
 *       server/discover reports what the server supports.
 *   legacy (2025-11-25 and earlier): the client opens with initialize,
 *       then sends requests without _meta.
 *
 * A request with _meta is served as modern; anything else as legacy. Legacy
 * requests are also accepted without a prior initialize: the device may
 * reboot in the middle of a client's session and must not then refuse it.
 *
 * Methods: initialize, server/discover, ping, tools/list, tools/call.
 * Notifications (initialized, cancelled, ...) need no reply and get none.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "mcp_priv.h"

/* ---------- host presence ---------- */

static atomic_uint s_last_rx_ms;        /* low 32 bits of mcp_now_ms() at the last message */
static atomic_bool s_any_rx;

bool mcp_host_online(void)
{
    if (!atomic_load(&s_any_rx)) {
        return false;
    }
    const uint32_t age = (uint32_t)mcp_now_ms() - (uint32_t)atomic_load(&s_last_rx_ms);
    return age < MCP_ONLINE_MS;
}

#define META_VERSION        "io.modelcontextprotocol/protocolVersion"
#define META_CLIENT_CAPS    "io.modelcontextprotocol/clientCapabilities"
#define META_SERVER_INFO    "io.modelcontextprotocol/serverInfo"

#define ERR_PARSE           -32700
#define ERR_INVALID_REQUEST -32600
#define ERR_NO_METHOD       -32601
#define ERR_INVALID_PARAMS  -32602
#define ERR_UNSUPPORTED_VER -32022

/* Modern clients may reuse discover / tools/list results this long. The
 * list only changes with a re-flash, which needs the serial port and so
 * restarts the bridge anyway. */
#define LIST_TTL_MS         600000

static const char *const MODERN_VERSIONS[] = { "2026-07-28" };
static const char *const LEGACY_VERSIONS[] = { "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05" };

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static bool in_list(const char *v, const char *const *list, int n)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(v, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Deepest bracket nesting outside strings; stops counting past `limit`. */
static int json_depth(const char *s, int limit)
{
    int depth = 0, max = 0;
    bool in_str = false, esc = false;
    for (; *s; s++) {
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (*s == '\\') {
                esc = true;
            } else if (*s == '"') {
                in_str = false;
            }
        } else if (*s == '"') {
            in_str = true;
        } else if (*s == '{' || *s == '[') {
            if (++depth > max && (max = depth) > limit) {
                return max;
            }
        } else if (*s == '}' || *s == ']') {
            depth--;
        }
    }
    return max;
}

static cJSON *server_info(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", MCP_SERVER_NAME);
    cJSON_AddStringToObject(o, "title", MCP_SERVER_TITLE);
    cJSON_AddStringToObject(o, "version", MCP_SERVER_VERSION);
    return o;
}

static cJSON *version_list(const char *const *list, int n)
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON_AddItemToArray(a, cJSON_CreateString(list[i]));
    }
    return a;
}

/* "jsonrpc" first, then "id" (null when the request's id could not be read). */
static cJSON *new_reply(const cJSON *id)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "jsonrpc", "2.0");
    cJSON_AddItemToObject(m, "id", id ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
    return m;
}

static void send(mcp_write_fn write, cJSON *msg)
{
    char *s = cJSON_PrintUnformatted(msg);
    if (s != NULL) {
        write(s);
        cJSON_free(s);
    }
    cJSON_Delete(msg);
}

/* `data` is taken over (may be NULL). */
static void send_error(mcp_write_fn write, const cJSON *id, int code, const char *message, cJSON *data)
{
    cJSON *m = new_reply(id);
    cJSON *e = cJSON_AddObjectToObject(m, "error");
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    if (data != NULL) {
        cJSON_AddItemToObject(e, "data", data);
    }
    send(write, m);
}

/* `result` is taken over. Modern results also say what they are and who sent them. */
static void send_result(mcp_write_fn write, const cJSON *id, cJSON *result, bool modern)
{
    if (modern) {
        cJSON_AddStringToObject(result, "resultType", "complete");
        cJSON *meta = cJSON_GetObjectItemCaseSensitive(result, "_meta");
        if (meta == NULL) {
            meta = cJSON_AddObjectToObject(result, "_meta");
        }
        cJSON_AddItemToObject(meta, META_SERVER_INFO, server_info());
    }
    cJSON *m = new_reply(id);
    cJSON_AddItemToObject(m, "result", result);
    send(write, m);
}

static cJSON *tools_capability(void)
{
    cJSON *caps = cJSON_CreateObject();
    cJSON *tools = cJSON_AddObjectToObject(caps, "tools");
    cJSON_AddFalseToObject(tools, "listChanged");
    return caps;
}

static void handle_initialize(mcp_write_fn write, const cJSON *id, const cJSON *params)
{
    const cJSON *req = params ? cJSON_GetObjectItemCaseSensitive(params, "protocolVersion") : NULL;
    const char *version = LEGACY_VERSIONS[0];
    if (cJSON_IsString(req) && in_list(req->valuestring, LEGACY_VERSIONS, COUNT(LEGACY_VERSIONS))) {
        version = req->valuestring;     /* speak the client's version when we know it */
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "protocolVersion", version);
    cJSON_AddItemToObject(r, "capabilities", tools_capability());
    cJSON_AddItemToObject(r, "serverInfo", server_info());
    cJSON_AddStringToObject(r, "instructions", mcp_tools_instructions());
    send_result(write, id, r, false);
}

static void dispatch(mcp_write_fn write, const cJSON *id, const char *method, const cJSON *params)
{
    if (params != NULL && !cJSON_IsObject(params)) {
        send_error(write, id, ERR_INVALID_PARAMS, "params must be an object", NULL);
        return;
    }
    if (strcmp(method, "initialize") == 0) {
        handle_initialize(write, id, params);
        return;
    }

    /* modern requests name their version and capabilities in _meta */
    const cJSON *meta = params ? cJSON_GetObjectItemCaseSensitive(params, "_meta") : NULL;
    const cJSON *version = cJSON_IsObject(meta) ? cJSON_GetObjectItemCaseSensitive(meta, META_VERSION) : NULL;
    bool modern = false;
    if (version != NULL) {
        if (!cJSON_IsString(version)) {
            send_error(write, id, ERR_INVALID_PARAMS, "_meta." META_VERSION " must be a string", NULL);
            return;
        }
        if (!in_list(version->valuestring, MODERN_VERSIONS, COUNT(MODERN_VERSIONS))) {
            cJSON *data = cJSON_CreateObject();
            cJSON *supported = version_list(MODERN_VERSIONS, COUNT(MODERN_VERSIONS));
            cJSON_AddItemToArray(supported, cJSON_CreateString(LEGACY_VERSIONS[0]));
            cJSON_AddItemToObject(data, "supported", supported);
            cJSON_AddStringToObject(data, "requested", version->valuestring);
            send_error(write, id, ERR_UNSUPPORTED_VER, "Unsupported protocol version", data);
            return;
        }
        if (!cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(meta, META_CLIENT_CAPS))) {
            send_error(write, id, ERR_INVALID_PARAMS, "Missing _meta." META_CLIENT_CAPS, NULL);
            return;
        }
        modern = true;
    }

    cJSON *result = NULL;
    bool cacheable = false;
    if (strcmp(method, "server/discover") == 0) {
        if (!modern) {
            send_error(write, id, ERR_INVALID_PARAMS, "server/discover needs _meta." META_VERSION, NULL);
            return;
        }
        result = cJSON_CreateObject();
        cJSON_AddItemToObject(result, "supportedVersions", version_list(MODERN_VERSIONS, COUNT(MODERN_VERSIONS)));
        cJSON_AddItemToObject(result, "capabilities", tools_capability());
        cJSON_AddStringToObject(result, "instructions", mcp_tools_instructions());
        cacheable = true;
    } else if (strcmp(method, "ping") == 0) {
        result = cJSON_CreateObject();
    } else if (strcmp(method, "tools/list") == 0) {
        result = cJSON_CreateObject();
        cJSON_AddItemToObject(result, "tools", mcp_tools_list());
        cacheable = true;
    } else if (strcmp(method, "tools/call") == 0) {
        const cJSON *name = params ? cJSON_GetObjectItemCaseSensitive(params, "name") : NULL;
        const cJSON *args = params ? cJSON_GetObjectItemCaseSensitive(params, "arguments") : NULL;
        if (!cJSON_IsString(name)) {
            send_error(write, id, ERR_INVALID_PARAMS, "tools/call needs params.name", NULL);
            return;
        }
        if (args != NULL && !cJSON_IsObject(args) && !cJSON_IsNull(args)) {
            send_error(write, id, ERR_INVALID_PARAMS, "params.arguments must be an object", NULL);
            return;
        }
        result = mcp_tools_call(name->valuestring, cJSON_IsObject(args) ? args : NULL);
        if (result == NULL) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Unknown tool: %.60s", name->valuestring);
            send_error(write, id, ERR_INVALID_PARAMS, msg, NULL);
            return;
        }
    } else {
        char msg[96];
        snprintf(msg, sizeof(msg), "Method not found: %.60s", method);
        send_error(write, id, ERR_NO_METHOD, msg, NULL);
        return;
    }
    if (modern && cacheable) {
        cJSON_AddNumberToObject(result, "ttlMs", LIST_TTL_MS);
        cJSON_AddStringToObject(result, "cacheScope", "public");    /* the same for every caller */
    }
    send_result(write, id, result, modern);
}

void mcp_server_handle(const char *line, mcp_write_fn write)
{
    /* anything at all from the host (the bridge's heartbeat ping included) means it is there */
    atomic_store(&s_last_rx_ms, (unsigned)mcp_now_ms());
    atomic_store(&s_any_rx, true);

    if (json_depth(line, MCP_DEPTH_MAX) > MCP_DEPTH_MAX) {
        send_error(write, NULL, ERR_INVALID_REQUEST, "Message nested too deeply", NULL);
        return;
    }
    cJSON *msg = cJSON_Parse(line);
    if (msg == NULL) {
        send_error(write, NULL, ERR_PARSE, "Parse error", NULL);
        return;
    }
    if (!cJSON_IsObject(msg)) {
        send_error(write, NULL, ERR_INVALID_REQUEST,
                   cJSON_IsArray(msg) ? "Batches are not supported" : "Invalid Request", NULL);
        cJSON_Delete(msg);
        return;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(msg, "method");
    const cJSON *jsonrpc = cJSON_GetObjectItemCaseSensitive(msg, "jsonrpc");
    const bool id_ok = cJSON_IsNumber(id) || cJSON_IsString(id);

    if (!cJSON_IsString(jsonrpc) || strcmp(jsonrpc->valuestring, "2.0") != 0 || !cJSON_IsString(method)) {
        /* Not a request or notification. A reply from the client (it sends
         * none to us) would land here too; it needs no answer. */
        if (id_ok && method != NULL) {
            send_error(write, id, ERR_INVALID_REQUEST, "Invalid Request", NULL);
        }
    } else if (id != NULL && !id_ok) {
        send_error(write, NULL, ERR_INVALID_REQUEST, "id must be a string or a number", NULL);
    } else if (id_ok) {
        dispatch(write, id, method->valuestring, cJSON_GetObjectItemCaseSensitive(msg, "params"));
    }
    /* else: a notification (initialized, cancelled, ...): nothing to do or say */
    cJSON_Delete(msg);
}

int mcp_server_feed(const void *data, size_t n, mcp_write_fn write)
{
    static char line[MCP_LINE_MAX + 1];
    static size_t len;
    static bool overflow;
    int dropped = 0;
    const char *p = data;
    for (size_t i = 0; i < n; i++) {
        const char c = p[i];
        if (c == '\n' || c == '\r') {
            if (overflow) {
                dropped++;
            } else if (len > 0) {
                line[len] = '\0';
                mcp_server_handle(line, write);
            }
            len = 0;
            overflow = false;
        } else if (len < MCP_LINE_MAX) {
            line[len++] = c;
        } else {
            overflow = true;
        }
    }
    return dropped;
}

char *mcp_server_ready_message(void)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "jsonrpc", "2.0");
    cJSON_AddStringToObject(m, "method", "tong/ready");
    cJSON_AddItemToObject(m, "params", server_info());
    char *s = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    return s;
}
