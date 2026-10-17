#include <math.h>
#include "servo.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/mcpwm_prelude.h"
#include "esp_log.h"

static const char *TAG = "servo";

#define SERVO_TASK_PRIORITY 5   // must stay above the LVGL task (esp_lvgl_port default: 4)

// ---- Timing ---------------------------------------------------------------
#define TICK_MS             20                  // aligned with 50 Hz PWM
#define DT_S                (TICK_MS / 1000.0f)
#define MAX_SPEED_DPS       300.0f
#define SERVO_TRACK_TAU_MS  60.0f               // first-order follow time constant
#define SNAP_DEG            0.05f               // below this error, snap to target
#define STAGGER_MS          300                 // pan first, tilt after this delay

#define TIMEBASE_HZ         1000000             // 1 tick = 1 us
#define PERIOD_US           20000
#define PULSE_ABS_MIN_US    500                 // DS3115 datasheet range
#define PULSE_ABS_MAX_US    2500

// ---- Calibration (measured Oct 16) ----------------------------------------
// pulse_us = zero_us + sign * deg * us_per_deg
typedef struct {
    float zero_us;
    float us_per_deg;
    float sign;
    float min_deg;
    float max_deg;
} axis_cal_t;

enum { YAW = 0, PITCH = 1 };

static const axis_cal_t CAL[2] = {
    // pan, 270 deg servo: pulse up = turn left = +yaw
    [YAW]   = { .zero_us = 1490.0f, .us_per_deg = 2000.0f / 270.0f, .sign = +1.0f,
                .min_deg = -35.0f, .max_deg = 35.0f },
    // tilt, 180 deg servo: pulse up = head up = -pitch
    // +18 deg down leaves ~2 deg margin to the mechanical stop (650 us)
    [PITCH] = { .zero_us = 875.0f,  .us_per_deg = 2000.0f / 180.0f, .sign = -1.0f,
                .min_deg = -90.0f, .max_deg = 18.0f },
};

// ---- State ----------------------------------------------------------------
typedef enum { SERVO_MODE_MOVE, SERVO_MODE_TRACK } servo_mode_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static float        s_target[2];      // clamped target, degrees
static float        s_pos[2];         // commanded position, degrees
static float        s_speed_dps = MAX_SPEED_DPS;
static servo_mode_t s_mode = SERVO_MODE_MOVE;

static mcpwm_cmpr_handle_t s_cmpr[2];
static bool s_inited = false;

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static uint32_t deg_to_pulse(int axis, float deg)
{
    const axis_cal_t *c = &CAL[axis];
    float us = c->zero_us + c->sign * deg * c->us_per_deg;
    us = clampf(us, PULSE_ABS_MIN_US, PULSE_ABS_MAX_US);
    return (uint32_t)lroundf(us);
}

static void set_target_locked(float yaw, float pitch)
{
    s_target[YAW]   = clampf(yaw,   CAL[YAW].min_deg,   CAL[YAW].max_deg);
    s_target[PITCH] = clampf(pitch, CAL[PITCH].min_deg, CAL[PITCH].max_deg);
}

// ---- Servo task: one step per PWM period ----------------------------------
static void servo_task(void *arg)
{
    const float alpha = fminf(DT_S / (SERVO_TRACK_TAU_MS / 1000.0f), 1.0f);  // <= 1: stable
    const float max_step = MAX_SPEED_DPS * DT_S;
    TickType_t last = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TICK_MS));

        float pos[2];
        portENTER_CRITICAL(&s_lock);
        float e[2] = { s_target[YAW] - s_pos[YAW], s_target[PITCH] - s_pos[PITCH] };
        float emax = fmaxf(fabsf(e[YAW]), fabsf(e[PITCH]));

        if (emax > 0.0f) {
            if (s_mode == SERVO_MODE_MOVE) {
                // Larger-error axis moves at speed, the other scales: straight path
                float step = s_speed_dps * DT_S;
                if (emax <= step) {
                    s_pos[YAW] = s_target[YAW];
                    s_pos[PITCH] = s_target[PITCH];
                } else {
                    float k = step / emax;
                    s_pos[YAW]   += e[YAW] * k;
                    s_pos[PITCH] += e[PITCH] * k;
                }
            } else {
                // First-order follow, scaled down together if over the speed cap
                float d[2] = { e[YAW] * alpha, e[PITCH] * alpha };
                float dmax = fmaxf(fabsf(d[YAW]), fabsf(d[PITCH]));
                if (dmax > max_step) {
                    float k = max_step / dmax;
                    d[YAW] *= k;
                    d[PITCH] *= k;
                }
                s_pos[YAW]   += d[YAW];
                s_pos[PITCH] += d[PITCH];
                if (fabsf(s_target[YAW] - s_pos[YAW]) < SNAP_DEG &&
                    fabsf(s_target[PITCH] - s_pos[PITCH]) < SNAP_DEG) {
                    s_pos[YAW] = s_target[YAW];
                    s_pos[PITCH] = s_target[PITCH];
                }
            }
        }
        pos[YAW] = s_pos[YAW];
        pos[PITCH] = s_pos[PITCH];
        portEXIT_CRITICAL(&s_lock);

        // Hardware writes outside the critical section; applied at next period start
        mcpwm_comparator_set_compare_value(s_cmpr[YAW],   deg_to_pulse(YAW,   pos[YAW]));
        mcpwm_comparator_set_compare_value(s_cmpr[PITCH], deg_to_pulse(PITCH, pos[PITCH]));
    }
}

// ---- Init -----------------------------------------------------------------
#define CHECK(x) do {                                               \
        esp_err_t err_ = (x);                                       \
        if (err_ != ESP_OK) {                                       \
            ESP_LOGE(TAG, "%s failed: %s", #x, esp_err_to_name(err_)); \
            return err_;                                            \
        }                                                           \
    } while (0)

static esp_err_t channel_create(mcpwm_oper_handle_t oper, int gpio, uint32_t pulse_us,
                                mcpwm_cmpr_handle_t *out_cmpr, mcpwm_gen_handle_t *out_gen)
{
    mcpwm_comparator_config_t cmpr_cfg = {
        .flags.update_cmp_on_tez = true,   // new value takes effect at period start
    };
    CHECK(mcpwm_new_comparator(oper, &cmpr_cfg, out_cmpr));
    CHECK(mcpwm_comparator_set_compare_value(*out_cmpr, pulse_us));

    mcpwm_generator_config_t gen_cfg = {
        .gen_gpio_num = gpio,
    };
    CHECK(mcpwm_new_generator(oper, &gen_cfg, out_gen));
    CHECK(mcpwm_generator_set_action_on_timer_event(*out_gen,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
    CHECK(mcpwm_generator_set_action_on_compare_event(*out_gen,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, *out_cmpr, MCPWM_GEN_ACTION_LOW)));
    return ESP_OK;
}

esp_err_t servo_init(int pan_gpio, int tilt_gpio)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_target[YAW] = s_target[PITCH] = 0.0f;
    s_pos[YAW] = s_pos[PITCH] = 0.0f;

    mcpwm_timer_handle_t timer = NULL;
    mcpwm_timer_config_t timer_cfg = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = TIMEBASE_HZ,
        .period_ticks = PERIOD_US,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
    };
    CHECK(mcpwm_new_timer(&timer_cfg, &timer));

    mcpwm_oper_handle_t oper = NULL;
    mcpwm_operator_config_t oper_cfg = {
        .group_id = 0,
    };
    CHECK(mcpwm_new_operator(&oper_cfg, &oper));
    CHECK(mcpwm_operator_connect_timer(oper, timer));

    mcpwm_gen_handle_t gen_pan = NULL, gen_tilt = NULL;
    CHECK(channel_create(oper, pan_gpio,  deg_to_pulse(YAW,   0.0f), &s_cmpr[YAW],   &gen_pan));
    CHECK(channel_create(oper, tilt_gpio, deg_to_pulse(PITCH, 0.0f), &s_cmpr[PITCH], &gen_tilt));

    // Hold tilt low (no pulses, servo idle) while pan starts
    CHECK(mcpwm_generator_set_force_level(gen_tilt, 0, true));

    CHECK(mcpwm_timer_enable(timer));
    CHECK(mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP));
    ESP_LOGI(TAG, "pan PWM on");

    vTaskDelay(pdMS_TO_TICKS(STAGGER_MS));
    CHECK(mcpwm_generator_set_force_level(gen_tilt, -1, true));   // release: normal PWM
    ESP_LOGI(TAG, "tilt PWM on");

    if (xTaskCreate(servo_task, "servo", 3072, NULL, SERVO_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_FAIL;
    }

    s_inited = true;
    ESP_LOGI(TAG, "init ok, limits yaw [%.0f, %.0f], pitch [%.0f, %.0f]",
             CAL[YAW].min_deg, CAL[YAW].max_deg, CAL[PITCH].min_deg, CAL[PITCH].max_deg);
    return ESP_OK;
}

// ---- Public API -----------------------------------------------------------
void servo_move_to(float yaw_deg, float pitch_deg, float speed_dps)
{
    float speed = clampf(speed_dps, 1.0f, MAX_SPEED_DPS);
    portENTER_CRITICAL(&s_lock);
    set_target_locked(yaw_deg, pitch_deg);
    s_speed_dps = speed;
    s_mode = SERVO_MODE_MOVE;
    portEXIT_CRITICAL(&s_lock);
}

void servo_track(float yaw_deg, float pitch_deg)
{
    portENTER_CRITICAL(&s_lock);
    set_target_locked(yaw_deg, pitch_deg);
    s_mode = SERVO_MODE_TRACK;
    portEXIT_CRITICAL(&s_lock);
}

void servo_rest(float speed_dps)
{
    servo_move_to(0.0f, 0.0f, speed_dps);
}

void servo_get_position(float *yaw_deg, float *pitch_deg)
{
    portENTER_CRITICAL(&s_lock);
    float y = s_pos[YAW], p = s_pos[PITCH];
    portEXIT_CRITICAL(&s_lock);
    if (yaw_deg)   *yaw_deg = y;
    if (pitch_deg) *pitch_deg = p;
}

bool servo_is_moving(void)
{
    portENTER_CRITICAL(&s_lock);
    bool moving = s_pos[YAW] != s_target[YAW] || s_pos[PITCH] != s_target[PITCH];
    portEXIT_CRITICAL(&s_lock);
    return moving;
}

void servo_get_limits(float *yaw_min, float *yaw_max, float *pitch_min, float *pitch_max)
{
    if (yaw_min)   *yaw_min   = CAL[YAW].min_deg;
    if (yaw_max)   *yaw_max   = CAL[YAW].max_deg;
    if (pitch_min) *pitch_min = CAL[PITCH].min_deg;
    if (pitch_max) *pitch_max = CAL[PITCH].max_deg;
}