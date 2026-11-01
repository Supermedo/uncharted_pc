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

void gyro_turn(const GyroSettings *s, const GyroCalibration *c, GyroFilter *f, const float rate[3], float dt,
               float *right_deg, float *up_deg) {
    /* deg/s of the controller, right and up (SDL: yaw left is positive) */
    const float v[2]={-(rate[1]-c->bias[1])*DEGREES_PER_RADIAN,(rate[0]-c->bias[0])*DEGREES_PER_RADIAN};
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
