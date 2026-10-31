/* Critically damped angular target: position and velocity stay continuous. */
#ifndef BB_MOUSE_AIM_FILTER_H
#define BB_MOUSE_AIM_FILTER_H
#include <math.h>
#include <stdint.h>

typedef struct {
    double remaining;
    double velocity;
    uint64_t last_us;
} MouseAimFilter;

static inline void mouse_aim_filter_reset(MouseAimFilter *filter) {
    filter->remaining=0;
    filter->velocity=0;
    filter->last_us=0;
}

static inline float mouse_aim_filter_step(MouseAimFilter *filter, float pixels,
                                         uint64_t now_us, float smoothing_ms) {
    const uint64_t previous=filter->last_us;
    if (smoothing_ms<=0) {
        const float result=(float)(filter->remaining+pixels);
        filter->remaining=0;
        filter->velocity=0;
        filter->last_us=now_us;
        return result;
    }
    if (previous && now_us<=previous) {
        filter->remaining+=pixels;
        return 0;
    }
    double dt=1.0/120.0;
    if (previous && now_us-previous<=100000) dt=(double)(now_us-previous)*0.000001;
    else {
        /* Release/camera changes reset the filter in the caller. Do not replay an
         * old tail after a long gap, or snap the new movement on its first tick. */
        filter->remaining=0;
        filter->velocity=0;
    }
    filter->last_us=now_us;
    filter->remaining+=pixels; /* Move the desired angle, not the displayed angle. */
    const double omega=1000.0/smoothing_ms;
    const double decay=exp(-omega*dt);
    const double change=(filter->velocity-omega*filter->remaining)*dt;
    const double error=(filter->remaining-change)*decay;
    double result=filter->remaining-error;
    filter->remaining=error;
    filter->velocity=(filter->velocity-omega*change)*decay;
    if (fabs(filter->remaining)<0.000001 && fabs(filter->velocity)<0.000001) {
        result+=filter->remaining;
        filter->remaining=0;
        filter->velocity=0;
    }
    return (float)result;
}
#endif
