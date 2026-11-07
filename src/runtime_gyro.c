/* Built-in gyro: the camera turn from gyro samples, and calibration (runtime_gyro.h). */
#include "runtime_gyro.h"
#include <math.h>
#include <string.h>

#define DEGREES_PER_RADIAN 57.29578f
#define STILL_TOLERANCE (1.0f/DEGREES_PER_RADIAN) /* rad/s from the mean: still */
#define AUTO_BIAS_MAX (1.5f/DEGREES_PER_RADIAN)   /* rad/s: a faster steady turn is aiming */
#define MANUAL_BIAS_MAX (10.0f/DEGREES_PER_RADIAN)
#define STILL_SECONDS 1.5f

void gyro_calibration_reset(GyroCalibration *c) { memset(c,0,sizeof(*c)); }

int gyro_calibration_add(GyroCalibration *c, const float rate[3], float dt) {
    int moved=0;
    if (c->still_count) {
        for (int a=0;a<3;++a)
            if (fabsf(rate[a]-c->still_sum[a]/(float)c->still_count)>STILL_TOLERANCE) moved=1;
    }
    if (moved) { /* start again from this sample */
        c->still_seconds=0.0f; c->still_count=0;
        memset(c->still_sum,0,sizeof(c->still_sum));
    }
    for (int a=0;a<3;++a) c->still_sum[a]+=rate[a];
    ++c->still_count;
    c->still_seconds+=dt;
    if (c->still_seconds<STILL_SECONDS) return 0;
    float mean[3];
    int too_fast=0;
    for (int a=0;a<3;++a) {
        mean[a]=c->still_sum[a]/(float)c->still_count;
        if (fabsf(mean[a])>(c->manual ? MANUAL_BIAS_MAX : AUTO_BIAS_MAX)) too_fast=1;
    }
    c->still_seconds=0.0f; c->still_count=0;
    memset(c->still_sum,0,sizeof(c->still_sum));
    if (too_fast) return 0;
    memcpy(c->bias,mean,sizeof(mean));
    c->calibrated=1;
    c->manual=0;
    return 1;
}

void gyro_filter_reset(GyroFilter *f) { memset(f,0,sizeof(*f)); }

#define OWED_MAX 2.0f      /* degrees held back at most, per axis */
#define PAY_BACK_SECONDS 0.1f

/* Player Space, World Space and the gravity tracker are ported from JibbSmart's GamepadMotionHelpers
 * (GamepadMotion.hpp version 10: CalculatePlayerSpaceGyro, CalculateWorldSpaceGyro, Motion::Update), whose
 * licence follows:
 *
 * Copyright (c) 2020-2023 Julian "Jibb" Smart
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
 * associated documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the
 * following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
 * LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO
 * EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE. */

#define STANDARD_GRAVITY 9.80665f               /* m/s^2 per g */
#define PLAYER_YAW_RELAX 1.41f                  /* CalculatePlayerSpaceGyro's yawRelaxFactor */
#define WORLD_SIDE_REDUCTION 0.125f             /* CalculateWorldSpaceGyro's sideReductionThreshold */
#define SHAKE_HALF_TIME 0.25f                   /* Motion::ShortSteadinessHalfTime, s */
#define GRAVITY_STILL_SPEED 1.0f                /* GamepadMotionSettings' GravityCorrection*, g/s */
#define GRAVITY_SHAKY_SPEED 0.1f
#define GRAVITY_SHAKY_MIN 0.01f                 /* shakiness (g) from which the correction slows ... */
#define GRAVITY_SHAKY_MAX 0.4f                  /* ... to the shaky speed */
#define GRAVITY_GYRO_FACTOR 0.1f                /* no faster than this x the turning speed (rad/s) ... */
#define GRAVITY_MINIMUM_SPEED 0.01f             /* ... or this, g/s ... */
#define GRAVITY_GYRO_MIN 0.05f                  /* ... while it is this close (g); free from this far */
#define GRAVITY_GYRO_MAX 0.25f

static float clamp01(float x) { return fminf(fmaxf(x,0.0f),1.0f); }
static float dot3(const float a[3], const float b[3]) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }

/* v turned by -angle around the unit axis k: a vector fixed in the room, seen from a controller that turned
 * by +angle (Rodrigues). */
static void unturn(float v[3], const float k[3], float angle) {
    const float c=cosf(angle), s=sinf(angle), kv=dot3(k,v);
    const float cross[3]={k[1]*v[2]-k[2]*v[1],k[2]*v[0]-k[0]*v[2],k[0]*v[1]-k[1]*v[0]};
    for (int a=0;a<3;++a) v[a]=v[a]*c-cross[a]*s+k[a]*kv*(1.0f-c);
}

void gyro_gravity_reset(GyroGravity *g) {
    memset(g,0,sizeof(*g));
    g->grav[1]=-1.0f;
}

void gyro_gravity_add(GyroGravity *g, const GyroCalibration *c, const float rate[3], const float accel[3], float dt) {
    const float w[3]={rate[0]-c->bias[0],rate[1]-c->bias[1],rate[2]-c->bias[2]};
    const float speed=sqrtf(dot3(w,w)); /* rad/s */
    if (speed>0.0f) {
        const float k[3]={w[0]/speed,w[1]/speed,w[2]/speed};
        unturn(g->grav,k,speed*dt);
        unturn(g->smooth_accel,k,speed*dt);
    }
    const float a[3]={accel[0]/STANDARD_GRAVITY,accel[1]/STANDARD_GRAVITY,accel[2]/STANDARD_GRAVITY};
    const float length=sqrtf(dot3(a,a));
    if (length<=0.0f) return; /* no reading: the gyro alone */
    const float down[3]={-a[0]/length,-a[1]/length,-a[2]/length};
    if (!g->seen) { /* the first reading: take it (GamepadMotionHelpers starts from nothing and eases in) */
        memcpy(g->grav,down,sizeof(down));
        memcpy(g->smooth_accel,a,sizeof(a));
        g->seen=1;
        return;
    }
    const float smooth=exp2f(-dt/SHAKE_HALF_TIME);
    float shake[3];
    for (int i=0;i<3;++i) shake[i]=a[i]-g->smooth_accel[i];
    g->shakiness=fmaxf(g->shakiness*smooth,sqrtf(dot3(shake,shake)));
    for (int i=0;i<3;++i) g->smooth_accel[i]=a[i]+(g->smooth_accel[i]-a[i])*smooth;
    float to[3];
    for (int i=0;i<3;++i) to[i]=down[i]-g->grav[i];
    const float off=sqrtf(dot3(to,to));
    float correction=GRAVITY_STILL_SPEED+(GRAVITY_SHAKY_SPEED-GRAVITY_STILL_SPEED)*
        clamp01((g->shakiness-GRAVITY_SHAKY_MIN)/(GRAVITY_SHAKY_MAX-GRAVITY_SHAKY_MIN));
    const float limit=fmaxf(speed*GRAVITY_GYRO_FACTOR,GRAVITY_MINIMUM_SPEED);
    if (correction>limit)
        correction=limit+(correction-limit)*clamp01((off-GRAVITY_GYRO_MIN)/(GRAVITY_GYRO_MAX-GRAVITY_GYRO_MIN));
    const float step=correction*dt;
    if (off>0.0f && step<off) { for (int i=0;i<3;++i) g->grav[i]+=to[i]/off*step; }
    else memcpy(g->grav,down,sizeof(down));
}

void gyro_space_rates(const GyroSettings *s, const float rate[3], const float grav[3], float *pitch, float *yaw) {
    float d[3]={0.0f,-1.0f,0.0f};
    if (grav) {
        const float length=sqrtf(dot3(grav,grav));
        if (length>0.0f) for (int a=0;a<3;++a) d[a]=grav[a]/length;
    }
    *pitch=rate[0];
    switch (s->space) {
    default:
    case GYRO_SPACE_YAW: *yaw=rate[1]; break;
    case GYRO_SPACE_ROLL: *yaw=rate[2]; break;     /* lean left (right side up): left */
    case GYRO_SPACE_YAW_ROLL: *yaw=rate[1]+s->local_complement*rate[2]; break; /* Roll Axis Contribution */
    case GYRO_SPACE_LOCAL: {
        const float t=s->local_angle/DEGREES_PER_RADIAN, ct=cosf(t), st=sinf(t);
        /* the primary axis tilted from yaw (0) to roll (90), and the axis at 90 degrees to it (at 0 the roll
         * axis, so a full complement there is Yaw + Roll) */
        *yaw=(rate[1]*ct+rate[2]*st)+s->local_complement*(rate[2]*ct-rate[1]*st);
        break;
    }
    case GYRO_SPACE_PLAYER: {
        /* CalculatePlayerSpaceGyro: gravity says which way yaw and roll together turn, never how much */
        const float world=-(d[1]*rate[1]+d[2]*rate[2]);
        const float both=sqrtf(rate[1]*rate[1]+rate[2]*rate[2]);
        *yaw=(world<0.0f ? -1.0f : 1.0f)*fminf(fabsf(world)*PLAYER_YAW_RELAX,both);
        break;
    }
    case GYRO_SPACE_WORLD: {
        /* CalculateWorldSpaceGyro: yaw around gravity, pitch around the side-to-side axis laid flat */
        *yaw=-dot3(d,rate);
        const float axis[3]={1.0f-d[0]*d[0],-d[1]*d[0],-d[2]*d[0]};
        const float length=sqrtf(dot3(axis,axis));
        if (length>0.0f) {
            const float side=clamp01((fmaxf(fabsf(d[1]),fabsf(d[2]))-WORLD_SIDE_REDUCTION)/WORLD_SIDE_REDUCTION);
            *pitch=side*dot3(axis,rate)/length;
        } else {
            *pitch=0.0f;
        }
        break;
    }
    }
}

void gyro_turn(const GyroSettings *s, const GyroCalibration *c, GyroFilter *f, const float rate[3], float dt,
               const float grav[3], float *right_deg, float *up_deg) {
    const float calibrated[3]={rate[0]-c->bias[0],rate[1]-c->bias[1],rate[2]-c->bias[2]};
    float pitch, yaw;
    gyro_space_rates(s,calibrated,grav,&pitch,&yaw);
    /* deg/s of the controller, right and up (SDL: yaw left is positive) */
    const float v[2]={-yaw*DEGREES_PER_RADIAN,pitch*DEGREES_PER_RADIAN};
    const float speed=sqrtf(v[0]*v[0]+v[1]*v[1]);
    float scale=1.0f;
    if (speed<s->speed_deadzone) scale=0.0f;
    else if (speed<s->precision_speed) scale=speed/s->precision_speed;
    const float fast=2.0f*fmaxf(s->speed_deadzone,s->precision_speed);
    float out[2];
    for (int a=0;a<2;++a) {
        out[a]=v[a]*scale*dt;
        f->owed[a]=fminf(fmaxf(f->owed[a]+v[a]*(1.0f-scale)*dt,-OWED_MAX),OWED_MAX);
        if (scale==1.0f && speed>=fast && fast>0.0f) {
            if (f->owed[a]*v[a]>0.0f) { /* moving the way it was held back: give it back */
                const float pay=f->owed[a]*fminf(1.0f,dt/PAY_BACK_SECONDS);
                out[a]+=pay; f->owed[a]-=pay;
            } else {
                f->owed[a]=0.0f;
            }
        }
    }
    if (s->rotate!=0.0f) { /* Rotate Output: clockwise on the screen */
        const float t=s->rotate/DEGREES_PER_RADIAN, ct=cosf(t), st=sinf(t), right=out[0], up=out[1];
        out[0]=right*ct+up*st;
        out[1]=up*ct-right*st;
    }
    *right_deg=out[0]*s->sensitivity*(s->invert_x ? -1.0f : 1.0f);
    *up_deg=out[1]*s->sensitivity*(s->invert_y ? -1.0f : 1.0f);
}

#define GYRO_PLAYBACK_DELAY_MS 10.0   /* how far behind the newest sample the playhead aims */
#define PLAYBACK_FOLLOW_SECONDS 0.5   /* how slowly it follows the newest sample's clumpy arrival */
#define PLAYBACK_RESYNC_MS 60.0       /* further off than this (a gap, a stall): jump */
#define PLAYBACK_AHEAD_MS 40.0        /* at most this long carried on at the last speed */
#define PLAYBACK_KEEP_MS 100.0        /* samples kept behind the playhead */
#define PLAYBACK_SPEED_MS 8.0         /* the last speed: over this much sensor time */

void gyro_playback_reset(GyroPlayback *p) { memset(p,0,sizeof(*p)); }

static int point(const GyroPlayback *p, int i) { return (p->first+i)%GYRO_PLAYBACK_POINTS; }

void gyro_playback_add(GyroPlayback *p, uint64_t ns, float right_deg, float up_deg) {
    if (p->count && ns<=p->ns[point(p,p->count-1)]) return; /* out of order: keep the clock moving forward */
    if (p->count==GYRO_PLAYBACK_POINTS) { p->first=point(p,1); --p->count; }
    p->total[0]+=right_deg; p->total[1]+=up_deg;
    const int i=point(p,p->count++);
    p->ns[i]=ns;
    p->turn[i][0]=p->total[0]; p->turn[i][1]=p->total[1];
}

/* The turn so far at sensor time t: between samples a straight line, past the newest one the last speed
 * for at most PLAYBACK_AHEAD_MS. */
static void turn_at(const GyroPlayback *p, double t, double out[2]) {
    const int newest=point(p,p->count-1);
    if (t>=(double)p->ns[newest]) {
        const double ahead=fmin(t-(double)p->ns[newest],PLAYBACK_AHEAD_MS*1e6);
        int k=p->count-1;
        while (k>0 && (double)p->ns[newest]-(double)p->ns[point(p,k)]<PLAYBACK_SPEED_MS*1e6) --k;
        const int from=point(p,k);
        const double span=(double)p->ns[newest]-(double)p->ns[from];
        for (int a=0;a<2;++a)
            out[a]=p->turn[newest][a]+(span>0.0 ? (p->turn[newest][a]-p->turn[from][a])/span*ahead : 0.0);
        return;
    }
    int lo=0, hi=p->count-1; /* the last sample at or before t */
    while (lo<hi) { const int mid=(lo+hi+1)/2; if ((double)p->ns[point(p,mid)]<=t) lo=mid; else hi=mid-1; }
    const int a0=point(p,lo);
    if (lo==p->count-1 || t<=(double)p->ns[a0]) { out[0]=p->turn[a0][0]; out[1]=p->turn[a0][1]; return; }
    const int a1=point(p,lo+1);
    const double f=(t-(double)p->ns[a0])/((double)p->ns[a1]-(double)p->ns[a0]);
    for (int a=0;a<2;++a) out[a]=p->turn[a0][a]+(p->turn[a1][a]-p->turn[a0][a])*f;
}

void gyro_playback_take(GyroPlayback *p, float dt, float *right_deg, float *up_deg) {
    *right_deg=*up_deg=0.0f;
    if (!p->count) return;
    const double aim=(double)p->ns[point(p,p->count-1)]-GYRO_PLAYBACK_DELAY_MS*1e6;
    if (!p->playing) {
        p->playing=1;
        p->playhead=fmax(aim,(double)p->ns[p->first]);
    } else if (fabs(aim-p->playhead)>PLAYBACK_RESYNC_MS*1e6) {
        p->playhead=fmax(p->playhead,aim); /* never backwards: what was shown stays shown */
    } else {
        const double follow=1.0-exp(-(double)dt/PLAYBACK_FOLLOW_SECONDS);
        const double ahead=p->playhead+(double)dt*1e9; /* where the clock alone takes it */
        p->playhead=fmax(p->playhead,ahead+(aim-ahead)*follow);
    }
    double now[2];
    turn_at(p,p->playhead,now);
    *right_deg=(float)(now[0]-p->shown[0]);
    *up_deg=(float)(now[1]-p->shown[1]);
    p->shown[0]=now[0]; p->shown[1]=now[1];
    while (p->count>2 && (double)p->ns[point(p,1)]<p->playhead-PLAYBACK_KEEP_MS*1e6) {
        p->first=point(p,1); --p->count;
    }
}
