/* Build like test_pad.c: link SDL3 and pthreads, with -I. -Isrc. */
#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "../src/runtime_pad.c"

_Thread_local int runtime_ndgi_open;
static int capture, host_captured;
static float host_dx, host_dy;
static uint32_t host_buttons;
int bbgpu_overlay_captures_input(void) { return capture; }
void bbgpu_mouse_configure(int look) { (void)look; }
int bbgpu_mouse_sample(float *dx, float *dy, uint32_t *buttons, int *wheel) {
    *dx=host_dx; *dy=host_dy; *buttons=host_buttons; *wheel=0;
    host_dx=host_dy=0;
    return host_captured;
}
int bbgpu_camera_yaw(float *yaw, double *age) { (void)yaw; (void)age; return 0; }
void bbgpu_prompts_controller(int device) { (void)device; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

/* The actual thunk leaves the entry return address after its saved-register block. */
typedef struct { RuntimeGuestRegs regs; uint64_t return_address; } Entry;
_Static_assert(offsetof(Entry,return_address)==sizeof(RuntimeGuestRegs),"entry stack layout");
static const uint64_t image=0x100000000ull;
static uint64_t session;
static void set_mouse_profile(float look_x, float look_y, float aim_x, float aim_y, int inverted) {
    const MouseSettings settings={look_x,look_y,aim_x,aim_y,1,inverted};
    publish_mouse_settings(&settings);
}
static void fresh_aim(void) {
    if (!session) session=now_us()-10000;
    aim_press_us=++session;
    aim_held=1;
    aim_dx=aim_dy=0.0f;
    set_mouse_profile(1,1,1,1,0);
}
static Entry entry_for(float *dir, uint64_t offset) {
    /* Poison the old fork's indices: rcx was regs[6], return address was regs[9]. */
    Entry entry={.regs={.r9=1,.rcx=(uint64_t)(uintptr_t)dir},.return_address=image+offset};
    return entry;
}
static void assert_close(float actual, float expected) { assert(SDL_fabsf(actual-expected)<0.01f); }
static float pitch_of(const float *dir) {
    return SDL_atan2f(dir[1],SDL_sqrtf(dir[0]*dir[0]+dir[2]*dir[2]))*57.29578f;
}
static void aim_at(float *dir, uint64_t controller, float dx, float dy) {
    Entry entry=entry_for(dir,0xc4c57);
    aim_dx+=dx; aim_dy+=dy;
    u1_102_aim_controller_c(0,controller,image,0,&entry.regs);
}
static void test_aim_callbacks(float scale) {
    const uint64_t returns[]={0xc4c57,0x451250,0x511894,0xc4b67,0x18cc5f,0x13c621};
    void (__attribute__((ms_abi)) *callbacks[])(uint64_t,uint64_t,uint64_t,uint64_t,const RuntimeGuestRegs *)={
        u1_102_aim_controller_c,u2_102_aim_controller,u3_102_aim_controller,
        u1_eu_aim_controller,u2_eu_aim_controller,u3_eu_aim_controller,
    };
    for (int i=0;i<6;++i) {
        fresh_aim();
        float dir[]={0,0,-1};
        Entry entry=entry_for(dir,returns[i]);
        aim_dx=100; aim_dy=20;
        callbacks[i](0,(uint64_t)(i+1)*0x1000,image,0,&entry.regs);
        assert_close(heading_of(dir),-15.0f*scale);
        assert_close(pitch_of(dir),-3.0f*scale);
        assert_close(aim_dx,0); assert_close(aim_dy,0);
        assert(raw_aim_armed(now_us()));
    }
}
static void test_call_filter_and_blending(void) {
    fresh_aim();
    float a[]={0,0,-1}, b[]={-1,0,0};
    Entry other=entry_for(a,0x4d09ce);
    aim_dx=100;
    u3_102_aim_controller(0,0x1000,image,0,&other.regs);
    assert_close(a[0],0); assert_close(aim_dx,100); /* another caller must not consume the motion */
    aim_at(a,0x1000,0,0);
    assert_close(heading_of(a),-15);
    aim_at(b,0x2000,0,0); /* a roll creates a camera with its own initial direction */
    assert_close(heading_of(b),75);
    aim_at(a,0x1000,20,0);
    aim_at(b,0x2000,0,0);
    assert_close(heading_of(a),-18); assert_close(heading_of(b),72);
    aim_at(a,0x1000,0,0);
    assert_close(heading_of(a),-18); /* returning to a camera must not replay the same turn */
    float inherited[]={a[0],a[1],a[2]};
    aim_at(inherited,0x3000,0,0);
    assert_close(heading_of(inherited),-18); /* a camera that copied the direction already has the turn */
    aim_turn(a,2,1); /* the game's own aim assist/recoil changes must survive */
    aim_at(a,0x1000,0,0);
    assert_close(heading_of(a),-20); assert_close(pitch_of(a),1);
}
static void test_motion_and_sticks(void) {
    fresh_aim();
    float whole[]={0,0,-1}, parts[]={0,0,-1};
    aim_at(whole,0x1000,1000,0); /* 150 degrees: no joystick speed cap */
    assert_close(heading_of(whole),-150);
    fresh_aim();
    for (int i=0;i<10;++i) aim_at(parts,0x2000,100,0);
    for (int i=0;i<3;++i) assert_close(parts[i],whole[i]);
    fresh_aim();
    float inverted[]={0,0,-1};
    set_mouse_profile(1,1,1,1,1);
    aim_at(inverted,0x3000,0,20);
    assert_close(pitch_of(inverted),3);
    set_mouse_profile(1,1,1,1,0);
    const AimCurve *curves[]={&u1_100_aim,&u1_102_aim,&u23_100_aim,&u23_102_aim};
    for (int i=0;i<4;++i) {
        float pad[]={0,0,.25f,-.3f,0,0,.25f,-.3f};
        camera_dx[1]=100; camera_dy[1]=50;
        mouse_sticks(pad,curves[i]);
        assert_close(pad[2],.25f); assert_close(pad[3],-.3f);
        assert_close(pad[6],.25f); assert_close(pad[7],-.3f); /* preserve the physical stick, add no mouse twice */
    }
    aim_direct_us=0;
    assert(!raw_aim_armed(now_us()));
    float fallback[8]={0};
    camera_dx[1]=100;
    mouse_sticks(fallback,&u1_102_aim);
    assert(fallback[2]>0 && fallback[6]>0); /* unsupported builds keep stick aiming */
    float direction[]={0,0,-1};
    aim_turn(direction,30,1000);
    assert(pitch_of(direction)<70.01f);
    assert_close(SDL_sqrtf(direction[0]*direction[0]+direction[1]*direction[1]+direction[2]*direction[2]),1);
}
static void test_cover_callbacks(void) {
    const size_t direction_offsets[]={0x240,0x1c0,0x2f0};
    const size_t pitch_offsets[]={0x230,0x1b0,0x2e0};
    for (int i=0;i<3;++i) {
        unsigned char *camera=calloc(1,0x1400);
        assert(camera);
        float limits[]={0,0,35,45};
        float *dir=(float *)(camera+direction_offsets[i]);
        float *pitch=(float *)(camera+pitch_offsets[i]);
        dir[2]=-1;
        if (i<2) *(const float **)(camera+(i==0 ? 0x1300 : 0x1290))=limits;
        else { *(float *)(camera+0x218)=35; *(float *)(camera+0x21c)=45; }
        usleep(6000); /* begin a new frame_motion sample */
        aim_held=0;
        cover_hook_us=camera_hook_us=now_us();
        camera_dx[0]=100; camera_dy[0]=1000;
        const uint64_t ptr=(uint64_t)(uintptr_t)camera;
        if (i==0) u1_102_cover_camera(0,ptr,image,0,NULL);
        if (i==1) u2_102_cover_camera(0,ptr,image,0,NULL);
        if (i==2) u3_102_cover_camera(ptr,0,image,0,NULL);
        assert_close(heading_of(dir),-15); assert_close(*pitch,-45);
        float pad[8]={0};
        camera_dx[1]=100; camera_dy[1]=100;
        follow_correction=90; follow_push=.3f;
        mouse_sticks(pad,&u23_102_aim);
        for (int j=0;j<8;++j) assert_close(pad[j],0);
        free(camera);
    }
}
static void test_follow_blend_and_pitch(void) {
    struct { float dir[4], arc; } a={{0,0,-1,0},.74f}, b=a;
    aim_held=0; follow_direct=1; follow_kept=1;
    memset(follow_states,0,sizeof(follow_states));
    camera_hook_us=now_us();
    usleep(6000);
    camera_dx[0]=100; camera_dy[0]=100;
    follow_camera((uint64_t)(uintptr_t)&a,0,16,&u2_arc,.75f);
    follow_camera((uint64_t)(uintptr_t)&b,0,16,&u2_arc,.75f);
    assert_close(heading_of(a.dir),-15); assert_close(heading_of(b.dir),-15);
    assert_close(a.arc,.75f); assert_close(b.arc,.75f);
    assert(follow_state((uint64_t)(uintptr_t)&a,now_us())!=
           follow_state((uint64_t)(uintptr_t)&b,now_us()));
}
static void test_host_motion_and_capture(void) {
    fresh_aim();
    float dir[]={0,0,-1};
    aim_at(dir,0x1000,0,0);
    sdl_ready=-1; /* these tests have no physical gamepad */
    host_captured=1; host_buttons=2; /* right mouse button: aim */
    PadData pad;
    host_dx=40;
    sample_host(&pad);
    host_dx=60;
    sample_host(&pad);
    assert_close(aim_dx,100);
    assert(pad.right_x==128 && pad.right_y==128);
    aim_at(dir,0x1000,0,0);
    assert_close(heading_of(dir),-15);
    capture=1; host_dx=100;
    sample_host(&pad);
    assert_close(aim_dx,0); /* the host settings overlay takes the input */
    capture=0; host_captured=0; host_dx=100;
    sample_host(&pad);
    assert_close(aim_dx,0); /* released capture must not queue a turn */
    host_captured=1; host_buttons=0;
    aim_held=0;
    sample(&pad);
    host_buttons=2;
    sample(&pad);
    assert(aim_held && aim_press_us>session); /* a new aim press starts a new blend session */
    injected.mouse[0]=1000;
    usleep(6000);
    sample(&pad);
    assert(aim_dx>0); /* scripted motion reaches the same raw aim queue */
    injected.mouse[0]=0;
    host_captured=0; host_buttons=0;
}
static void test_independent_mouse_axes(void) {
    fresh_aim();
    set_mouse_profile(2,3,.5f,4,0);
    float dir[]={0,0,-1};
    aim_at(dir,0x1000,100,10);
    assert_close(heading_of(dir),-7.5f);
    assert_close(pitch_of(dir),-6);
    aim_held=0;
    usleep(6000);
    cover_hook_us=camera_hook_us=now_us();
    camera_dx[0]=20; camera_dy[0]=10;
    float cover_dir[]={0,0,-1}, cover_pitch=0, limits[]={70,70};
    cover_camera(cover_dir,&cover_pitch,limits);
    assert_close(heading_of(cover_dir),-6);
    assert_close(cover_pitch,-4.5f);
    usleep(6000);
    camera_dx[0]=20; camera_dy[0]=10;
    struct { float dir[4], arc; } follow={{0,0,-1,0},.5f};
    memset(follow_states,0,sizeof(follow_states));
    follow_direct=1; follow_kept=1;
    follow_camera((uint64_t)(uintptr_t)&follow,0,16,&u2_arc,1);
    assert_close(heading_of(follow.dir),-6);
    assert_close(arc_to_pitch(&u2_arc,follow.arc),-12.5f);
}
static void write_controls(const char *path, const char *text) {
    FILE *file=fopen(path,"w");
    assert(file);
    assert(fputs(text,file)>=0);
    assert(fclose(file)==0);
}
static void test_live_mouse_settings(void) {
    char path[]="bbport-mouse-settings-XXXXXX";
    int fd=mkstemp(path);
    assert(fd>=0);
    close(fd);
    const char *previous_path=input_config_path;
    fresh_aim();
    float dir[]={0,0,-1};
    aim_at(dir,0x1000,0,0);
    input_config_path=path;
    mouse_config_hash_valid=0;
    mouse_config_poll_us=0;
    uint64_t now=now_us();
    write_controls(path,"mouse_sensitivity=1.5\nmouse_look_sensitivity_x=2.0\nmouse_look_sensitivity_y=3.0\n"
                        "mouse_aim_sensitivity_x=0.5\nmouse_aim_sensitivity_y=4.0\n");
    reload_mouse_settings(now);
    MouseSettings settings=mouse_settings();
    assert_close(settings.look_x,2); assert_close(settings.look_y,3);
    aim_at(dir,0x1000,10,10);
    assert_close(heading_of(dir),-.75f); assert_close(pitch_of(dir),-6);
    /* Same-sized edit within one second: an mtime/size-only watcher would miss it. */
    write_controls(path,"mouse_sensitivity=1.5\nmouse_look_sensitivity_x=4.0\nmouse_look_sensitivity_y=2.0\n"
                        "mouse_aim_sensitivity_x=1.5\nmouse_aim_sensitivity_y=2.0\n");
    const uint64_t held_press=aim_press_us;
    reload_mouse_settings(now+=50001);
    assert(aim_held && aim_press_us==held_press);
    settings=mouse_settings();
    assert_close(settings.look_x,4); assert_close(settings.look_y,2);
    aim_at(dir,0x1000,10,10);
    assert_close(heading_of(dir),-3); assert_close(pitch_of(dir),-9);
    write_controls(path,"mouse_sensitivity=9\nmouse_look_sensitivity_x=nan\nmouse_look_sensitivity_y=inf\n"
                        "mouse_aim_sensitivity_x=invalid\nmouse_aim_sensitivity_y=0\n");
    reload_mouse_settings(now+=50001);
    settings=mouse_settings();
    assert_close(settings.look_x,4); assert_close(settings.look_y,2);
    assert_close(settings.aim_x,1.5f); assert_close(settings.aim_y,2);
    write_controls(path,"mouse_sensitivity=2.25\nmouse_invert_y=1\n");
    reload_mouse_settings(now+=50001);
    settings=mouse_settings();
    assert_close(settings.look_x,2.25f); assert_close(settings.look_y,2.25f);
    assert_close(settings.aim_x,2.25f); assert_close(settings.aim_y,2.25f);
    assert(settings.invert_y);
    write_controls(path,"mouse_look=0\n");
    reload_mouse_settings(now+=50001);
    const float before[3]={dir[0],dir[1],dir[2]};
    aim_at(dir,0x1000,100,100);
    for (int i=0;i<3;++i) assert_close(dir[i],before[i]);
    input_config_path=previous_path;
    mouse_config_poll_us=0;
    mouse_config_hash_valid=0;
    set_mouse_profile(1,1,1,1,0);
    unlink(path);
}
int main(int argc, char **argv) {
    (void)argv;
    if (argc>1 && !strcmp(argv[1],"watch-controls")) {
        /* Cross-process fixture: a launcher can change input.ini while this runtime stays alive. */
        setvbuf(stdout,NULL,_IONBF,0);
        load_input();
        uint64_t previous=0;
        for (int i=0;i<1000;++i) {
            reload_mouse_settings(now_us());
            if (!i || previous!=mouse_config_hash) {
                const MouseSettings settings=mouse_settings();
                printf("LIVE %.3f %.3f %.3f %.3f %d %d\n",settings.look_x,settings.look_y,
                       settings.aim_x,settings.aim_y,settings.enabled,settings.invert_y);
                previous=mouse_config_hash;
            }
            usleep(5000);
        }
        return 0;
    }
    if (argc>1 && !strcmp(argv[1],"disabled")) {
        fresh_aim();
        float dir[]={0,0,-1};
        aim_at(dir,0x1000,100,20);
        assert_close(heading_of(dir),0); assert_close(pitch_of(dir),0);
        assert(!raw_aim_armed(now_us()));
        puts("PASS: BB_AIM_DIRECT=0 retains stick aiming");
        return 0;
    }
    if (argc>1 && !strcmp(argv[1],"scale")) {
        test_aim_callbacks(2);
        puts("PASS: BB_AIM_SCALE=2 for all three games");
        return 0;
    }
    test_aim_callbacks(1);
    test_call_filter_and_blending();
    test_motion_and_sticks();
    test_cover_callbacks();
    test_follow_blend_and_pitch();
    test_host_motion_and_capture();
    test_independent_mouse_axes();
    test_live_mouse_settings();
    puts("PASS: raw mouse, US/EU, blends, recoil, four axes, live updates, fallback, cover, follow, capture");
    return 0;
}
