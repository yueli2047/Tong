#pragma once
#include <stdbool.h>
#include "esp_err.h"

// Head pose in degrees, ROS REP-103 (x forward, y left, z up):
//   +yaw   = head turns to its own left (counter-clockwise seen from above)
//   +pitch = head looks down
// (0, 0) = head level, facing forward. Calibration offsets are handled inside.
//
// Single caller: at runtime only face's motion layer calls this API.
// Other modules that want to move the head go through face.
// Internally, the last call (move_to / track / rest) wins.

// Starts PWM at the rest pose (0, 0). Pan starts first, tilt ~300 ms later
// to avoid both servos drawing inrush current at once. Blocks ~300 ms.
esp_err_t servo_init(int pan_gpio, int tilt_gpio);

// Constant-speed point-to-point move (nods, return to center).
// Both axes arrive together on a straight path. Non-blocking.
void servo_move_to(float yaw_deg, float pitch_deg, float speed_dps);

// Streaming follow: call every frame (30-60 Hz). First-order tracking with
// time constant SERVO_TRACK_TAU_MS, capped at the max speed. Non-blocking.
void servo_track(float yaw_deg, float pitch_deg);

// Equivalent to servo_move_to(0, 0, speed_dps).
void servo_rest(float speed_dps);

// Current commanded angles (stepping toward the target), not the target itself.
void servo_get_position(float *yaw_deg, float *pitch_deg);

bool servo_is_moving(void);

void servo_get_limits(float *yaw_min, float *yaw_max, float *pitch_min, float *pitch_max);