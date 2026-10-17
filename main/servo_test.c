#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "servo.h"

#define TEST_SECONDS      10
#define SAMPLE_MS         10        // log every 10 ms: sees every 50 Hz servo step
#define UPDATE_PERIOD_US  33333     // targets at 30 Hz

typedef enum { G_MOVE80, G_MOVE300, G_TRACK } group_t;

static void back_to_rest(void)
{
    servo_rest(60.0f);
    while (servo_is_moving()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}

static void run_group(const char *name, group_t g)
{
    back_to_rest();
    printf("# group %s\n", name);
    printf("CSV,t_ms,tgt_yaw,tgt_pitch,cmd_yaw,cmd_pitch\n");

    const int64_t t0 = esp_timer_get_time();
    int64_t next_update = t0;
    float tgt_yaw = 0.0f, tgt_pitch = 0.0f;
    TickType_t last = xTaskGetTickCount();

    while (1) {
        int64_t now = esp_timer_get_time();
        int64_t t_us = now - t0;
        if (t_us >= (int64_t)TEST_SECONDS * 1000000) {
            break;
        }
        if (now >= next_update) {
            float ts = t_us / 1e6f;
            tgt_yaw = 30.0f * sinf(2.0f * (float)M_PI * 0.3f * ts);   // starts at 0: no jump
            switch (g) {
            case G_MOVE80:  servo_move_to(tgt_yaw, tgt_pitch, 80.0f);  break;
            case G_MOVE300: servo_move_to(tgt_yaw, tgt_pitch, 300.0f); break;
            case G_TRACK:   servo_track(tgt_yaw, tgt_pitch);           break;
            }
            next_update += UPDATE_PERIOD_US;
        }
        float cy, cp;
        servo_get_position(&cy, &cp);
        printf("CSV,%lld,%.2f,%.2f,%.2f,%.2f\n",
               (long long)(t_us / 1000), tgt_yaw, tgt_pitch, cy, cp);
        vTaskDelayUntil(&last, pdMS_TO_TICKS(SAMPLE_MS));
    }
}

void servo_stream_test(void)
{
    run_group("move80",  G_MOVE80);
    run_group("move300", G_MOVE300);
    run_group("track60", G_TRACK);
    back_to_rest();
    printf("# servo stream test done\n");
}