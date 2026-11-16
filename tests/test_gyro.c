/* Built-in gyro (src/runtime_gyro.c): the camera turn from gyro samples, and calibration. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "runtime_gyro.h"

static const GyroSettings plain={1.0f,0,0,0.0f,0.0f,GYRO_SPACE_YAW,0.0f,0.0f,0.0f};

static int near(float a, float b, float tolerance) { return fabsf(a-b)<=tolerance; }

/* The turn of `seconds` of a constant rate, in samples of 1 ms, adding to *right and *up; f carries the
 * turn held back by the deadzone and precision speed from one call to the next. */
static void turn_with(const GyroSettings *s, const GyroCalibration *c, GyroFilter *f, float pitch, float yaw,
                      float seconds, float *right, float *up) {
    const float rate[3]={pitch,yaw,0.0f};
    for (int i=0;i<(int)(seconds*1000.0f+0.5f);++i) {
        float r, u;
        gyro_turn(s,c,f,rate,0.001f,NULL,&r,&u);
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
static const GyroSettings steady={1.0f,0,0,0.36f,0.75f,GYRO_SPACE_YAW,0.0f,0.0f,0.0f};

static void deadzone_stops_slow(void) {
    GyroCalibration c; gyro_calibration_reset(&c);
    float right, up;
    turn_for(&steady,&c,0.2f*DEG,-0.2f*DEG,1.0f,&right,&up);
    assert(near(right,0.0f,1e-5f) && near(up,0.0f,1e-5f));
}

static void precision_scales(void) {
    GyroCalibration c; gyro_calibration_reset(&c);
    const GyroSettings precise={1.0f,0,0,0.0f,0.75f,GYRO_SPACE_YAW,0.0f,0.0f,0.0f};
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
    const GyroSettings twice={2.0f,0,0,0.0f,0.0f,GYRO_SPACE_YAW,0.0f,0.0f,0.0f};
    float right, up;
    turn_for(&twice,&c,1.0f,1.0f,0.5f,&right,&up);
    assert(near(right,-57.296f,0.02f));
    assert(near(up,57.296f,0.02f));
}

static void invert_flips(void) {
    GyroCalibration c; gyro_calibration_reset(&c);
    const GyroSettings x={1.0f,1,0,0.0f,0.0f,GYRO_SPACE_YAW,0.0f,0.0f,0.0f}, y={1.0f,0,1,0.0f,0.0f,GYRO_SPACE_YAW,0.0f,0.0f,0.0f};
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
    const GyroSettings thrice={3.0f,0,0,0.0f,0.0f,GYRO_SPACE_YAW,0.0f,0.0f,0.0f};
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

/* Gyro space (Steam's 3DOF to 2D Conversion Style). The controller turned left at 1 rad/s around the real
 * vertical while its top is tipped up by `tip` degrees (90: held upright, sticks facing the player). */
static void turn_left_tipped(float tip, float rate[3], float grav[3]) {
    const float t=tip*DEG, c=cosf(t), s=sinf(t);
    rate[0]=0.0f; rate[1]=c; rate[2]=-s; /* the real vertical in the controller's axes */
    grav[0]=0.0f; grav[1]=-c; grav[2]=s;
}
static void space_of(int space, const float rate[3], const float grav[3], float *pitch, float *yaw) {
    GyroSettings s=plain; s.space=space;
    gyro_space_rates(&s,rate,grav,pitch,yaw);
}
static void local_of(float angle, float complement, const float rate[3], float *pitch, float *yaw) {
    GyroSettings s=plain; s.space=GYRO_SPACE_LOCAL; s.local_angle=angle; s.local_complement=complement;
    gyro_space_rates(&s,rate,NULL,pitch,yaw);
}

static void yaw_is_the_controllers_yaw(void) {
    const float rate[3]={0.2f,0.5f,0.7f}, upright[3]={0.0f,0.0f,1.0f};
    float pitch, yaw;
    space_of(GYRO_SPACE_YAW,rate,upright,&pitch,&yaw);
    assert(near(pitch,0.2f,1e-6f) && near(yaw,0.5f,1e-6f));
    space_of(GYRO_SPACE_YAW,rate,NULL,&pitch,&yaw);
    assert(near(pitch,0.2f,1e-6f) && near(yaw,0.5f,1e-6f));
    float rate_up[3], grav[3];
    turn_left_tipped(90.0f,rate_up,grav); /* held upright, Yaw loses the turn */
    space_of(GYRO_SPACE_YAW,rate_up,grav,&pitch,&yaw);
    assert(near(yaw,0.0f,1e-5f));
}

static void roll_is_leaning(void) {
    const float lean_right[3]={0.1f,0.0f,-1.0f}; /* right side down, like a steering wheel */
    float pitch, yaw;
    space_of(GYRO_SPACE_ROLL,lean_right,NULL,&pitch,&yaw);
    assert(near(yaw,-1.0f,1e-6f) && near(pitch,0.1f,1e-6f)); /* yaw left positive: right */
}

static void yaw_plus_roll_adds_turn_and_lean(void) {
    const float both_left[3]={0.0f,0.5f,0.25f};
    GyroSettings s=plain; s.space=GYRO_SPACE_YAW_ROLL;
    float pitch, yaw;
    s.local_complement=1.0f;  /* Roll Axis Contribution 100%, Steam's default */
    gyro_space_rates(&s,both_left,NULL,&pitch,&yaw);
    assert(near(yaw,0.75f,1e-6f));
    s.local_complement=-0.5f; /* -50%: the lean counts half, the other way */
    gyro_space_rates(&s,both_left,NULL,&pitch,&yaw);
    assert(near(yaw,0.375f,1e-6f));
}

static void local_space_turns_around_its_tipped_axis(void) {
    float rate[3], grav[3], pitch, yaw;
    const float any[3]={0.3f,0.5f,0.7f};
    local_of(0.0f,0.0f,any,&pitch,&yaw);         /* untipped, no complement: Yaw */
    assert(near(yaw,0.5f,1e-6f) && near(pitch,0.3f,1e-6f));
    local_of(0.0f,1.0f,any,&pitch,&yaw);         /* untipped, full complement: Yaw + Roll */
    assert(near(yaw,1.2f,1e-6f));
    local_of(90.0f,0.0f,any,&pitch,&yaw);        /* tilted 90 degrees, no complement: Roll */
    assert(near(yaw,0.7f,1e-6f));
    turn_left_tipped(40.0f,rate,grav);
    local_of(-40.0f,0.0f,rate,&pitch,&yaw);      /* the axis tilted below the horizon, as the controller's top */
    assert(near(yaw,1.0f,1e-5f));                /* is tipped up: the whole turn */
    local_of(0.0f,0.0f,rate,&pitch,&yaw);        /* not tilted: part of the turn is lost */
    assert(near(yaw,cosf(40.0f*DEG),1e-5f));
    const float around_complement[3]={0.0f,sinf(40.0f*DEG),cosf(40.0f*DEG)};
    local_of(-40.0f,0.5f,around_complement,&pitch,&yaw); /* the axis at 90 degrees counts by half */
    assert(near(yaw,0.5f,1e-5f));
    local_of(-40.0f,-1.0f,around_complement,&pitch,&yaw); /* or against */
    assert(near(yaw,-1.0f,1e-5f));
}

static void player_and_world_turn_the_same_however_tipped(void) {
    const float tips[]={0.0f,45.0f,90.0f,-45.0f,70.0f};
    for (int i=0;i<5;++i) {
        float rate[3], grav[3], pitch, yaw;
        turn_left_tipped(tips[i],rate,grav);
        space_of(GYRO_SPACE_PLAYER,rate,grav,&pitch,&yaw);
        assert(near(yaw,1.0f,1e-4f) && near(pitch,0.0f,1e-4f));
        space_of(GYRO_SPACE_WORLD,rate,grav,&pitch,&yaw);
        assert(near(yaw,1.0f,1e-4f) && near(pitch,0.0f,1e-4f));
    }
}

static void player_space_caps_an_out_of_line_turn(void) {
    const float flat[3]={0.0f,-1.0f,0.0f};
    const float roll[3]={0.0f,0.0f,1.0f}, leaning[3]={0.0f,0.5f,1.0f}, near_line[3]={0.0f,1.0f,0.3f};
    float pitch, yaw;
    space_of(GYRO_SPACE_PLAYER,roll,flat,&pitch,&yaw);      /* rolling a flat controller: no turn */
    assert(near(yaw,0.0f,1e-6f));
    space_of(GYRO_SPACE_PLAYER,leaning,flat,&pitch,&yaw);   /* well out of line: 1.41 x the real turn */
    assert(near(yaw,0.705f,1e-4f));
    space_of(GYRO_SPACE_PLAYER,near_line,flat,&pitch,&yaw); /* close to the line: the whole of it */
    assert(near(yaw,sqrtf(1.09f),1e-4f));
    const float pitching[3]={0.4f,0.0f,0.0f}, upright[3]={0.0f,0.0f,1.0f};
    space_of(GYRO_SPACE_PLAYER,pitching,upright,&pitch,&yaw); /* up-down is the controller's own pitch */
    assert(near(pitch,0.4f,1e-6f) && near(yaw,0.0f,1e-6f));
}

static void world_space_pitch_fades_on_the_side(void) {
    const float rate[3]={1.0f,0.0f,0.0f};
    const float flat[3]={0.0f,-1.0f,0.0f}, side[3]={1.0f,0.0f,0.0f};
    const float s=0.17f, almost[3]={sqrtf(1.0f-s*s),-s,0.0f}; /* tipped onto its side but for 10 degrees */
    float pitch, yaw;
    space_of(GYRO_SPACE_WORLD,rate,flat,&pitch,&yaw);
    assert(near(pitch,1.0f,1e-5f));
    space_of(GYRO_SPACE_WORLD,rate,side,&pitch,&yaw);
    assert(near(pitch,0.0f,1e-6f));
    space_of(GYRO_SPACE_WORLD,rate,almost,&pitch,&yaw); /* the flattened axis catches 0.17, faded to 0.06 */
    assert(pitch>0.03f && pitch<0.1f);
}

static void rotate_output_turns_clockwise(void) {
    GyroCalibration c; gyro_calibration_reset(&c);
    GyroSettings turned=plain;
    float right, up;
    turned.rotate=90.0f;  /* a turn to the right shows as a turn down */
    turn_for(&turned,&c,0.0f,-10.0f*DEG,1.0f,&right,&up);
    assert(near(right,0.0f,1e-3f) && near(up,-10.0f,1e-3f));
    turned.rotate=-30.0f; /* up, turned 30 degrees anticlockwise: up and a little left */
    turn_for(&turned,&c,10.0f*DEG,0.0f,1.0f,&right,&up);
    assert(near(right,-5.0f,1e-3f) && near(up,8.660254f,1e-3f));
}

static void player_space_through_the_turn(void) {
    GyroCalibration c; gyro_calibration_reset(&c);
    GyroFilter f; gyro_filter_reset(&f);
    GyroSettings player=plain; player.space=GYRO_SPACE_PLAYER;
    float rate[3], grav[3], r, u;
    turn_left_tipped(90.0f,rate,grav);
    gyro_turn(&player,&c,&f,rate,0.01f,grav,&r,&u);
    assert(near(r,-0.5729578f,1e-4f) && near(u,0.0f,1e-5f)); /* 1 rad/s left for 10 ms */
}

/* Gravity: m/s^2 as SDL gives it (at rest it reads 1 g upwards). */
#define G 9.80665f
static void gravity_feed(GyroGravity *g, const GyroCalibration *c, const float rate[3], const float accel[3],
                         float seconds) {
    for (int i=0;i<(int)(seconds*1000.0f+0.5f);++i) gyro_gravity_add(g,c,rate,accel,0.001f);
}
static float gap(const float a[3], const float b[3]) {
    return sqrtf((a[0]-b[0])*(a[0]-b[0])+(a[1]-b[1])*(a[1]-b[1])+(a[2]-b[2])*(a[2]-b[2]));
}

static void gravity_starts_flat_then_takes_the_first_reading(void) {
    GyroGravity g; gyro_gravity_reset(&g);
    GyroCalibration c; gyro_calibration_reset(&c);
    const float flat[3]={0.0f,-1.0f,0.0f}, upright[3]={0.0f,0.0f,1.0f};
    assert(gap(g.grav,flat)<1e-6f);
    const float still[3]={0}, reads_upright[3]={0.0f,0.0f,-G};
    gyro_gravity_add(&g,&c,still,reads_upright,0.001f);
    assert(gap(g.grav,upright)<1e-5f);
}

static void gravity_settles_on_the_accelerometer(void) {
    GyroGravity g; gyro_gravity_reset(&g);
    GyroCalibration c; gyro_calibration_reset(&c);
    const float still[3]={0}, reads_upright[3]={0.0f,0.0f,-G}, reads_flat[3]={0.0f,G,0.0f};
    const float flat[3]={0.0f,-1.0f,0.0f};
    gravity_feed(&g,&c,still,reads_upright,0.1f);
    gravity_feed(&g,&c,still,reads_flat,0.2f);
    assert(gap(g.grav,flat)>0.5f);   /* not at once */
    gravity_feed(&g,&c,still,reads_flat,4.0f);
    assert(gap(g.grav,flat)<0.06f);  /* the last 0.05 g at 0.01 g/s while still (the reference's pace) */
    gravity_feed(&g,&c,still,reads_flat,4.0f);
    assert(gap(g.grav,flat)<0.02f);
}

static void gravity_follows_the_gyro(void) {
    GyroGravity g; gyro_gravity_reset(&g);
    GyroCalibration c; gyro_calibration_reset(&c);
    const float tip_up[3]={90.0f*DEG,0.0f,0.0f}, none[3]={0}, upright[3]={0.0f,0.0f,1.0f};
    gravity_feed(&g,&c,tip_up,none,1.0f); /* no readings: the gyro alone tips it upright */
    assert(gap(g.grav,upright)<0.01f);
}

static void gravity_takes_off_the_gyros_offset(void) {
    GyroGravity g; gyro_gravity_reset(&g);
    GyroCalibration c; gyro_calibration_reset(&c);
    c.bias[0]=5.0f*DEG; c.calibrated=1;
    const float resting[3]={5.0f*DEG,0.0f,0.0f}, none[3]={0}, flat[3]={0.0f,-1.0f,0.0f};
    gravity_feed(&g,&c,resting,none,1.0f);
    assert(gap(g.grav,flat)<1e-4f);
}

static void gravity_trusts_a_shaken_reading_less(void) {
    GyroCalibration c; gyro_calibration_reset(&c);
    const float still[3]={0}, reads_flat[3]={0.0f,G,0.0f};
    float moved[2];
    for (int shaken=0;shaken<2;++shaken) {
        GyroGravity g; gyro_gravity_reset(&g);
        gravity_feed(&g,&c,still,reads_flat,0.5f);
        const float start[3]={g.grav[0],g.grav[1],g.grav[2]};
        for (int i=0;i<2000;++i) { /* 2 s of readings tipped 30 degrees, shaken hard (+-0.5 g) or not */
            const float shake=shaken ? (i%2 ? 0.5f*G : -0.5f*G) : 0.0f;
            const float accel[3]={shake,G*0.866f,-G*0.5f};
            gyro_gravity_add(&g,&c,still,accel,0.001f);
        }
        moved[shaken]=gap(g.grav,start);
    }
    assert(moved[0]>0.45f && moved[1]<0.25f); /* the tip is 0.52 g away; shaken, 0.1 g/s at most */
}

static void gravity_ignores_a_small_wobble_while_still(void) {
    GyroGravity g; gyro_gravity_reset(&g);
    GyroCalibration c; gyro_calibration_reset(&c);
    const float still[3]={0}, reads_flat[3]={0.0f,G,0.0f};
    gravity_feed(&g,&c,still,reads_flat,0.5f);
    const float start[3]={g.grav[0],g.grav[1],g.grav[2]};
    const float reads_2_degrees[3]={G*0.0349f,G*0.9994f,0.0f};
    gravity_feed(&g,&c,still,reads_2_degrees,1.0f);
    assert(gap(g.grav,start)<0.015f); /* 0.01 g/s while it is this close and not turning */
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
    yaw_is_the_controllers_yaw();
    roll_is_leaning();
    yaw_plus_roll_adds_turn_and_lean();
    local_space_turns_around_its_tipped_axis();
    player_and_world_turn_the_same_however_tipped();
    player_space_caps_an_out_of_line_turn();
    world_space_pitch_fades_on_the_side();
    rotate_output_turns_clockwise();
    player_space_through_the_turn();
    gravity_starts_flat_then_takes_the_first_reading();
    gravity_settles_on_the_accelerometer();
    gravity_follows_the_gyro();
    gravity_takes_off_the_gyros_offset();
    gravity_trusts_a_shaken_reading_less();
    gravity_ignores_a_small_wobble_while_still();
    puts("gyro tests passed");
    return 0;
}
