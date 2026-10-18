/* libScePad on SDL3 gamepads, keyboard and mouse. SDL events are pumped by the window thread
 * (gpu/shim/window.cpp); here state is only sampled. The controller, keyboard and mouse work
 * together; their bindings come from input.ini (BB_INPUT_CONFIG, written by the launcher):
 *
 *   key.<action>=<key or mouse name>[|<second>]   SDL key names ("Space", "Left Shift", "W"),
 *       "Mouse Left", "Mouse Right", "Mouse Middle", "Mouse 4", "Mouse 5", "Wheel Up",
 *       "Wheel Down"; actions: cross circle square triangle l1 r1 l2 r2 l3 r3 options touchpad
 *       up down left right, ls_up ls_down ls_left ls_right, rs_up rs_down rs_left rs_right.
 *   pad.<action>=<controller button>   SDL names (a b x y leftshoulder rightshoulder
 *       lefttrigger righttrigger leftstick rightstick back start dpup dpdown dpleft dpright
 *       touchpad misc1 paddle1..paddle4) or "none", for the button actions above.
 *   mouse_look=1  mouse_sensitivity=1.0  mouse_invert_y=0  pad_invert_y=0
 *
 * Defaults (no file): WASD move, mouse look, left button R2 (fire), right button L2 (aim),
 * Space Cross, C Circle, F Square, E Triangle, Q L1, G R1, Left Shift L3, V R3, Escape/Enter
 * Options, Tab touchpad, arrow keys and the wheel the d-pad, IJKL the right stick. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PAD_HANDLE 1

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, opened, sdl_ready;
static SDL_Gamepad *gamepad;
static size_t reads;
static uint8_t connected_count;

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
static uint8_t axis(int16_t v) { int x=(v+32768)>>8; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}
static void touch_click(PadData *d, int right) {
    d->buttons|=BTN_TOUCHPAD;
    d->touch_count=1;
    d->touches[0]=(PadTouch){.x=right ? 1440 : 480,.y=471,.id=0};
}

/* Opens the first gamepad SDL knows about; called under lock. */
static SDL_Gamepad *current_gamepad(void) {
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return NULL;
    if (gamepad && !SDL_GamepadConnected(gamepad)) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
    if (!gamepad) {
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        if (ids && count>0) {
            gamepad=SDL_OpenGamepad(ids[0]);
            if (gamepad) { ++connected_count; printf("Runtime: gamepad connected: %s\n",SDL_GetGamepadName(gamepad)); }
        }
        SDL_free(ids);
    }
    return gamepad;
}
/* Degrees of camera turn per mouse pixel at sensitivity 1 (0.08 felt "very slow", issue #18; the
 * launcher's slider and box go from 0.1 to 10 times this). */
#define MOUSE_DEGREES_PER_PIXEL 0.15f

/* ---- bindings (input.ini) ---------------------------------------------------------------- */
enum { BIND_KEY=1, BIND_MOUSE, BIND_WHEEL };
typedef struct { uint8_t kind; int16_t code; } Bind;
typedef struct { const char *name; uint32_t ps; const char *keys; const char *pad; } Action;
enum { A_LS_UP=16, A_LS_DOWN, A_LS_LEFT, A_LS_RIGHT, A_RS_UP, A_RS_DOWN, A_RS_LEFT, A_RS_RIGHT, ACTIONS };
static const Action actions[ACTIONS]={
    {"cross",BTN_CROSS,"Space","a"}, {"circle",BTN_CIRCLE,"C","b"}, {"square",BTN_SQUARE,"F","x"},
    {"triangle",BTN_TRIANGLE,"E","y"}, {"l1",BTN_L1,"Q","leftshoulder"}, {"r1",BTN_R1,"G","rightshoulder"},
    {"l2",BTN_L2,"Mouse Right","lefttrigger"}, {"r2",BTN_R2,"Mouse Left","righttrigger"},
    {"l3",BTN_L3,"Left Shift","leftstick"}, {"r3",BTN_R3,"V","rightstick"},
    {"options",BTN_OPTIONS,"Escape|Return","start"}, {"touchpad",BTN_TOUCHPAD,"Tab","back"},
    {"up",BTN_UP,"Up","dpup"}, {"down",BTN_DOWN,"Down","dpdown"},
    {"left",BTN_LEFT,"Left|Wheel Up","dpleft"}, {"right",BTN_RIGHT,"Right|Wheel Down","dpright"},
    {"ls_up",0,"W",NULL}, {"ls_down",0,"S",NULL}, {"ls_left",0,"A",NULL}, {"ls_right",0,"D",NULL},
    {"rs_up",0,"I",NULL}, {"rs_down",0,"K",NULL}, {"rs_left",0,"J",NULL}, {"rs_right",0,"L",NULL},
};
#define PAD_TRIGGER_LEFT (-2)
#define PAD_TRIGGER_RIGHT (-3)
static struct {
    Bind keys[ACTIONS][2];
    int pad[16];               /* SDL_GamepadButton, PAD_TRIGGER_*, or -1 */
    int mouse_look, mouse_invert_y, pad_invert_y;
    float mouse_sensitivity;
} input;

static Bind parse_bind(const char *name) {
    static const struct { const char *name; uint8_t kind; int16_t code; } mouse[]={
        {"Mouse Left",BIND_MOUSE,1}, {"Mouse Right",BIND_MOUSE,2}, {"Mouse Middle",BIND_MOUSE,4},
        {"Mouse 4",BIND_MOUSE,8}, {"Mouse 5",BIND_MOUSE,16}, {"Wheel Up",BIND_WHEEL,1}, {"Wheel Down",BIND_WHEEL,-1},
    };
    for (size_t i=0;i<sizeof(mouse)/sizeof(*mouse);++i)
        if (!SDL_strcasecmp(name,mouse[i].name)) return (Bind){mouse[i].kind,mouse[i].code};
    const SDL_Scancode code=SDL_GetScancodeFromName(name);
    return code==SDL_SCANCODE_UNKNOWN ? (Bind){0,0} : (Bind){BIND_KEY,(int16_t)code};
}
static void set_keys(int action, const char *value) {
    char text[128];
    snprintf(text,sizeof(text),"%s",value);
    input.keys[action][0]=input.keys[action][1]=(Bind){0,0};
    char *second=strchr(text,'|');
    if (second) *second++=0;
    if (*text) input.keys[action][0]=parse_bind(text);
    if (second && *second) input.keys[action][1]=parse_bind(second);
}
static int parse_pad(const char *value) {
    if (!SDL_strcasecmp(value,"lefttrigger")) return PAD_TRIGGER_LEFT;
    if (!SDL_strcasecmp(value,"righttrigger")) return PAD_TRIGGER_RIGHT;
    const SDL_GamepadButton button=SDL_GetGamepadButtonFromString(value);
    return button==SDL_GAMEPAD_BUTTON_INVALID ? -1 : (int)button;
}
static void load_input(void) {
    for (int a=0;a<ACTIONS;++a) {
        set_keys(a,actions[a].keys);
        if (a<16) input.pad[a]=parse_pad(actions[a].pad);
    }
    input.mouse_look=1; input.mouse_sensitivity=1.0f;
    const char *path=getenv("BB_INPUT_CONFIG");
    FILE *f=path && *path ? fopen(path,"r") : NULL;
    if (f) {
        char line[256];
        while (fgets(line,sizeof(line),f)) {
            line[strcspn(line,"\r\n")]=0;
            char *eq=strchr(line,'=');
            if (line[0]=='#' || !eq) continue;
            *eq=0;
            const char *key=line, *value=eq+1;
            if (!strcmp(key,"mouse_look")) input.mouse_look=atoi(value)!=0;
            else if (!strcmp(key,"mouse_sensitivity")) input.mouse_sensitivity=(float)atof(value);
            else if (!strcmp(key,"mouse_invert_y")) input.mouse_invert_y=atoi(value)!=0;
            else if (!strcmp(key,"pad_invert_y")) input.pad_invert_y=atoi(value)!=0;
            else for (int a=0;a<ACTIONS;++a) {
                if (!strncmp(key,"key.",4) && !strcmp(key+4,actions[a].name)) set_keys(a,value);
                else if (a<16 && !strncmp(key,"pad.",4) && !strcmp(key+4,actions[a].name)) input.pad[a]=parse_pad(value);
            }
        }
        fclose(f);
        printf("Runtime: controls from %s (mouse look %s, sensitivity %.2f)\n",path,
               input.mouse_look ? "on" : "off",input.mouse_sensitivity);
    }
    if (input.mouse_sensitivity<=0.0f) input.mouse_sensitivity=1.0f;
    bbgpu_mouse_configure(input.mouse_look);
}

/* Mouse motion as a right stick: the filtered speed (pixels per second) scaled to a deflection,
 * past the game's dead zone as soon as the mouse moves. */
static float mouse_vx, mouse_vy;
static uint64_t mouse_last_us;
static uint64_t wheel_until_us[2]; /* wheel up, wheel down: held briefly for the game to see */
/* A turn rate (degrees/s) as a stick byte, through the inverse of the U1 follow camera's response
 * measured below its speed ramp: about 240 x^2.5 degrees/s (x = 0.25: 7, 0.5: 37). */
static uint8_t mouse_axis(float rate) {
    const float r=rate<0 ? -rate : rate;
    if (r<0.5f) return 128;
    float out=SDL_powf(r/240.0f,0.4f);
    if (out>1.0f) out=1.0f;
    int x=(int)(128.0f+(rate<0 ? -out : out)*127.5f);
    return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x);
}
static int bind_down(const Bind *b, const bool *k, uint32_t mouse, uint64_t now) {
    switch (b->kind) {
    case BIND_KEY: return k && k[b->code];
    case BIND_MOUSE: return (mouse & (uint32_t)b->code)!=0;
    case BIND_WHEEL: return now<wheel_until_us[b->code>0 ? 0 : 1];
    default: return 0;
    }
}
static int action_down(int a, const bool *k, uint32_t mouse, uint64_t now) {
    return bind_down(&input.keys[a][0],k,mouse,now) || bind_down(&input.keys[a][1],k,mouse,now);
}

/* ---- mouse camera ------------------------------------------------------------------------
 * The games turn the camera from the right stick through their own curve, a speed ramp and a
 * top speed (U1: about 240 degrees/s, 45 while aiming), which no stick position can make feel like
 * a mouse. Where the camera's state is known, the mouse turns it directly instead: a callback at
 * the start of the camera's per-frame update (installed by the loader when the code matches)
 * rotates its direction by the mouse motion since the last frame. Elsewhere the mouse still
 * moves the right stick. */
static pthread_mutex_t camera_lock=PTHREAD_MUTEX_INITIALIZER;
/* Mouse motion (pixels) not yet given to the follow camera [0] and to the joypad state [1]: each
 * gets all of it (the follow camera does not read the joypad's right stick values, the other
 * cameras do not use the follow camera's direction). */
static float camera_dx[2], camera_dy[2];
static uint64_t camera_hook_us;       /* last follow camera callback: it turns with the mouse */
static uint64_t sticks_hook_us;       /* last joypad callback: the mouse is a right stick value there */
static float camera_test_rate=-1.0f;  /* BB_MOUSE_CAMERA_TEST=<degrees per frame>: constant turn */
static int mouse_debug(void) {
    static int on=-1;
    if (on<0) on=getenv("BB_MOUSE_DEBUG")!=NULL;
    return on;
}
static int camera_take(int queue, float *dx, float *dy) {
    pthread_mutex_lock(&camera_lock);
    *dx=camera_dx[queue]; *dy=camera_dy[queue];
    camera_dx[queue]=camera_dy[queue]=0;
    pthread_mutex_unlock(&camera_lock);
    if (camera_test_rate<0.0f) {
        const char *test=getenv("BB_MOUSE_CAMERA_TEST");
        camera_test_rate=test ? (float)atof(test) : 0.0f;
    }
    return 1;
}
/* The follow camera's collision/position update runs once a frame with the camera object (U1
 * CUSA02343 1.00 and others with the same code; U2 and U3 have the same function, found by its
 * instructions): its horizontal direction (x, y, z) at +0x1310 (U2 +0x9ec0, U3 +0x78c0) and the
 * vertical arc position at +0x12bc (U2 +0x9e60, U3 +0x7874: the right stick up lowers it). */
static float wrap_degrees(float a) {
    while (a>180.0f) a-=360.0f;
    while (a<-180.0f) a+=360.0f;
    return a;
}
static float heading_of(const float *dir) { return SDL_atan2f(-dir[0],-dir[2])*57.29578f; }
/* The follow camera keeps the turns of its direction while the right stick is held: with the stick
 * at rest the game sets the direction itself every few frames and most turns were lost (an uneven,
 * short turn). So while the mouse turns the direction, the joypad callback holds the stick a little
 * (0.3, about 13 degrees/s) the same way, and the mouse moves a wanted heading: each frame the
 * direction turns by the mouse's motion plus what the direction is still short of it (read from the
 * direction itself, which also takes out the stick's own turn; the shown camera follows the
 * direction with the game's smoothing, 1 to 8 frames late, too late to steer by). While the mouse
 * rests, the wanted heading follows the direction (the game's own camera moves stay free). Should
 * the camera keep no turns at all, the mouse goes to the joypad's right stick (the follow camera's
 * curve), with a small turn each frame to notice when turns are kept again. */
static volatile int follow_direct=1;
static float follow_kept=1.0f;            /* average fraction of a turn still there the next frame */
static float follow_before, follow_after; /* heading before and after the last turn */
static int follow_pending;
static volatile int aim_held;             /* L2 is held: the aiming camera (also while it blends in) */
static float follow_target;               /* wanted heading (degrees) */
static uint64_t follow_moved_us;          /* last frame the mouse moved */
static volatile float follow_correction;  /* degrees the direction is short of the wanted heading */
static volatile float follow_push;        /* the right stick while the direction turns (-0.3, 0, 0.3) */
static volatile int follow_has_arc;       /* the follow camera's vertical arc is known */
/* arc_degrees: the camera pitch one unit of arc covers (U1 61; measured in U2 about 135, in U3 about 105, steeper upwards) */
static void follow_camera(uint64_t camera, uint32_t dir_offset, uint32_t arc_offset, float arc_degrees) {
    const uint64_t now=now_us();
    follow_has_arc=arc_offset!=0;
    if (camera_hook_us && now-camera_hook_us>100000) {
        float ignore_x, ignore_y;
        camera_take(0,&ignore_x,&ignore_y);
        follow_moved_us=0;
    }
    camera_hook_us=now;
    float *dir=(float *)(uintptr_t)(camera+dir_offset);
    const float heading=heading_of(dir);
    float dx, dy;
    camera_take(0,&dx,&dy);
    if (aim_held) { follow_moved_us=0; follow_pending=0; return; } /* the aiming camera takes the mouse */
    if (follow_pending) {
        float kept=wrap_degrees(heading-follow_before)/wrap_degrees(follow_after-follow_before);
        kept=kept<0.0f ? 0.0f : kept>1.0f ? 1.0f : kept;
        follow_kept+=0.2f*(kept-follow_kept);
        if (follow_kept<0.12f) follow_direct=0;
        else if (follow_kept>0.3f) follow_direct=1;
        follow_pending=0;
    }
    float shown=heading; /* the shown camera's yaw (debug output) */
    double age=0.0;
    if (mouse_debug() && (!bbgpu_camera_yaw(&shown,&age) || age>0.25)) shown=heading;
    const float turn=camera_test_rate>0.0f ? camera_test_rate : dx*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
    /* the wanted heading and how far the direction is short of it (degrees still to turn). The
     * camera rests 0.15 s after the mouse stopped, once the direction has reached the wanted heading
     * (fast turns go beyond what the game keeps in a frame) or at the latest after 0.5 s. */
    float correction=wrap_degrees(heading-follow_target);
    const uint64_t still=now-follow_moved_us;
    const int resting=!follow_moved_us || (still>150000 && (SDL_fabsf(correction)<0.5f || still>500000));
    if (turn!=0.0f) follow_moved_us=now;
    if (resting) {
        follow_target=heading;
        correction=0.0f;
    }
    if (correction>90.0f || correction<-90.0f) { /* at most a quarter turn behind */
        correction=correction>0.0f ? 90.0f : -90.0f;
        follow_target=wrap_degrees(heading-correction);
    }
    follow_target=wrap_degrees(follow_target-turn); /* a positive turn lowers the heading */
    if (!follow_direct) {
        /* the joypad turns the camera (with the shortfall as extra speed); a small probe turn shows
         * whether direct turns are kept again */
        follow_correction=correction;
        follow_push=0.0f;
        if (turn!=0.0f) {
            const float probe=turn>0.0f ? 0.3f : -0.3f;
            const float a=probe*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
            const float x=dir[0], z=dir[2];
            dir[0]=x*c-z*s;
            dir[2]=x*s+z*c;
            follow_before=heading;
            follow_after=heading_of(dir);
            follow_pending=1;
        }
        if (mouse_debug()) printf("Mouse: follow t=%.4f dx=%.1f heading=%.2f shown=%.2f target=%.2f kept=%.2f joypad\n",
                                  now*1e-6,dx,heading,shown,follow_target,follow_kept);
        return;
    }
    follow_correction=0.0f;
    /* the shortfall is made up a few degrees a frame (no jump when the game keeps turns again) */
    if (correction<-4.0f) correction=-4.0f;
    if (correction>4.0f) correction=4.0f;
    const float total=turn+correction;
    follow_push=total>0.05f ? 0.3f : total<-0.05f ? -0.3f : 0.0f;
    if (mouse_debug()) printf("Mouse: follow t=%.4f dx=%.1f heading=%.2f shown=%.2f target=%.2f turn=%.2f total=%.2f\n",
                              now*1e-6,dx,heading,shown,follow_target,turn,total);
    if (total!=0.0f) {
        const float a=total*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
        const float x=dir[0], z=dir[2];
        dir[0]=x*c-z*s;
        dir[2]=x*s+z*c;
        if (SDL_fabsf(total)>=0.15f) {
            follow_before=heading;
            follow_after=heading_of(dir);
            follow_pending=1;
        }
    }
    if (dy!=0.0f && arc_offset) {
        float *arc=(float *)(uintptr_t)(camera+arc_offset);
        float v=*arc+(input.mouse_invert_y ? -dy : dy)*(MOUSE_DEGREES_PER_PIXEL/arc_degrees)*input.mouse_sensitivity;
        *arc=v<0.0f ? 0.0f : v>1.0f ? 1.0f : v;
    }
}
static void __attribute__((ms_abi)) u1_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14) {
    (void)unused; (void)image; (void)r14;
    follow_camera(camera,0x1310,0x12bc,61.0f);
}
static void __attribute__((ms_abi)) u2_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14) {
    (void)unused; (void)image; (void)r14;
    follow_camera(camera,0x9ec0,0x9e60,135.0f);
}
static void __attribute__((ms_abi)) u3_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14) {
    (void)unused; (void)image; (void)r14;
    follow_camera(camera,0x78c0,0x7874,105.0f);
}
static const unsigned char u1_follow_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x28,0x01,0x00,0x00,
    0x44,0x89,0x8d,0x00,0xff,0xff,0xff,0x49,0x89,0xcd,0x48,0x89,0xd3,0x49,0x89,0xf7,0x49,0x89,0xfc,
    0x4c,0x8b,0x35,0x92,0x7b,0x8d,0x00,0x49,0x8b,
};
static const unsigned char u2_follow_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x48,0x01,0x00,0x00,
    0x44,0x89,0x8d,0x18,0xff,0xff,0xff,0x48,0x89,0x8d,0xc8,0xfe,0xff,0xff,0x49,0x89,0xd5,0x4c,0x89,0xad,
    0x38,0xff,0xff,0xff,0x48,0x89,0xf3,0x49,
};
static const unsigned char u3_follow_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xd8,0x01,0x00,0x00,
    0x44,0x89,0x8d,0xa0,0xfe,0xff,0xff,0x48,0x89,0x8d,0x48,0xfe,0xff,0xff,0x49,0x89,0xd5,0x48,0x89,0xf3,
    0x49,0x89,0xff,0x4c,0x8b,0x65,0x20,0x4c,
};
/* U1: the game's pad update (image+0x117200) fills the caller's joypad state (its first argument):
 * the sticks as floats lx ly rx ry at +0 and again at +0x10, converted from the pad's bytes. Cameras
 * other than the follow camera (aiming: about 45 degrees/s * x|x|, no speed ramp) get the mouse as a
 * right stick value there, not limited to the stick's range. */
static uint64_t joypad_state;         /* the state the pad update fills (its first argument) */
static uint64_t sticks_last_us;
static float aim_test=-1.0f;          /* BB_MOUSE_AIM_TEST=<x>: right stick x while not following */
static float aim_stick(float rate) {
    const float r=rate<0 ? -rate : rate;
    const float x=r<=45.0f ? SDL_sqrtf(r/45.0f) : (r+9.0f)/54.0f;
    return rate<0 ? -x : x;
}
/* The follow camera's response to the right stick value (measured in U1): about 144 x^2 degrees/s up
 * to x = 0.5, then a speed that builds up over a second towards 240 degrees/s from x = 0.7 (and stays
 * there above 0.9): fast turns reach it late. */
static float follow_stick(float rate) {
    const float r=rate<0 ? -rate : rate;
    float x=r<=36.0f ? SDL_sqrtf(r/144.0f) : 0.5f+(r-36.0f)/(240.0f-36.0f)*0.4f;
    if (x>0.9f) x=0.9f;
    return rate<0 ? -x : x;
}
static void __attribute__((ms_abi)) u1_pad_update(uint64_t state, uint64_t unused, uint64_t image, uint64_t r14) {
    (void)unused; (void)r14;
    if (mouse_debug() && joypad_state!=state)
        printf("Mouse: joypad state %#llx (image %#llx, offset %#llx)\n",(unsigned long long)state,
               (unsigned long long)image,(unsigned long long)(state-image));
    joypad_state=state;
}
static void __attribute__((ms_abi)) u1_joypad_sticks(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t r14) {
    (void)unused0; (void)unused1; (void)image;
    if (!joypad_state || r14-0x62!=joypad_state) return; /* pad 0 only */
    const uint64_t now=now_us();
    sticks_hook_us=now;
    float dt=sticks_last_us ? (float)(now-sticks_last_us)*1e-6f : 1.0f/60.0f;
    sticks_last_us=now;
    if (dt<1.0f/480.0f) dt=1.0f/480.0f;
    if (dt>0.05f) dt=0.05f;
    static float follow_test=-1.0f;   /* BB_MOUSE_FOLLOW_TEST=<x>: right stick x everywhere (measuring) */
    if (follow_test<0.0f) { const char *t=getenv("BB_MOUSE_FOLLOW_TEST"); follow_test=t ? (float)atof(t) : 0.0f; }
    if (follow_test>0.0f) { ((float *)(uintptr_t)joypad_state)[2]+=follow_test; ((float *)(uintptr_t)joypad_state)[6]+=follow_test; return; }
    float dx, dy;
    camera_take(1,&dx,&dy);
    if (aim_test<0.0f) { const char *t=getenv("BB_MOUSE_AIM_TEST"); aim_test=t ? (float)atof(t) : 0.0f; }
    float *pad=(float *)(uintptr_t)joypad_state;
    float x=0.0f, y=0.0f;
    if (aim_test>0.0f) x=aim_test;
    else {
        /* the turn rate (degrees/s) the mouse asks for, through the inverse of the aiming curve
         * measured in U1: 45 x^2 degrees/s up to x = 1, then about 54 x - 9 (x = 3: 152, x = 6: 324) */
        const float scale=MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity/dt;
        const int following=camera_hook_us && now-camera_hook_us<50000;
        float push=0.0f;
        if (!aim_held && following && follow_direct) { /* the follow camera turned itself */
            dx=dy=0.0f;
            push=follow_push;
        }
        float (*curve)(float)=aim_held ? aim_stick : follow_stick;
        /* following through the joypad: the shortfall is made up within about a quarter second */
        const float extra=!aim_held && following && !follow_direct ? follow_correction*4.0f : 0.0f;
        x=curve(dx*scale+extra)+push;
        y=curve((input.mouse_invert_y ? -dy : dy)*scale);
    }
    if (mouse_debug()) printf("Mouse: aim t=%.4f dt=%.4f dx=%.1f x=%.3f game=%.3f\n",now*1e-6,dt,dx,x,pad[2]);
    if (x!=0.0f) { pad[2]+=x; pad[6]+=x; }
    if (y!=0.0f) { pad[3]+=y; pad[7]+=y; }
}
static const unsigned char u1_pad_update_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xb8,0x00,0x00,0x00,
    0x49,0x89,0xfe,0x4c,0x8b,0x2d,0x92,0xf6,0x86,0x00,0x49,0x8b,
};
static const unsigned char u1_joypad_sticks_code[]={
    0x80,0x7c,0x01,0x6d,0x00,0x74,0x3b,0xc5,0xe2,0x59,0x64,0x01,0x40,0xc4,0xc1,0x7a,0x11,0x66,0xbe,0xc5,
    0xe2,0x59,0x64,0x01,
};
/* U2 UpdateFocus (player-base.cpp:1101): the camera's look-at spring went NaN once at 240 Hz and
 * the game's assertion ended it. The assertion's call now restarts the spring at the player's
 * target ([rbp-0x60]): position (+0x2b0, y +0x2a4) and velocities (+0x2c0, +0x2a8); a NaN target
 * still stops at the trap. */
static const unsigned char u2_focus_assert_code[]={
    0xe8,0xa8,0xd7,0x1a,0x00,0x48,0x85,0xc0,0x74,0x27,0x48,0x8d,0x3d,0x4c,0x9c,0x83,0x00,0x48,0x8d,0x35,
    0x4b,0x99,0x83,0x00,0x48,0x8d,0x15,0x5b,0x9c,0x83,0x00,0x48,0x8d,0x0d,0x7e,0x99,0x83,0x00,0x41,0xb8,
    0x4d,0x04,0x00,0x00,0x45,0x31,0xc9,0xff,0xd0,0xcd,0x41,0xc5,0xf8,0x28,0x45,0xd0,
};
static const unsigned char u2_focus_reset[]={
    0xc5,0xf8,0x28,0x45,0xa0,                     /* vmovaps xmm0, [rbp-0x60] (the target) */
    0x0f,0x2e,0xc0,0x7a,0x2c,                     /* ucomiss xmm0, xmm0; jp trap */
    0xc5,0xf8,0x29,0x83,0xb0,0x02,0x00,0x00,      /* vmovaps [rbx+0x2b0], xmm0 */
    0xc4,0xe3,0x79,0x17,0x83,0xa4,0x02,0x00,0x00,0x01, /* vextractps [rbx+0x2a4], xmm0, 1 */
    0xc5,0xf8,0x29,0x45,0xd0,                     /* vmovaps [rbp-0x30], xmm0 */
    0xc5,0xf0,0x57,0xc9,                          /* vxorps xmm1, xmm1, xmm1 */
    0xc5,0xf8,0x29,0x8b,0xc0,0x02,0x00,0x00,      /* vmovaps [rbx+0x2c0], xmm1 */
    0x83,0xa3,0xa8,0x02,0x00,0x00,0x00,           /* and dword [rbx+0x2a8], 0 */
    0xeb,0x02,                                    /* jmp (store the focus) */
    0xcd,0x41,                                    /* trap: int 0x41 */
};
_Static_assert(sizeof(u2_focus_reset)==sizeof(u2_focus_assert_code),"same size");
/* U2 (EU 1.00) game/character-leg-ik.cpp:391, assertion IsFinite(deltaTransform): the root delta
 * the leg IK gets from its caller can be NaN for a frame (train wreck, chapter 1: issues #88,
 * #253) and the game stopped. The leg IK only adds its y to two accumulators, so a non-finite
 * delta now counts as no movement: it is zeroed (also in the caller's vector) and the update
 * goes on. */
static const unsigned char u2_leg_ik_assert_code[]={
    0xe8,0xa3,0xcb,0xbf,0xff,0x48,0x85,0xc0,0x74,0x27,0x48,0x8d,0x3d,0x1b,0xfe,0x25,0x00,0x48,0x8d,
    0x35,0x92,0xfc,0x25,0x00,0x48,0x8d,0x15,0x06,0xfe,0x25,0x00,0x48,0x8d,0x0d,0xd1,0xfc,0x25,0x00,
    0x41,0xb8,0x87,0x01,0x00,0x00,0x45,0x31,0xc9,0xff,0xd0,0xcd,0x41,
};
static const unsigned char u2_leg_ik_zero[]={
    0xc5,0xf8,0x57,0xc0,                          /* vxorps xmm0, xmm0, xmm0 */
    0xc4,0xc1,0x78,0x29,0x07,                     /* vmovaps [r15], xmm0 */
    0xeb,0x2d,                                    /* jmp (the update, 0x8ec650) */
    0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,
    0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,
    0xcc,0xcc,
};
_Static_assert(sizeof(u2_leg_ik_zero)==sizeof(u2_leg_ik_assert_code),"same size");
static const RuntimeGuestCallback guest_callbacks[]={
    {"U1 follow camera (mouse)",0xaecf0,u1_follow_camera_code,sizeof(u1_follow_camera_code),6,u1_follow_camera,NULL},
    {"U1 pad update (mouse)",0x117200,u1_pad_update_code,sizeof(u1_pad_update_code),6,u1_pad_update,NULL},
    {"U1 joypad sticks (mouse)",0x11784e,u1_joypad_sticks_code,sizeof(u1_joypad_sticks_code),5,u1_joypad_sticks,NULL},
    {"U2 follow camera (mouse)",0x171fe0,u2_follow_camera_code,sizeof(u2_follow_camera_code),6,u2_follow_camera,NULL},
    {"U3 follow camera (mouse)",0x121b90,u3_follow_camera_code,sizeof(u3_follow_camera_code),6,u3_follow_camera,NULL},
    {"U2 camera focus NaN reset",0x33ba13,u2_focus_assert_code,sizeof(u2_focus_assert_code),
     sizeof(u2_focus_reset),NULL,u2_focus_reset},
    {"U2 leg IK NaN delta",0x8ec618,u2_leg_ik_assert_code,sizeof(u2_leg_ik_assert_code),
     sizeof(u2_leg_ik_zero),NULL,u2_leg_ik_zero},
};
const RuntimeGuestCallback *runtime_guest_callbacks(size_t *count) {
    *count=sizeof(guest_callbacks)/sizeof(*guest_callbacks);
    return guest_callbacks;
}

static void sample_host(PadData *d) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=now_us();
    static int loaded;
    if (!loaded) { load_input(); loaded=1; }
    SDL_Gamepad *g=current_gamepad();
    float mdx=0, mdy=0;
    uint32_t mouse=0;
    int wheel=0, captured=bbgpu_mouse_sample(&mdx,&mdy,&mouse,&wheel);
    if (bbgpu_overlay_captures_input()) return; /* settings menu open: neutral input */
    /* Scripted runs (BB_PAD_FILE) ignore the player's controller and keyboard: a pad in use on
     * the same PC reached the test window (BB_PAD_HOST=1 keeps them). */
    static int scripted=-1;
    if (scripted<0) {
        const char *file=getenv("BB_PAD_FILE"), *host=getenv("BB_PAD_HOST");
        scripted=file && *file && !(host && host[0]=='1');
    }
    if (scripted) return;
    const uint64_t now=d->timestamp;
    if (wheel>0) wheel_until_us[0]=now+80000;
    if (wheel<0) wheel_until_us[1]=now+80000;
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    if (g) {
        for (int a=0;a<16;++a) {
            const int b=input.pad[a];
            if (b>=0 && SDL_GetGamepadButton(g,(SDL_GamepadButton)b)) d->buttons|=actions[a].ps;
            else if (b==PAD_TRIGGER_LEFT || b==PAD_TRIGGER_RIGHT) {
                const uint8_t v=trigger(SDL_GetGamepadAxis(g,b==PAD_TRIGGER_LEFT ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                                                              : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
                if (v>30) d->buttons|=actions[a].ps;
                if (actions[a].ps==BTN_L2 && v>d->l2) d->l2=v;
                if (actions[a].ps==BTN_R2 && v>d->r2) d->r2=v;
            }
        }
        if (SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_TOUCHPAD)) d->buttons|=BTN_TOUCHPAD; /* DualSense/DS4 click */
        if ((d->buttons & BTN_L2) && !d->l2) d->l2=255;
        if ((d->buttons & BTN_R2) && !d->r2) d->r2=255;
        d->left_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX)); d->left_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY));
        d->right_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)); d->right_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY));
        if (input.pad_invert_y) d->right_y=(uint8_t)(255-d->right_y);
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
    }
    /* Keyboard and mouse, together with the controller. */
    for (int a=0;a<16;++a) if (action_down(a,k,mouse,now)) d->buttons|=actions[a].ps;
    if (d->buttons & BTN_L2) d->l2=255;
    if (d->buttons & BTN_R2) d->r2=255;
    // The touchpad button without a finger on a touch surface is a left-side click.
    if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    const int lx=action_down(A_LS_RIGHT,k,mouse,now)-action_down(A_LS_LEFT,k,mouse,now);
    const int ly=action_down(A_LS_DOWN,k,mouse,now)-action_down(A_LS_UP,k,mouse,now);
    if (lx) d->left_x=(uint8_t)(lx<0 ? 0 : 255);
    if (ly) d->left_y=(uint8_t)(ly<0 ? 0 : 255);
    const int rx=action_down(A_RS_RIGHT,k,mouse,now)-action_down(A_RS_LEFT,k,mouse,now);
    const int ry=action_down(A_RS_DOWN,k,mouse,now)-action_down(A_RS_UP,k,mouse,now);
    if (rx) d->right_x=(uint8_t)(rx<0 ? 0 : 255);
    if (ry) d->right_y=(uint8_t)(ry<0 ? 0 : 255);
    /* Where the game's camera or joypad state takes the mouse directly (their callbacks ran in the
     * last 100 ms), the motion goes there. U2 and U3 have no joypad callback: the follow camera turns
     * with the mouse (vertically too where its arc is known), with the right stick held a little the
     * same way (follow_push); aiming, other cameras and a follow camera that keeps no turns get the
     * mouse as a right stick here. */
    pthread_mutex_lock(&camera_lock);
    const int following=camera_hook_us && now-camera_hook_us<100000;
    const int joypad=sticks_hook_us && now-sticks_hook_us<100000;
    for (int q=0;q<2;++q) {
        if (captured && (following || joypad)) { camera_dx[q]+=mdx; camera_dy[q]+=mdy; }
        else { camera_dx[q]=camera_dy[q]=0; }
    }
    pthread_mutex_unlock(&camera_lock);
    const int direct_x=joypad || (following && !aim_held && follow_direct);
    const int direct_y=joypad || (following && !aim_held && follow_direct && follow_has_arc);
    if (captured && direct_x && direct_y) {
        mouse_vx=mouse_vy=0; mouse_last_us=0;
    } else if (captured) {
        float dt=mouse_last_us ? (float)(now-mouse_last_us)*1e-6f : 0.0f;
        mouse_last_us=now;
        if (dt>0.0f) {
            if (dt>0.1f) dt=0.1f;
            /* turn rate in degrees/s, smoothed over 12 ms (mouse reports arrive in bursts) */
            const float alpha=1.0f-SDL_expf(-dt/0.012f), scale=MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
            mouse_vx+=alpha*(mdx/dt*scale-mouse_vx);
            mouse_vy+=alpha*((input.mouse_invert_y ? -mdy : mdy)/dt*scale-mouse_vy);
            if (mdx==0 && mouse_vx>-1.0f && mouse_vx<1.0f) mouse_vx=0;
            if (mdy==0 && mouse_vy>-1.0f && mouse_vy<1.0f) mouse_vy=0;
        }
        if (mouse_vx!=0 && !rx && !direct_x) d->right_x=mouse_axis(mouse_vx);
        if (mouse_vy!=0 && !ry && !direct_y) d->right_y=mouse_axis(mouse_vy);
    } else {
        mouse_vx=mouse_vy=0; mouse_last_us=0;
    }
}

/* BB_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad defaults to a left-side click; lx= ly= rx= ry= (0..255) override
 * the sticks. An empty file releases everything. */
static struct { uint32_t buttons; int stick[4]; int touch_side; float mouse[2]; } injected={0,{-1,-1,-1,-1},-1,{0,0}};
static int replay_armed;      /* 1 while a BB_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) { path=getenv("BB_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef _WIN32
    /* Whole-second mtimes: the size tells two edits within a second apart. */
    if (st.st_mtime==mtime.tv_sec && st.st_size==mtime.tv_nsec) return;
    mtime=(struct timespec){st.st_mtime,(long)st.st_size};
#else
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const struct { const char *name; uint32_t ps; } names[]={
        {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
        {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
        {"options",BTN_OPTIONS}, {"touchpad",BTN_TOUCHPAD},
        {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    };
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    injected.touch_side=-1;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    injected.mouse[0]=injected.mouse[1]=0.0f;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* BB_PAD_REPLAY */
        if (!strcmp(token,"touchpad_left") || !strcmp(token,"touchpad_right")) {
            injected.buttons|=BTN_TOUCHPAD;
            injected.touch_side=!strcmp(token,"touchpad_right");
        }
        for (size_t i=0;i<sizeof(names)/sizeof(*names);++i) if (!strcmp(token,names[i].name)) injected.buttons|=names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
        if (!strncmp(token,"mx=",3)) injected.mouse[0]=(float)atof(token+3); /* mouse motion, pixels/s */
        if (!strncmp(token,"my=",3)) injected.mouse[1]=(float)atof(token+3);
    }
    fclose(f);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d mouse %g %g (t=%.4f)\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3],injected.mouse[0],
           injected.mouse[1],now*1e-6);
}
/* BB_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; BB_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in BB_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("BB_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("BB_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}
/* After the menu or the text dialog closes, buttons still held (the Cross that accepted a
 * name) stay hidden until released: the game would take them as a new press. */
static int hold_after_capture;
static void sample(PadData *d) {
    sample_host(d);
    if (bbgpu_overlay_captures_input()) { hold_after_capture=1; return; }
    record_sample(d);
    read_inject();
    /* scripted mouse motion (mx= my=) goes where the player's mouse would */
    static uint64_t mouse_inject_us;
    const uint64_t inject_now=now_us();
    if ((injected.mouse[0]!=0.0f || injected.mouse[1]!=0.0f) && mouse_inject_us) {
        float dt=(float)(inject_now-mouse_inject_us)*1e-6f;
        if (dt>0.1f) dt=0.1f;
        pthread_mutex_lock(&camera_lock);
        for (int q=0;q<2;++q) { camera_dx[q]+=injected.mouse[0]*dt; camera_dy[q]+=injected.mouse[1]*dt; }
        pthread_mutex_unlock(&camera_lock);
    }
    mouse_inject_us=inject_now;
    replay_sample(d);
    d->buttons|=injected.buttons;
    aim_held=(d->buttons & BTN_L2)!=0;
    if (injected.touch_side>=0) touch_click(d,injected.touch_side);
    else if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    /* U2, U3 (no joypad callback): the right stick held a little while the mouse turns the follow
     * camera's direction (follow_push, see follow_camera), unless the stick is in use */
    const uint64_t now=now_us();
    if (follow_push!=0.0f && camera_hook_us && now-camera_hook_us<100000 &&
        !(sticks_hook_us && now-sticks_hook_us<100000) && !aim_held && d->right_x>=118 && d->right_x<=138)
        d->right_x=(uint8_t)(128.0f+follow_push*127.5f);
    if (hold_after_capture) {
        if (d->buttons) d->buttons=0;
        else hold_after_capture=0;
    }
}

static ABI int32_t pad_init(void) { pthread_mutex_lock(&lock); initialized=1; pthread_mutex_unlock(&lock); return 0; }
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user!=1) return ERR_INVALID_ARG;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    int already=opened; opened=1;
    pthread_mutex_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    puts("Runtime: pad opened for user 1 (SDL gamepad or keyboard)");
    return PAD_HANDLE;
}
static ABI int32_t pad_close(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    opened=0; return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    sample(data); ++reads;
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pad_read_state(handle,data);
    return 1;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=0;
    pthread_mutex_lock(&lock);
    current_gamepad();
    info->connected_count=connected_count ? connected_count : 1;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    SDL_Gamepad *g=current_gamepad();
    if (g) SDL_RumbleGamepad(g,(uint16_t)(param[0]*257),(uint16_t)(param[1]*257),1000);
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Light bar {r, g, b}: shown on controllers that have one (DualShock 4, DualSense). */
static void set_led(uint8_t r, uint8_t g, uint8_t b) {
    pthread_mutex_lock(&lock);
    SDL_Gamepad *pad=current_gamepad();
    if (pad) SDL_SetGamepadLED(pad,r,g,b);
    pthread_mutex_unlock(&lock);
}
static ABI int32_t pad_light_bar(int32_t handle, const uint8_t *rgb) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!rgb) return ERR_INVALID_ARG;
    set_led(rgb[0],rgb[1],rgb[2]);
    return 0;
}
static ABI int32_t pad_reset_light_bar(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    set_led(0,0,255); /* user 1 colour */
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle==PAD_HANDLE && opened ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }

static const RuntimeExport exports[]={
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
    {"scePadSetLightBar",pad_light_bar}, {"scePadResetLightBar",pad_reset_light_bar},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) { printf("Runtime: pad reads=%zu, gamepad=%s\n",reads,gamepad ? SDL_GetGamepadName(gamepad) : "none"); }
