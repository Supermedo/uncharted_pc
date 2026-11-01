/* Built-in gyro (src/runtime_gyro.c): the camera turn from gyro samples, and calibration. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "runtime_gyro.h"

static const GyroSettings plain={1.0f,0,0,0.0f,0.0f};

static int near(float a, float b, float tolerance) { return fabsf(a-b)<=tolerance; }

/* The turn of `seconds` of a constant rate, in samples of 1 ms, adding to *right and *up; f carries the
 * turn held back by the deadzone and precision speed from one call to the next. */
static void turn_with(const GyroSettings *s, const GyroCalibration *c, GyroFilter *f, float pitch, float yaw,
                      float seconds, float *right, float *up) {
    const float rate[3]={pitch,yaw,0.0f};
    for (int i=0;i<(int)(seconds*1000.0f+0.5f);++i) {
        float r, u;
        gyro_turn(s,c,f,rate,0.001f,&r,&u);
        *right+=r; *up+=u;
    }
}
static void turn_for(const GyroSettings *s, const GyroCalibration *c, float pitch, float yaw, float seconds,
                     float *right, float *up) {
    GyroFilter f; gyro_filter_reset(&f);
    *right=*up=0.0f;
    turn_with(s,c,&f,pitch,yaw,seconds,right,up);
}

#define DEG (1.0f/57.29578f) /* rad/s per deg/s */
/* Ben's Steam values: Gyro Speed Deadzone 0.36 deg/s, Gyro Precision Speed 0.75 deg/s. */
static const GyroSettings steady={1.0f,0,0,0.36f,0.75f};

static void deadzone_stops_slow(void) {
    GyroCalibration c; gyro_calibration_reset(&c);
    float right, up;
    turn_for(&steady,&c,0.2f*DEG,-0.2f*DEG,1.0f,&right,&up);
    assert(near(right,0.0f,1e-5f) && near(up,0.0f,1e-5f));
}

static void precision_scales(void) {
    GyroCalibration c; gyro_calibration_reset(&c);
    const GyroSettings precise={1.0f,0,0,0.0f,0.75f};
    float right, up;
    turn_for(&precise,&c,0.0f,-0.5f*DEG,1.0f,&right,&up); /* 0.5 deg/s right: two thirds of the turn */
    assert(near(right,0.5f*0.5f/0.75f,0.002f) && near(up,0.0f,1e-6f));
}

static void lost_turn_returns(void) {
    GyroCalibration c; gyro_calibration_reset(&c);
    GyroFilter f; gyro_filter_reset(&f);
    float right=0.0f, up=0.0f;
    turn_with(&steady,&c,&f,0.0f,-0.5f*DEG,1.0f,&right,&up);  /* slow: most of 0.5 degrees held back */
    assert(right<0.4f);
    turn_with(&steady,&c,&f,0.0f,-30.0f*DEG,0.5f,&right,&up); /* fast: 15 degrees and the rest back */
    assert(near(right,15.5f,0.02f));
}


static void turn_left_is_negative_right(void) {
    GyroCalibration c; gyro_calibration_reset(&c);
    float right, up;
    turn_for(&plain,&c,0.0f,1.0f,0.5f,&right,&up);
    assert(near(right,-28.648f,0.01f));
    assert(near(up,0.0f,1e-6f));
}

static void pitch_up_is_up(void) {
    GyroCalibration c; gyro_calibration_reset(&c);
    float right, up;
    turn_for(&plain,&c,1.0f,0.0f,0.5f,&right,&up);
    assert(near(up,28.648f,0.01f));
    assert(near(right,0.0f,1e-6f));
}

static void sensitivity_scales(void) {
    GyroCalibration c; gyro_calibration_reset(&c);
    const GyroSettings twice={2.0f,0,0,0.0f,0.0f};
    float right, up;
    turn_for(&twice,&c,1.0f,1.0f,0.5f,&right,&up);
    assert(near(right,-57.296f,0.02f));
    assert(near(up,57.296f,0.02f));
}

static void invert_flips(void) {
    GyroCalibration c; gyro_calibration_reset(&c);
    const GyroSettings x={1.0f,1,0,0.0f,0.0f}, y={1.0f,0,1,0.0f,0.0f};
    float right, up;
    turn_for(&x,&c,1.0f,1.0f,0.5f,&right,&up);
    assert(near(right,28.648f,0.01f) && near(up,28.648f,0.01f));
    turn_for(&y,&c,1.0f,1.0f,0.5f,&right,&up);
    assert(near(right,-28.648f,0.01f) && near(up,-28.648f,0.01f));
}

static void still_learns_bias(void) {
    GyroCalibration c; gyro_calibration_reset(&c);
    const float drift[3]={0.01f,-0.01f,0.005f};
    int learned=0;
    for (int i=0;i<2000;++i) learned|=gyro_calibration_add(&c,drift,0.001f);
    assert(learned && c.calibrated);
    float right, up;
    turn_for(&plain,&c,drift[0],drift[1],1.0f,&right,&up);
    assert(near(right,0.0f,0.01f) && near(up,0.0f,0.01f));
}

static void moving_learns_nothing(void) {
    GyroCalibration c; gyro_calibration_reset(&c);
    for (int i=0;i<3000;++i) {
        const float rate[3]={0.0f,sinf((float)i*0.006f),0.0f}; /* 1 rad/s sine, about 1 Hz */
        assert(!gyro_calibration_add(&c,rate,0.001f));
    }
    assert(!c.calibrated);
}

static void slow_turn_not_learned(void) {
    GyroCalibration c; gyro_calibration_reset(&c);
    const float pan[3]={0.0f,0.05f,0.0f}; /* a steady 2.9 deg/s turn is aiming, not drift */
    for (int i=0;i<3000;++i) assert(!gyro_calibration_add(&c,pan,0.001f));
    c.manual=1; /* the menu's Calibrate: the controller is said to lie still */
    int learned=0;
    for (int i=0;i<2000;++i) learned|=gyro_calibration_add(&c,pan,0.001f);
    assert(learned && near(c.bias[1],0.05f,1e-4f) && !c.manual);
}

static void bias_removed_before_sensitivity(void) {
    GyroCalibration c; gyro_calibration_reset(&c);
    const float drift[3]={0.01f,0.01f,0.0f};
    for (int i=0;i<2000;++i) gyro_calibration_add(&c,drift,0.001f);
    const GyroSettings thrice={3.0f,0,0,0.0f,0.0f};
    float right, up;
    turn_for(&thrice,&c,drift[0],drift[1],1.0f,&right,&up);
    assert(near(right,0.0f,0.01f) && near(up,0.0f,0.01f));
}

/* Playback: a turn of `rate` deg/s right (until `stop_s`, then still), one sample every 2 ms of sensor time,
 * arriving in clumps as over Bluetooth (arrivals every 4 to 25 ms, none for `stall_ms` from 0.6 s), the
 * game reading every 16.667 ms. Each read's turn goes to out[] (max reads); returns the turn added. */
static double play(GyroPlayback *p, float rate, double seconds, double stop_s, double stall_ms, float *out,
                   int reads) {
    static const double gaps[]={4.0,20.0,9.0,15.0,2.0,25.0};
    const uint64_t origin=123456789000ull; /* the sensor's clock starts anywhere */
    double arrival=0.0, added=0.0;
    int next=0, gap=0;
    gyro_playback_reset(p);
    for (int r=0;r<reads;++r) {
        const double wall=r*16.667;
        while (arrival<=wall) {
            while (next*2.0<=arrival && next*2.0<seconds*1000.0) { /* the samples taken by then */
                const float turn=next*2.0<stop_s*1000.0 ? rate*0.002f : 0.0f;
                gyro_playback_add(p,origin+(uint64_t)next*2000000u,turn,0.0f);
                added+=turn; ++next;
            }
            arrival+=gaps[gap++%6];
            if (arrival>=600.0 && arrival<600.0+stall_ms) arrival=600.0+stall_ms;
        }
        float right, up;
        gyro_playback_take(p,0.016667f,&right,&up);
        out[r]=right;
    }
    return added;
}

static void playback_even_from_clumps(void) {
    static GyroPlayback p;
    float out[120];
    play(&p,60.0f,2.0,2.0,0.0,out,120);
    for (int r=30;r<110;++r) assert(near(out[r],1.0f,0.06f)); /* 60 deg/s: 1 degree per read, +-6% */
}

static void playback_keeps_every_degree(void) {
    static GyroPlayback p;
    float out[120];
    const double added=play(&p,45.0f,2.0,1.0,0.0,out,120);
    double shown=0.0;
    for (int r=0;r<120;++r) shown+=out[r];
    assert(fabs(shown-added)<1e-3 && added>40.0);
}

static void playback_bridges_a_stall(void) {
    static GyroPlayback p;
    float out[120];
    const double added=play(&p,60.0f,2.0,1.5,30.0,out,120); /* no arrivals for 30 ms at 0.6 s */
    for (int r=30;r<80;++r) assert(out[r]>0.75f && out[r]<1.25f);
    double shown=0.0;
    for (int r=0;r<120;++r) shown+=out[r];
    assert(fabs(shown-added)<1e-3);
}

/* The controller's clock runs 1% fast against the game's: the playhead keeps following the samples. */
static void playback_follows_the_controllers_clock(void) {
    static GyroPlayback p;
    gyro_playback_reset(&p);
    int next=0;
    for (int r=0;r<240;++r) {
        const double wall=r*16.667;
        for (;next*2.0<=wall*1.01;++next) gyro_playback_add(&p,(uint64_t)next*2000000u,0.12f,0.0f);
        float right, up;
        gyro_playback_take(&p,0.016667f,&right,&up);
        const double behind=((double)(next-1)*2e6-p.playhead)*1e-6; /* ms behind the newest sample */
        if (r>=60) assert(behind>=0.0 && behind<=30.0);
    }
}

int main(void) {
    turn_left_is_negative_right();
    pitch_up_is_up();
    sensitivity_scales();
    invert_flips();
    still_learns_bias();
    moving_learns_nothing();
    slow_turn_not_learned();
    bias_removed_before_sensitivity();
    deadzone_stops_slow();
    precision_scales();
    lost_turn_returns();
    playback_even_from_clumps();
    playback_keeps_every_degree();
    playback_bridges_a_stall();
    playback_follows_the_controllers_clock();
    puts("gyro tests passed");
    return 0;
}
