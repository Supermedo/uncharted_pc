/* Built-in gyro: the camera turn from the controller's
 * gyro samples, and calibration. Pure C, no SDL: runtime_pad.c feeds it, tests/test_gyro.c tests it. */
#ifndef RUNTIME_GYRO_H
#define RUNTIME_GYRO_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* One gyro sample as SDL gives it: rad/s, [0] pitch (top tilts up: positive), [1] yaw (turn left:
 * positive), [2] roll; the sensor's time in ns. */
typedef struct { float rate[3]; uint64_t ns; } GyroSample;

/* Steam's names: Gyro ° Sensitivity (1 = the camera turns 1 degree per degree of the controller),
 * Invert X Output, Invert Y Output, Gyro Speed Deadzone and Gyro Precision Speed (deg/s: below the
 * deadzone no turn, below the precision speed a turn scaled down by speed / precision speed; the turn held
 * back comes back once the controller moves fast). */
typedef struct { float sensitivity; int invert_x, invert_y; float speed_deadzone, precision_speed; } GyroSettings;

/* The turn the deadzone and precision speed held back (degrees of controller, right and up), owed until
 * the controller moves fast in the same direction. */
typedef struct { float owed[2]; } GyroFilter;

/* The gyro's offset at rest (rad/s), learned while the controller lies still. */
typedef struct {
    float bias[3];
    int calibrated;
    float still_seconds;  /* how long the samples have stayed near still_mean */
    float still_sum[3];
    int still_count;
    int manual;           /* the menu's Calibrate: learn up to 10 deg/s (else 1.5), then clear */
} GyroCalibration;

void gyro_calibration_reset(GyroCalibration *c);
/* Learns the bias while the controller is still: once every sample has stayed within 1 deg/s of the
 * mean for 1.5 s and the mean is under 1.5 deg/s (a slower steady turn is aiming), the bias becomes
 * that mean. Returns 1 when it (re)learned the bias. */
int gyro_calibration_add(GyroCalibration *c, const float rate[3], float dt);
void gyro_filter_reset(GyroFilter *f);
/* The camera turn for one sample of dt seconds, degrees: right and up positive. */
void gyro_turn(const GyroSettings *s, const GyroCalibration *c, GyroFilter *f, const float rate[3], float dt,
               float *right_deg, float *up_deg);
/* Playback: the turn shown follows the clock, not the arrival of the controller's reports, which come
 * in clumps (Bluetooth): each read shows the turn of the sensor time that passed since the last read, a
 * little behind the newest sample (GYRO_PLAYBACK_DELAY_MS), carried on at the last speed for a while when
 * reports are late; what arrives later corrects it, and every degree added is shown once. */
#define GYRO_PLAYBACK_POINTS 512
typedef struct {
    uint64_t ns[GYRO_PLAYBACK_POINTS];        /* sensor time of each sample, oldest first (a ring) */
    double turn[GYRO_PLAYBACK_POINTS][2];     /* the turn so far at that time, degrees right and up */
    int first, count;
    double total[2];                          /* the turn so far at the newest sample */
    double playhead;                          /* the sensor time shown, ns */
    double shown[2];                          /* the turn shown so far */
    int playing;
} GyroPlayback;
void gyro_playback_reset(GyroPlayback *p);
/* A sample's turn (degrees), ending at sensor time ns. */
void gyro_playback_add(GyroPlayback *p, uint64_t ns, float right_deg, float up_deg);
/* The turn to show now, dt seconds of the game's clock after the last call. */
void gyro_playback_take(GyroPlayback *p, float dt, float *right_deg, float *up_deg);

#ifdef __cplusplus
}
#endif
#endif
