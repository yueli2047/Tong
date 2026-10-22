/*
 * The tools Tong offers to a model, and what they do.
 *
 * Every name list in a schema (states, expressions, actions) and every
 * action description is read from the face component at run time, so a
 * state added in the avatar lab or an action added to actions.json shows up
 * here with no change to this file.
 *
 * Bad arguments are answered with isError = true and a message that says
 * what is allowed, so the model can fix its call and try again.
 *
 * Besides the tools, this file keeps the face's "life" between calls:
 *   - a face set through MCP eases back to the resting face
 *     (FACE_REST_EXPRESSION) after hold_s seconds
 *   - a pat on the head (the YES button while online) plays a short happy
 *     reaction, then restores the face it interrupted; pats are counted and
 *     reported in the next get_status or express result
 * All of it runs in the one task that handles MCP messages (mcp_tools_tick
 * is called from the same loop), so only the two request flags are shared.
 */
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "mcp_priv.h"
#include "face.h"
#include "oled.h"

#define TEXT_MAX            120     /* characters accepted by the display tools */
#define EXPR_TRANSITION_MAX 5000
#define SEQ_TRANSITION_MAX  10000
#define SEQ_HOLD_MAX        60000
#define HOLD_DEFAULT_S      10      /* a face set through MCP stays this long, then rest */
#define STR_(x)             #x
#define STR(x)              STR_(x)
#define HOLD_MAX_S          3600
#define RESTORE_MS          400     /* easing back to the face a pat interrupted */
#define PAT_MS              2800    /* how long the happy reaction to a pat lasts */

static const char INSTRUCTIONS[] =
    "Tong is a small desk robot: an animated face on a 2-inch screen, a neck that can turn "
    "and tilt (two servos), and a 128x64 text display under the face. It is your body on the "
    "user's desk: use it to show how you feel and what you mean while you talk.\n"
    "In most replies, call express once: a face that matches your feeling (a looping state "
    "such as happy or thinking, or a single expression), optionally a head gesture (nod, "
    "shake, tilt, ...), and a short English gist of your key point for the display: plain "
    "ASCII words, even when the conversation is in another language (it has no other font); "
    "it wraps lines by itself, and under 60 characters fits on one screen. The face goes back "
    "to its resting face (" FACE_REST_EXPRESSION ", facing forward) by itself after hold_s seconds (" STR(HOLD_DEFAULT_S) " by "
    "default); use a longer hold_s for something that should last, such as thinking during "
    "long work.\n"
    "The user can pat Tong on the head; express and get_status tell you how often since you "
    "last looked, so you can react.";

const char *mcp_tools_instructions(void)
{
    return INSTRUCTIONS;
}

/* ---------- life between calls: auto-return and pats ---------- */

static atomic_int s_pat_requests;       /* set from any task by mcp_pat() */
static atomic_int s_cancel_request;     /* set from any task by mcp_cancel_auto_return() */

static int64_t s_rest_due;              /* ease back to the resting face at this time; 0 = never */
static int64_t s_pat_until;             /* happy reaction in progress until; 0 = none */
static face_showing_t s_prev_kind;      /* the face a pat interrupted */
static const char *s_prev_name;
static int s_pats;                      /* since the model last looked */
static int64_t s_last_pat_ms = -1;

void mcp_pat(void)
{
    atomic_fetch_add(&s_pat_requests, 1);
}

void mcp_cancel_auto_return(void)
{
    atomic_store(&s_cancel_request, 1);
}

static void go_rest(void)
{
    if (face_show_expression(FACE_REST_EXPRESSION, RESTORE_MS) != ESP_OK && face_expression_count() > 0) {
        face_show_expression(face_expression_name(0), RESTORE_MS);
    }
}

static void start_pat(int64_t now)
{
    face_status_t st;
    if (s_pat_until == 0 && face_get_status(&st) == ESP_OK) {
        s_prev_kind = st.kind;          /* only the first pat of a burst remembers */
        s_prev_name = st.name;
    }
    static const face_step_t happy[] = {
        { "joyful-down-right", 150, 1100, FACE_EASE_SNAPPY },
        { "joyful-wide",       300, 900,  FACE_EASE_SPRING },
    };
    face_play_sequence(happy, 2, FACE_LOOP_ONCE);
    face_do_action("bounce");
    s_pat_until = now + PAT_MS;
}

static void end_pat(int64_t now)
{
    s_pat_until = 0;
    if (s_rest_due != 0 && now >= s_rest_due) {
        s_rest_due = 0;
        go_rest();
    } else if (s_prev_kind == FACE_SHOWING_STATE && s_prev_name != NULL) {
        face_play_state(s_prev_name);
    } else if (s_prev_kind == FACE_SHOWING_EXPRESSION && s_prev_name != NULL) {
        face_show_expression(s_prev_name, RESTORE_MS);
    } else {
        go_rest();                      /* a sequence cannot be resumed part-way */
    }
}

void mcp_tools_tick(int64_t now)
{
    if (atomic_exchange(&s_cancel_request, 0)) {
        s_rest_due = 0;
        s_pat_until = 0;
    }
    const int pats = atomic_exchange(&s_pat_requests, 0);
    if (pats > 0) {
        s_pats += pats;
        s_last_pat_ms = now;
        start_pat(now);
    }
    if (s_pat_until != 0) {
        if (now >= s_pat_until) {
            end_pat(now);
        }
    } else if (s_rest_due != 0 && now >= s_rest_due) {
        s_rest_due = 0;
        go_rest();
    }
}

/* A face was just set through MCP: it overrides a pat in progress and
 * returns to the resting face after `hold_s` seconds (0: stays), counted from `shown_ms`
 * from now, when it is fully on screen. */
static void face_set_by_model(int shown_ms, int hold_s)
{
    const int64_t now = mcp_now_ms();
    s_pat_until = 0;
    s_rest_due = hold_s > 0 ? now + shown_ms + (int64_t)hold_s * 1000 : 0;
}

/* "Patted 2 times since you last looked (last 15 s ago)." into buf, and clears the count. */
static const char *take_pats(char *buf, size_t size)
{
    buf[0] = '\0';
    if (s_pats > 0) {
        snprintf(buf, size, " The user patted you on the head %d time%s since you last looked "
                 "(last %d s ago).", s_pats, s_pats > 1 ? "s" : "",
                 (int)((mcp_now_ms() - s_last_pat_ms) / 1000));
        s_pats = 0;
    }
    return buf;
}

/* ---------- small builders ---------- */

typedef const char *(*name_at_fn)(int index);

static void add_names(cJSON *array, name_at_fn name_at, int count)
{
    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(array, cJSON_CreateString(name_at(i)));
    }
}

static cJSON *names_enum(name_at_fn name_at, int count)
{
    cJSON *a = cJSON_CreateArray();
    add_names(a, name_at, count);
    return a;
}

static cJSON *str_prop(cJSON *props, const char *key, const char *desc)
{
    cJSON *p = cJSON_AddObjectToObject(props, key);
    cJSON_AddStringToObject(p, "type", "string");
    cJSON_AddStringToObject(p, "description", desc);
    return p;
}

static cJSON *int_prop(cJSON *props, const char *key, int min, int max, int def, const char *desc)
{
    cJSON *p = cJSON_AddObjectToObject(props, key);
    cJSON_AddStringToObject(p, "type", "integer");
    cJSON_AddNumberToObject(p, "minimum", min);
    cJSON_AddNumberToObject(p, "maximum", max);
    cJSON_AddNumberToObject(p, "default", def);
    cJSON_AddStringToObject(p, "description", desc);
    return p;
}

static void hold_prop(cJSON *props)
{
    int_prop(props, "hold_s", 0, HOLD_MAX_S, HOLD_DEFAULT_S,
             "Seconds to keep this face before Tong eases back to its resting face (" FACE_REST_EXPRESSION ") on its own "
             "(counted from when it is fully shown). 0 keeps it until the face is changed.");
}

static cJSON *object_schema(cJSON **props)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    *props = cJSON_AddObjectToObject(s, "properties");
    return s;
}

static void require(cJSON *schema, const char *key)
{
    cJSON *r = cJSON_GetObjectItemCaseSensitive(schema, "required");
    if (r == NULL) {
        r = cJSON_AddArrayToObject(schema, "required");
    }
    cJSON_AddItemToArray(r, cJSON_CreateString(key));
}

static cJSON *no_params(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON_AddFalseToObject(s, "additionalProperties");
    return s;
}

/* ---------- results (one MCP task: the buffers can be static) ---------- */

static cJSON *result(bool error, const char *fmt, va_list ap)
{
    static char text[1600];
    vsnprintf(text, sizeof(text), fmt, ap);
    cJSON *r = cJSON_CreateObject();
    cJSON *content = cJSON_AddArrayToObject(r, "content");
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "text");
    cJSON_AddStringToObject(t, "text", text);
    cJSON_AddItemToArray(content, t);
    cJSON_AddBoolToObject(r, "isError", error);
    return r;
}

static cJSON *ok(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    cJSON *r = result(false, fmt, ap);
    va_end(ap);
    return r;
}

static cJSON *fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    cJSON *r = result(true, fmt, ap);
    va_end(ap);
    return r;
}

/* "a, b, c" into buf (truncated with "..." if it does not fit). */
static const char *join_names(char *buf, size_t size, name_at_fn name_at, int count)
{
    size_t len = 0;
    buf[0] = '\0';
    for (int i = 0; i < count; i++) {
        const int n = snprintf(buf + len, size - len, "%s%s", i ? ", " : "", name_at(i));
        if (n < 0 || (size_t)n >= size - len) {
            snprintf(buf + (size > 4 ? size - 4 : 0), 4, "...");
            break;
        }
        len += (size_t)n;
    }
    return buf;
}

static int find(const char *name, name_at_fn name_at, int count)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(name, name_at(i)) == 0) {
            return i;
        }
    }
    return -1;
}

static const cJSON *arg(const cJSON *args, const char *key)
{
    const cJSON *v = args ? cJSON_GetObjectItemCaseSensitive(args, key) : NULL;
    return cJSON_IsNull(v) ? NULL : v;
}

/* Integer argument in [min, max], `def` when absent. False if present but invalid. */
static bool int_arg(const cJSON *args, const char *key, int min, int max, int def, int *out)
{
    const cJSON *v = arg(args, key);
    if (v == NULL) {
        *out = def;
        return true;
    }
    if (!cJSON_IsNumber(v) || !(v->valuedouble >= min && v->valuedouble <= max) ||
        v->valuedouble != (double)(int)v->valuedouble) {
        return false;
    }
    *out = (int)v->valuedouble;
    return true;
}

static const char *seconds(int ms, char *buf, size_t size)
{
    snprintf(buf, size, "%d.%d s", ms / 1000, (ms % 1000) / 100);
    return buf;
}

static const char *back_to_rest(int hold_s, char *buf, size_t size)
{
    if (hold_s > 0) {
        snprintf(buf, size, "back to " FACE_REST_EXPRESSION " %d s after it is shown", hold_s);
    } else {
        snprintf(buf, size, "it stays until the face is changed");
    }
    return buf;
}

/* One UTF-8 character; U+FFFD for a broken sequence. */
static uint32_t next_char(const unsigned char **pp)
{
    const unsigned char *p = *pp;
    uint32_t c = *p++;
    int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (c >= 0x80 && more == 0) {
        *pp = p;
        return 0xFFFD;                      /* stray continuation byte */
    }
    c &= more == 3 ? 0x07 : more == 2 ? 0x0F : more == 1 ? 0x1F : 0x7F;
    for (; more > 0; more--, p++) {
        if ((*p & 0xC0) != 0x80) {
            *pp = p;
            return 0xFFFD;
        }
        c = (c << 6) | (*p & 0x3F);
    }
    *pp = p;
    return c;
}

/* An emoji or dingbat, not counting skin-tone marks (they ride on one). */
static bool is_emoji(uint32_t c)
{
    return (c >= 0x1F000 && c <= 0x1FAFF && !(c >= 0x1F3FB && c <= 0x1F3FF)) || (c >= 0x2600 && c <= 0x27BF);
}

/* What a non-ASCII character looks like in ASCII: a look-alike, "" for
 * something best left out (emoji, invisible marks), NULL for no idea. */
static const char *ascii_for(uint32_t c)
{
    switch (c) {
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: case 0x02BC:
        return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: case 0x00AB: case 0x00BB:
    case 0x300C: case 0x300D:
        return "\"";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
        return "-";
    case 0x2026: return "...";
    case 0x2022: return "*";
    case 0x00B7: case 0x3002: return ".";
    case 0x3001: return ",";
    case 0x00D7: return "x";
    case 0x2190: return "<-";
    case 0x2192: return "->";
    case 0x21D2: return "=>";
    case 0x00A0: case 0x202F: case 0x205F: case 0x3000: return " ";
    case 0x200B: case 0x200C: case 0x200D: case 0x2060: case 0xFEFF: return "";
    default: break;
    }
    static char one[2];
    if (c >= 0x2000 && c <= 0x200A) {
        return " ";                         /* the typographic spaces */
    }
    if (c >= 0xC0 && c <= 0xFF) {           /* accented Latin letters: café -> cafe */
        static const char base[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUY?saaaaaaaceeeeiiiidnooooo/ouuuuy?y";
        one[0] = base[c - 0xC0];
        return one[0] == '?' ? NULL : one;
    }
    if (c >= 0xFF01 && c <= 0xFF5E) {       /* full-width ASCII, e.g. a Chinese comma */
        one[0] = (char)(c - 0xFEE0);
        return one;
    }
    if ((c >= 0xFE00 && c <= 0xFE0F) || (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF)) {
        return "";                          /* emoji, their style marks, dingbats */
    }
    return NULL;
}

/* Text for the display, whose font is ASCII only. What a model sends is not
 * always plain ASCII, so:
 *   - a line break written as the two characters '\' 'n' (an escape typed
 *     inside the string) becomes a real one;
 *   - curly quotes, dashes, the ellipsis, odd spaces and full-width
 *     punctuation become their ASCII look-alikes;
 *   - emoji and invisible marks are left out, anything else becomes '?';
 *     *lost counts those two;
 *   - blanks at both ends are trimmed.
 * NULL if longer than TEXT_MAX characters. */
static const char *display_text(const char *in, int *lost)
{
    static char buf[3 * TEXT_MAX + 1];      /* "..." is the longest look-alike */
    size_t len = 0, chars = 0;
    *lost = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p;) {
        const uint32_t c = next_char(&p);
        if (++chars > TEXT_MAX) {
            return NULL;
        }
        const char *out;
        char one[2] = { 0, 0 };
        if (c == '\\' && (*p == 'n' || *p == 't' || *p == 'r')) {
            out = *p == 'n' ? "\n" : *p == 't' ? " " : "";
            p++;
        } else if (c == '\n' || (c >= 0x20 && c <= 0x7E)) {
            one[0] = (char)c;
            out = one;
        } else if (c == '\t') {
            out = " ";
        } else if (c < 0x20 || c == 0x7F) {
            out = "";                       /* other control characters, '\r' included */
        } else if ((out = ascii_for(c)) == NULL) {
            out = "?";
            (*lost)++;
        } else if (out[0] == '\0' && is_emoji(c)) {
            (*lost)++;                      /* tell the model it was not shown */
        }
        /* no double spaces where something was left out, none around a line break */
        if (out[0] == ' ' && len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\n')) {
            continue;
        }
        if (out[0] == '\n' && len > 0 && buf[len - 1] == ' ') {
            len--;
        }
        const size_t n = strlen(out);
        memcpy(buf + len, out, n);
        len += n;
    }
    while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\n')) {
        len--;
    }
    buf[len] = '\0';
    size_t start = 0;
    while (buf[start] == ' ' || buf[start] == '\n') {
        start++;
    }
    return buf + start;
}

static const char *lost_note(int lost, char *buf, size_t size)
{
    snprintf(buf, size, " %d character%s the display cannot draw (ASCII only) %s left out or shown as '?'.",
             lost, lost == 1 ? "" : "s", lost == 1 ? "was" : "were");
    return buf;
}

static const char *const EASE_NAMES[] = { "smooth", "snappy", "spring" };
static const char *const LOOP_NAMES[] = { "once", "repeat", "ping_pong" };

static cJSON *string_enum(const char *const *names, int n)
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON_AddItemToArray(a, cJSON_CreateString(names[i]));
    }
    return a;
}

/* ---------- express ---------- */

static cJSON *schema_express(void)
{
    cJSON *props, *s = object_schema(&props);
    cJSON *face = str_prop(props, "face",
        "How you feel: a state (loops, e.g. happy, thinking, curious, sad, sleeping) or a "
        "single expression (holds one look, e.g. joyful-wide, eyes-closed).");
    cJSON *names = cJSON_AddArrayToObject(face, "enum");
    add_names(names, face_state_name, face_state_count());
    add_names(names, face_expression_name, face_expression_count());
    cJSON *gesture = str_prop(props, "gesture",
        "A one-shot head gesture on top of the face (see do_action for what each one means).");
    cJSON_AddItemToObject(gesture, "enum", names_enum(face_action_name, face_action_count()));
    cJSON *text = str_prop(props, "text",
        "Short plain English gist of your key point for the display (ASCII only, no emoji). It "
        "wraps by itself, so no line breaks; about 60 characters fit at once and longer text "
        "scrolls, so under 60 reads best (at most 120). An empty string clears it.");
    cJSON_AddNumberToObject(text, "maxLength", TEXT_MAX);
    hold_prop(props);
    return s;
}

static cJSON *call_express(const cJSON *args)
{
    static char names[1024], pats[160];
    const cJSON *face = arg(args, "face");
    const cJSON *gesture = arg(args, "gesture");
    const cJSON *text = arg(args, "text");

    /* check everything first, so a bad argument changes nothing */
    int state = -1, expr = -1, action = -1;
    if (face != NULL) {
        if (cJSON_IsString(face)) {
            state = find(face->valuestring, face_state_name, face_state_count());
            expr = state < 0 ? find(face->valuestring, face_expression_name, face_expression_count()) : -1;
        }
        if (state < 0 && expr < 0) {
            char *p = names;
            join_names(p, 400, face_state_name, face_state_count());
            p += strlen(p);
            snprintf(p, sizeof(names) - (size_t)(p - names), "; or an expression: ");
            p += strlen(p);
            join_names(p, sizeof(names) - (size_t)(p - names), face_expression_name, face_expression_count());
            return fail("'face' must be a state: %s", names);
        }
    }
    if (gesture != NULL) {
        action = cJSON_IsString(gesture) ? find(gesture->valuestring, face_action_name, face_action_count()) : -1;
        if (action < 0) {
            return fail("'gesture' must be one of: %s",
                        join_names(names, sizeof(names), face_action_name, face_action_count()));
        }
    }
    const char *shown = NULL;
    int lost = 0;
    if (text != NULL) {
        if (!cJSON_IsString(text)) {
            return fail("'text' must be a string (an empty string clears the display).");
        }
        shown = display_text(text->valuestring, &lost);
        if (shown == NULL) {
            return fail("'text' is too long: at most %d characters.", TEXT_MAX);
        }
    }
    int hold_s;
    if (!int_arg(args, "hold_s", 0, HOLD_MAX_S, HOLD_DEFAULT_S, &hold_s)) {
        return fail("'hold_s' must be a whole number of seconds from 0 to %d.", HOLD_MAX_S);
    }
    if (face == NULL && gesture == NULL && text == NULL) {
        return fail("Give at least one of face, gesture or text.");
    }

    /* then act */
    char said[400], t[16], rest[64];
    size_t len = 0;
    said[0] = '\0';
    if (state >= 0) {
        if (face_play_state(face->valuestring) != ESP_OK) {
            return fail("The face is not running.");
        }
        face_set_by_model(0, hold_s);
        len += (size_t)snprintf(said + len, sizeof(said) - len, " Face: state '%s' (%s).",
                                face->valuestring, back_to_rest(hold_s, rest, sizeof(rest)));
    } else if (expr >= 0) {
        if (face_show_expression(face->valuestring, FACE_TRANSITION_DEFAULT_MS) != ESP_OK) {
            return fail("The face is not running.");
        }
        face_set_by_model(FACE_TRANSITION_DEFAULT_MS, hold_s);
        len += (size_t)snprintf(said + len, sizeof(said) - len, " Face: expression '%s' (%s).",
                                face->valuestring, back_to_rest(hold_s, rest, sizeof(rest)));
    }
    if (action >= 0) {
        face_do_action(gesture->valuestring);
        len += (size_t)snprintf(said + len, sizeof(said) - len, " Gesture: '%s' (%s).", gesture->valuestring,
                                seconds(face_action_duration_ms(action), t, sizeof(t)));
    }
    if (shown != NULL) {
        char note[100];
        oled_show_text(shown);
        snprintf(said + len, sizeof(said) - len, "%s%s", shown[0] == '\0' ? " Display cleared." : " Text shown.",
                 lost > 0 ? lost_note(lost, note, sizeof(note)) : "");
    }
    return ok("Done.%s%s", said, take_pats(pats, sizeof(pats)));
}

/* ---------- show_expression ---------- */

static cJSON *schema_show_expression(void)
{
    cJSON *props, *s = object_schema(&props);
    cJSON *name = str_prop(props, "name",
        "Expression to show. Names describe the look, e.g. joyful-wide (big happy eyes), "
        "eyes-closed, far-right-glance (looking far to its own right).");
    cJSON_AddItemToObject(name, "enum", names_enum(face_expression_name, face_expression_count()));
    int_prop(props, "transition_ms", 0, EXPR_TRANSITION_MAX, FACE_TRANSITION_DEFAULT_MS,
             "How long to ease into it; 0 jumps there at once.");
    hold_prop(props);
    require(s, "name");
    return s;
}

static cJSON *call_show_expression(const cJSON *args)
{
    static char names[1024];
    char rest[64];
    const cJSON *name = arg(args, "name");
    if (!cJSON_IsString(name) || find(name->valuestring, face_expression_name, face_expression_count()) < 0) {
        return fail("'name' must be one of: %s",
                    join_names(names, sizeof(names), face_expression_name, face_expression_count()));
    }
    int ms, hold_s;
    if (!int_arg(args, "transition_ms", 0, EXPR_TRANSITION_MAX, FACE_TRANSITION_DEFAULT_MS, &ms)) {
        return fail("'transition_ms' must be a whole number from 0 to %d.", EXPR_TRANSITION_MAX);
    }
    if (!int_arg(args, "hold_s", 0, HOLD_MAX_S, HOLD_DEFAULT_S, &hold_s)) {
        return fail("'hold_s' must be a whole number of seconds from 0 to %d.", HOLD_MAX_S);
    }
    if (face_show_expression(name->valuestring, ms) != ESP_OK) {
        return fail("The face is not running.");
    }
    face_set_by_model(ms, hold_s);
    return ok("Showing expression '%s' (easing in over %d ms); %s.",
              name->valuestring, ms, back_to_rest(hold_s, rest, sizeof(rest)));
}

/* ---------- play_state ---------- */

static cJSON *schema_play_state(void)
{
    cJSON *props, *s = object_schema(&props);
    cJSON *name = str_prop(props, "name", "State to play.");
    cJSON_AddItemToObject(name, "enum", names_enum(face_state_name, face_state_count()));
    hold_prop(props);
    require(s, "name");
    return s;
}

static cJSON *call_play_state(const cJSON *args)
{
    static char names[1024];
    char rest[64];
    const cJSON *name = arg(args, "name");
    if (!cJSON_IsString(name) || find(name->valuestring, face_state_name, face_state_count()) < 0) {
        return fail("'name' must be one of: %s",
                    join_names(names, sizeof(names), face_state_name, face_state_count()));
    }
    int hold_s;
    if (!int_arg(args, "hold_s", 0, HOLD_MAX_S, HOLD_DEFAULT_S, &hold_s)) {
        return fail("'hold_s' must be a whole number of seconds from 0 to %d.", HOLD_MAX_S);
    }
    if (face_play_state(name->valuestring) != ESP_OK) {
        return fail("The face is not running.");
    }
    face_set_by_model(0, hold_s);
    return ok("Playing state '%s' (it loops; %s).", name->valuestring, back_to_rest(hold_s, rest, sizeof(rest)));
}

/* ---------- play_sequence ---------- */

static cJSON *schema_play_sequence(void)
{
    cJSON *props, *s = object_schema(&props);

    cJSON *steps = cJSON_AddObjectToObject(props, "steps");
    cJSON_AddStringToObject(steps, "type", "array");
    cJSON_AddNumberToObject(steps, "minItems", 1);
    cJSON_AddNumberToObject(steps, "maxItems", FACE_SEQ_MAX_STEPS);
    cJSON_AddStringToObject(steps, "description",
        "The expressions in order. Each step eases into its expression over transition_ms, "
        "then holds it for hold_ms.");
    cJSON *iprops, *item = object_schema(&iprops);
    cJSON *expr = str_prop(iprops, "expression", "Expression for this step.");
    cJSON_AddItemToObject(expr, "enum", names_enum(face_expression_name, face_expression_count()));
    int_prop(iprops, "transition_ms", 0, SEQ_TRANSITION_MAX, 400, "Time to ease into this expression.");
    int_prop(iprops, "hold_ms", 0, SEQ_HOLD_MAX, 800, "Time to hold it before the next step.");
    cJSON *ease = str_prop(iprops, "ease",
        "How the step moves: smooth (ease in and out), snappy (fast start, gentle stop), "
        "spring (overshoots a little and settles).");
    cJSON_AddItemToObject(ease, "enum", string_enum(EASE_NAMES, 3));
    cJSON_AddStringToObject(ease, "default", "smooth");
    require(item, "expression");
    cJSON_AddItemToObject(steps, "items", item);

    cJSON *loop = str_prop(props, "loop",
        "once: play through and stay on the last step. repeat: 1,2,3,1,2,3... "
        "ping_pong: 1,2,3,2,1,2...");
    cJSON_AddItemToObject(loop, "enum", string_enum(LOOP_NAMES, 3));
    cJSON_AddStringToObject(loop, "default", "once");
    hold_prop(props);
    require(s, "steps");
    return s;
}

static cJSON *call_play_sequence(const cJSON *args)
{
    static char names[1024];
    const cJSON *steps = arg(args, "steps");
    const int n = cJSON_IsArray(steps) ? cJSON_GetArraySize(steps) : 0;
    if (n < 1 || n > FACE_SEQ_MAX_STEPS) {
        return fail("'steps' must be a list of 1 to %d steps.", FACE_SEQ_MAX_STEPS);
    }

    face_loop_t loop = FACE_LOOP_ONCE;
    const cJSON *lp = arg(args, "loop");
    if (lp != NULL) {
        int k = cJSON_IsString(lp) ? -1 : -2;
        for (int i = 0; k == -1 && i < 3; i++) {
            if (strcmp(lp->valuestring, LOOP_NAMES[i]) == 0) {
                k = i;
            }
        }
        if (k < 0) {
            return fail("'loop' must be once, repeat or ping_pong.");
        }
        loop = (face_loop_t)k;
    }
    int hold_s;
    if (!int_arg(args, "hold_s", 0, HOLD_MAX_S, HOLD_DEFAULT_S, &hold_s)) {
        return fail("'hold_s' must be a whole number of seconds from 0 to %d.", HOLD_MAX_S);
    }

    face_step_t seq[FACE_SEQ_MAX_STEPS];
    int total_ms = 0;
    for (int i = 0; i < n; i++) {
        const cJSON *st = cJSON_GetArrayItem(steps, i);
        if (!cJSON_IsObject(st)) {
            return fail("Step %d must be an object with an 'expression'.", i + 1);
        }
        const cJSON *ex = cJSON_GetObjectItemCaseSensitive(st, "expression");
        if (!cJSON_IsString(ex) || find(ex->valuestring, face_expression_name, face_expression_count()) < 0) {
            return fail("Step %d: 'expression' must be one of: %s", i + 1,
                        join_names(names, sizeof(names), face_expression_name, face_expression_count()));
        }
        int tr, hold;
        if (!int_arg(st, "transition_ms", 0, SEQ_TRANSITION_MAX, 400, &tr)) {
            return fail("Step %d: 'transition_ms' must be a whole number from 0 to %d.", i + 1, SEQ_TRANSITION_MAX);
        }
        if (!int_arg(st, "hold_ms", 0, SEQ_HOLD_MAX, 800, &hold)) {
            return fail("Step %d: 'hold_ms' must be a whole number from 0 to %d.", i + 1, SEQ_HOLD_MAX);
        }
        face_ease_t ease = FACE_EASE_SMOOTH;
        const cJSON *e = arg(st, "ease");
        if (e != NULL) {
            int k = -1;
            for (int j = 0; cJSON_IsString(e) && j < 3; j++) {
                if (strcmp(e->valuestring, EASE_NAMES[j]) == 0) {
                    k = j;
                }
            }
            if (k < 0) {
                return fail("Step %d: 'ease' must be smooth, snappy or spring.", i + 1);
            }
            ease = (face_ease_t)k;
        }
        seq[i] = (face_step_t){ ex->valuestring, (uint16_t)tr, (uint16_t)hold, ease };
        total_ms += tr + hold;
    }
    if (loop != FACE_LOOP_ONCE && total_ms == 0) {
        return fail("A repeating sequence needs some time per cycle: give the steps a transition_ms or hold_ms.");
    }
    if (face_play_sequence(seq, n, loop) != ESP_OK) {
        return fail("The face is not running.");
    }

    char t[16], rest[64];
    if (loop == FACE_LOOP_ONCE) {
        const int play_ms = total_ms - seq[n - 1].hold_ms;
        face_set_by_model(play_ms, hold_s);
        return ok("Playing %d step%s once (about %s), then holding '%s'; %s.", n, n > 1 ? "s" : "",
                  seconds(play_ms, t, sizeof(t)), seq[n - 1].expression, back_to_rest(hold_s, rest, sizeof(rest)));
    }
    face_set_by_model(0, hold_s);
    return ok("Playing %d step%s on %s (cycle about %s); %s.", n, n > 1 ? "s" : "",
              loop == FACE_LOOP_REPEAT ? "repeat" : "ping-pong", seconds(total_ms, t, sizeof(t)),
              back_to_rest(hold_s, rest, sizeof(rest)));
}

/* ---------- do_action / stop_action ---------- */

static cJSON *schema_do_action(void)
{
    cJSON *props, *s = object_schema(&props);
    cJSON *name = str_prop(props, "name", "Gesture to play (see the list in the tool description).");
    cJSON_AddItemToObject(name, "enum", names_enum(face_action_name, face_action_count()));
    require(s, "name");
    return s;
}

static char *describe_do_action(void)
{
    static const char intro[] =
        "Play a one-shot head gesture on top of the current face; the face keeps playing "
        "underneath and the neck servos move with it. A new gesture cuts the previous one "
        "short. Gestures:";
    size_t size = sizeof(intro) + 32;
    for (int i = 0; i < face_action_count(); i++) {
        size += strlen(face_action_name(i)) + strlen(face_action_description(i)) + 24;
    }
    char *d = cJSON_malloc(size);
    if (d == NULL) {
        return NULL;
    }
    size_t len = (size_t)snprintf(d, size, "%s", intro);
    for (int i = 0; i < face_action_count() && len < size; i++) {
        char t[16];
        len += (size_t)snprintf(d + len, size - len, "\n- %s (%s): %s", face_action_name(i),
                                seconds(face_action_duration_ms(i), t, sizeof(t)), face_action_description(i));
    }
    return d;
}

static cJSON *call_do_action(const cJSON *args)
{
    static char names[512];
    const cJSON *name = arg(args, "name");
    const int i = cJSON_IsString(name) ? find(name->valuestring, face_action_name, face_action_count()) : -1;
    if (i < 0) {
        return fail("'name' must be one of: %s", join_names(names, sizeof(names), face_action_name, face_action_count()));
    }
    if (face_do_action(name->valuestring) != ESP_OK) {
        return fail("The face is not running.");
    }
    char t[16];
    return ok("Doing '%s' (%s) on top of the current face.", name->valuestring,
              seconds(face_action_duration_ms(i), t, sizeof(t)));
}

static cJSON *call_stop_action(const cJSON *args)
{
    (void)args;
    face_status_t st;
    if (face_get_status(&st) != ESP_OK) {
        return fail("The face is not running.");
    }
    if (st.action == NULL) {
        return ok("No head gesture was playing.");
    }
    face_stop_action();
    return ok("Stopped '%s'; the head eases back.", st.action);
}

/* ---------- get_status ---------- */

static cJSON *call_get_status(const cJSON *args)
{
    (void)args;
    face_status_t st;
    if (face_get_status(&st) != ESP_OK) {
        return fail("The face is not running.");
    }
    const int64_t now = mcp_now_ms();
    static const char *const kinds[] = { "state", "expression", "sequence" };
    cJSON *s = cJSON_CreateObject();
    cJSON *face = cJSON_AddObjectToObject(s, "face");
    cJSON_AddStringToObject(face, "showing", kinds[st.kind]);
    if (st.name != NULL) {
        cJSON_AddStringToObject(face, "name", st.name);
    } else {
        cJSON_AddNullToObject(face, "name");
    }
    cJSON_AddBoolToObject(face, "finished", st.finished);
    if (s_rest_due != 0) {
        cJSON_AddNumberToObject(face, "back_to_rest_in_s", (double)((s_rest_due - now + 999) / 1000));
    } else {
        cJSON_AddNullToObject(face, "back_to_rest_in_s");
    }
    if (st.action != NULL) {
        cJSON_AddStringToObject(s, "action", st.action);
    } else {
        cJSON_AddNullToObject(s, "action");
    }
    cJSON_AddBoolToObject(s, "servo_follow", face_get_servo_follow());
    cJSON_AddNumberToObject(s, "pats_since_last_check", s_pats);
    if (s_last_pat_ms >= 0) {
        cJSON_AddNumberToObject(s, "last_pat_s_ago", (double)((now - s_last_pat_ms) / 1000));
    } else {
        cJSON_AddNullToObject(s, "last_pat_s_ago");
    }
    s_pats = 0;

    char *text = cJSON_PrintUnformatted(s);
    cJSON *r = ok("%s", text ? text : "{}");
    cJSON_free(text);
    cJSON_AddItemToObject(r, "structuredContent", s);
    return r;
}

/* ---------- show_text ---------- */

static cJSON *schema_show_text(void)
{
    cJSON *props, *s = object_schema(&props);
    cJSON *text = str_prop(props, "text",
        "Short plain English text (the font is ASCII only), at most 120 characters. It wraps by "
        "itself, so line breaks are not needed: about 4 lines of 17 characters (some 60 "
        "characters) fit at once, and longer text scrolls up a line at a time. An empty string "
        "clears the display.");
    cJSON_AddNumberToObject(text, "maxLength", TEXT_MAX);
    require(s, "text");
    return s;
}

static cJSON *call_show_text(const cJSON *args)
{
    const cJSON *text = arg(args, "text");
    if (!cJSON_IsString(text)) {
        return fail("'text' must be a string (an empty string clears the display).");
    }
    int lost;
    const char *shown = display_text(text->valuestring, &lost);
    if (shown == NULL) {
        return fail("'text' is too long: at most %d characters.", TEXT_MAX);
    }
    oled_show_text(shown);
    char note[100];
    return ok("%s%s", shown[0] == '\0' ? "Cleared the display." : "Showing the text.",
              lost > 0 ? lost_note(lost, note, sizeof(note)) : "");
}

/* ---------- set_servo_follow ---------- */

static cJSON *schema_set_servo_follow(void)
{
    cJSON *props, *s = object_schema(&props);
    cJSON *on = cJSON_AddObjectToObject(props, "on");
    cJSON_AddStringToObject(on, "type", "boolean");
    cJSON_AddStringToObject(on, "description",
        "true: the neck turns the device toward where the face looks (the default). "
        "false: the neck eases back to center and the screen draws the whole head motion.");
    require(s, "on");
    return s;
}

static cJSON *call_set_servo_follow(const cJSON *args)
{
    const cJSON *on = arg(args, "on");
    if (!cJSON_IsBool(on)) {
        return fail("'on' must be true or false.");
    }
    if (face_set_servo_follow(cJSON_IsTrue(on)) != ESP_OK) {
        return fail("The face is not running.");
    }
    return ok(cJSON_IsTrue(on) ? "The neck now follows the face." : "The neck eases back to center; the screen draws the head motion.");
}

/* ---------- the table ---------- */

typedef struct {
    const char *name;
    const char *title;
    const char *description;            /* NULL: built by describe() */
    char *(*describe)(void);
    cJSON *(*schema)(void);             /* NULL: no parameters */
    cJSON *(*call)(const cJSON *args);
    bool read_only;
    bool idempotent;
} tool_t;

static const tool_t TOOLS[] = {
    {
        "express", "Express yourself on Tong",
        "Show how you feel and what you mean on Tong in one call; use it once in most replies. "
        "All fields are optional (give at least one): face sets a state or expression that "
        "matches your feeling, gesture adds a head gesture, text puts a short English gist of "
        "your key point on the display. The face returns to rest after hold_s seconds. The "
        "result also tells you if the user patted Tong on the head since you last looked.",
        NULL, schema_express, call_express, false, false,
    },
    {
        "play_state", "Play a state",
        "Play one of the face's looping states: a rhythm of expressions with its own blinking, "
        "e.g. thinking while you work on something, happy, curious, sleeping. Replaces the "
        "current face; returns to rest after hold_s seconds.",
        NULL, schema_play_state, call_play_state, false, true,
    },
    {
        "show_expression", "Show an expression",
        "Ease the face into one expression and hold it there (it keeps blinking). Replaces the "
        "current face; returns to rest after hold_s seconds. Each expression also turns the "
        "head its own way; the neck follows.",
        NULL, schema_show_expression, call_show_expression, false, true,
    },
    {
        "play_sequence", "Play a sequence of expressions",
        "Play expressions in an order you choose, e.g. surprised and then happy. Replaces the "
        "current face. With loop once it stays on the last expression, then returns to rest "
        "after hold_s seconds.",
        NULL, schema_play_sequence, call_play_sequence, false, true,
    },
    {
        "do_action", "Do a head gesture",
        NULL, describe_do_action, schema_do_action, call_do_action, false, false,
    },
    {
        "stop_action", "Stop the head gesture",
        "Cut the current head gesture short; the head eases back in about 0.35 s.",
        NULL, NULL, call_stop_action, false, true,
    },
    {
        "get_status", "Get what Tong is showing",
        "What Tong is showing right now: the face (state, expression or sequence, whether it "
        "has finished, and when it goes back to rest), the head gesture in progress, whether "
        "the neck follows the face, and how often the user patted Tong on the head since you "
        "last looked.",
        NULL, NULL, call_get_status, true, false,
    },
    {
        "show_text", "Show text on the display",
        "Show a short message on the small text display under the face (128x64 pixels). "
        "Replaces the previous text.",
        NULL, schema_show_text, call_show_text, false, true,
    },
    {
        "set_servo_follow", "Turn the neck on or off",
        "Choose whether the neck servos move. Turn it off if the servos have no power or "
        "should stay still; the face still turns on the screen.",
        NULL, schema_set_servo_follow, call_set_servo_follow, false, true,
    },
};

#define TOOL_COUNT ((int)(sizeof(TOOLS) / sizeof(TOOLS[0])))

cJSON *mcp_tools_list(void)
{
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < TOOL_COUNT; i++) {
        const tool_t *t = &TOOLS[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", t->name);
        cJSON_AddStringToObject(o, "title", t->title);
        if (t->description != NULL) {
            cJSON_AddStringToObject(o, "description", t->description);
        } else {
            char *d = t->describe();
            cJSON_AddStringToObject(o, "description", d ? d : t->title);
            cJSON_free(d);
        }
        cJSON_AddItemToObject(o, "inputSchema", t->schema ? t->schema() : no_params());
        cJSON *a = cJSON_AddObjectToObject(o, "annotations");
        cJSON_AddStringToObject(a, "title", t->title);
        cJSON_AddBoolToObject(a, "readOnlyHint", t->read_only);
        cJSON_AddFalseToObject(a, "destructiveHint");
        cJSON_AddBoolToObject(a, "idempotentHint", t->idempotent);
        cJSON_AddFalseToObject(a, "openWorldHint");
        cJSON_AddItemToArray(list, o);
    }
    return list;
}

cJSON *mcp_tools_call(const char *name, const cJSON *args)
{
    for (int i = 0; i < TOOL_COUNT; i++) {
        if (strcmp(name, TOOLS[i].name) == 0) {
            return TOOLS[i].call(args);
        }
    }
    return NULL;
}
