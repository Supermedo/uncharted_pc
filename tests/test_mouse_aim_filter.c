#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../src/mouse_aim_filter.h"

int main(void) {
    const unsigned rates[]={30,60,120,240};
    const float intervals[]={0,8,20,40};
    for (unsigned setting=0;setting<4;++setting)
    for (unsigned r=0;r<4;++r) {
        MouseAimFilter filter={0};
        uint64_t now=1000000, step=1000000/rates[r];
        double total=0;
        mouse_aim_filter_step(&filter,0,now,intervals[setting]);
        for (unsigned frame=0;frame<rates[r];++frame) {
            now+=step;
            total+=mouse_aim_filter_step(&filter,600.0f/rates[r],now,intervals[setting]);
        }
        for (unsigned frame=0;frame<rates[r];++frame) {
            now+=step;
            total+=mouse_aim_filter_step(&filter,0,now,intervals[setting]);
        }
        assert(fabs(total-600)<0.0001);
        assert(filter.remaining==0);
    }
    MouseAimFilter filter={0};
    uint64_t now=1000000;
    double raw_variance=0, smooth_variance=0;
    mouse_aim_filter_step(&filter,0,now,8);
    for (unsigned frame=0;frame<240;++frame) {
        now+=8333;
        const float raw=frame%2 ? 10 : 6;
        const float smooth=mouse_aim_filter_step(&filter,raw,now,8);
        if (frame>=120) {
            raw_variance+=(raw-8)*(raw-8);
            smooth_variance+=(smooth-8)*(smooth-8);
        }
    }
    assert(smooth_variance<raw_variance*0.3);
    printf("Synthetic alternating input at 120 Hz: %.1f%% less frame-step variance\n",
           100*(1-smooth_variance/raw_variance));
    /* Raw mode flushes any residual and exactly preserves the new displacement. */
    filter.remaining=2;
    assert(mouse_aim_filter_step(&filter,3,now,0)==5);
    assert(filter.remaining==0);
    /* Duplicate timestamps retain motion for the next actual tick. */
    assert(mouse_aim_filter_step(&filter,4,now,8)==0);
    assert(filter.remaining==4);
    assert(mouse_aim_filter_step(&filter,0,now+8333,8)>0);
    mouse_aim_filter_reset(&filter);
    assert(filter.last_us==0 && filter.remaining==0 && filter.velocity==0);
    /* A single mouse count must ease across frames, including the first movement
     * after reset. */
    now+=16666;
    const float first=mouse_aim_filter_step(&filter,1,now,8);
    assert(first>0 && first<0.4f);
    double micro_total=first;
    const float second=mouse_aim_filter_step(&filter,0,now+8333,8);
    assert(second>first && second<0.4f);
    micro_total+=second;
    for (unsigned frame=2;frame<40;++frame)
        micro_total+=mouse_aim_filter_step(&filter,0,now+frame*8333,8);
    assert(fabs(micro_total-1)<0.000001 && filter.remaining==0);
    /* Simultaneous movement on both axes follows the same temporal path. */
    MouseAimFilter horizontal={0}, vertical={0};
    for (unsigned frame=0;frame<40;++frame) {
        const float h=mouse_aim_filter_step(&horizontal,frame ? 0 : 1,now+frame*8333,8);
        const float v=mouse_aim_filter_step(&vertical,frame ? 0 : -2,now+frame*8333,8);
        assert(fabs(v+2*h)<0.000001);
    }
    printf("One-count movement: %.1f%% first frame, %.1f%% second frame; total %.6f\n",
           first*100,second*100,micro_total);
    puts("PASS: conserved distance, frame rates, raw mode, clock duplicates and reset");
}
