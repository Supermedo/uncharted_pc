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
/* Raw aim: mouse pixels not yet given to the aim direction, and the last
 * time the default aim controller ran (the mouse then turns its direction, not the stick). */
static float aim_dx, aim_dy;
static volatile uint64_t aim_direct_us;
/* When aim was pressed. After a roll the game starts the aiming camera's controller up to
 * a second later: the mouse motion of that time waits for it (1.5 s at most) instead of being lost. */
static volatile uint64_t aim_press_us;
static uint64_t camera_hook_us;       /* last follow camera callback: it turns with the mouse */
static uint64_t cover_hook_us;        /* last cover camera callback: it turns with the mouse too */
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
static volatile int aim_held;             /* L2 is held: the aiming camera (also while it blends in) */
static int raw_aim_armed(uint64_t now) {
    return aim_held && aim_direct_us && (now-aim_direct_us<1000000 || now-aim_press_us<1500000);
}
/* While the game blends (a roll, cover, a cut) it runs two follow cameras, a new one each time:
 * each keeps its own wanted heading and turn check (shared state made them pull against each other
 * and switch the mouse to the joypad), and every camera updated in a frame gets the frame's mouse
 * motion, so they turn together. */
typedef struct {
    uint64_t camera, last_us;
    uint64_t moved_us;  /* last frame the mouse moved */
    float target;       /* wanted heading (degrees) */
    float before, after; /* heading before and after the last turn */
    float shown, shown_v; /* where the game's smoothed camera is, estimated (degrees, degrees/s) */
    uint64_t shown_us;
    int pending;
} FollowState;
static FollowState follow_states[4];
static FollowState *follow_state(uint64_t camera, uint64_t now) {
    FollowState *slot=&follow_states[0];
    for (int i=0;i<4;++i) {
        if (follow_states[i].camera==camera) { slot=&follow_states[i]; slot->last_us=now; return slot; }
        if (follow_states[i].last_us<slot->last_us) slot=&follow_states[i];
    }
    *slot=(FollowState){.camera=camera,.last_us=now};
    return slot;
}
/* Every camera updated in the same frame (within 4 ms) gets the frame's mouse motion: the follow
 * cameras of a blend, and the cover camera blending with them. */
static void frame_motion(uint64_t now, float *dx, float *dy) {
    static uint64_t frame_take_us;
    static float frame_dx, frame_dy;
    if (now-frame_take_us>4000) {
        camera_take(0,&frame_dx,&frame_dy);
        frame_take_us=now;
    }
    *dx=frame_dx; *dy=frame_dy;
}
static volatile float follow_correction;  /* degrees the direction is short of the wanted heading */
static volatile float follow_push;        /* the right stick while the direction turns (-0.3, 0, 0.3) */
static volatile int follow_has_arc;       /* the follow camera's vertical arc is known */
/* arc_degrees: the camera pitch one unit of arc covers (U1 61; measured in U2 about 135, in U3 about 105, steeper upwards) */
static void follow_camera(uint64_t camera, uint32_t dir_offset, uint32_t arc_offset, float arc_degrees, float arc_max) {
    const uint64_t now=now_us();
    follow_has_arc=arc_offset!=0;
    if (camera_hook_us && now-camera_hook_us>100000) {
        float ignore_x, ignore_y;
        camera_take(0,&ignore_x,&ignore_y);
        for (int i=0;i<4;++i) follow_states[i].moved_us=0;
    }
    camera_hook_us=now;
    float *dir=(float *)(uintptr_t)(camera+dir_offset);
    const float heading=heading_of(dir);
    FollowState *st=follow_state(camera,now);
    float dx, dy;
    frame_motion(now,&dx,&dy);
    if (aim_held) { st->moved_us=0; st->pending=0; return; } /* the aiming camera takes the mouse */
    /* The shown camera follows the camera's position with the game's smoothing, a spring (fitted to a
     * U3 recording: critically damped, w about 24/s, plus about 50 ms of render delay). It moves the
     * position in a straight line, so a direction far ahead cut through the orbit and swung the
     * camera up over Drake (looking straight down). The direction stays within 60 degrees of the
     * spring (the camera dips about 4 degrees at most); faster turns than about 700 degrees/s
     * follow a few frames later through the wanted heading. */
    {
        float lag_dt=st->shown_us ? (float)(now-st->shown_us)*1e-6f : 0.0f;
        if (!st->shown_us || lag_dt>0.5f) { st->shown=heading; st->shown_v=0.0f; lag_dt=0.0f; }
        const float w=24.0f;
        const int steps=1+(int)(lag_dt/0.004f);
        for (int i=0;i<steps;++i) {
            const float h=lag_dt/(float)steps;
            st->shown_v+=(w*w*wrap_degrees(heading-st->shown)-2.0f*w*st->shown_v)*h;
            st->shown=wrap_degrees(st->shown+st->shown_v*h);
        }
        st->shown_us=now;
    }
    if (st->pending) {
        float kept=wrap_degrees(heading-st->before)/wrap_degrees(st->after-st->before);
        kept=kept<0.0f ? 0.0f : kept>1.0f ? 1.0f : kept;
        follow_kept+=0.2f*(kept-follow_kept);
        if (follow_kept<0.12f) follow_direct=0;
        else if (follow_kept>0.3f) follow_direct=1;
        st->pending=0;
    }
    float shown=heading; /* the shown camera's yaw (debug output) */
    double age=0.0;
    if (mouse_debug() && (!bbgpu_camera_yaw(&shown,&age) || age>0.25)) shown=heading;
    const float turn=camera_test_rate>0.0f ? camera_test_rate : dx*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
    /* the wanted heading and how far the direction is short of it (degrees still to turn). The
     * camera rests 0.15 s after the mouse stopped, once the direction has reached the wanted heading
     * (fast turns go beyond what the game keeps in a frame) or at the latest after 0.5 s. */
    float correction=wrap_degrees(heading-st->target);
    const uint64_t still=now-st->moved_us;
    const int resting=!st->moved_us || (still>150000 && (SDL_fabsf(correction)<0.5f || still>500000));
    if (turn!=0.0f) st->moved_us=now;
    if (resting) {
        st->target=heading;
        correction=0.0f;
    }
    if (correction>90.0f || correction<-90.0f) { /* at most a quarter turn behind */
        correction=correction>0.0f ? 90.0f : -90.0f;
        st->target=wrap_degrees(heading-correction);
    }
    st->target=wrap_degrees(st->target-turn); /* a positive turn lowers the heading */
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
            st->before=heading;
            st->after=heading_of(dir);
            st->pending=1;
        }
        if (mouse_debug()) printf("Mouse: follow t=%.4f dx=%.1f heading=%.2f shown=%.2f target=%.2f kept=%.2f joypad\n",
                                  now*1e-6,dx,heading,shown,st->target,follow_kept);
        return;
    }
    follow_correction=0.0f;
    /* the shortfall is made up a few degrees a frame (no jump when the game keeps turns again) */
    if (correction<-4.0f) correction=-4.0f;
    if (correction>4.0f) correction=4.0f;
    float total=turn+correction;
    const float lead=wrap_degrees(heading-total-st->shown); /* a positive turn lowers the heading */
    if (lead>60.0f) total+=lead-60.0f;
    else if (lead<-60.0f) total+=lead+60.0f;
    /* the stick is held the way the mouse moves (until 0.1 s after it stops): the stick's own turn
     * is then steady and the correction takes it out evenly. Following the sign of the total flipped
     * the stick every frame on slow moves, and the camera shook by the stick's turn. */
    static float push_sign;
    static uint64_t push_us;
    if (turn!=0.0f) { push_sign=turn>0.0f ? 0.3f : -0.3f; push_us=now; }
    follow_push=push_us && now-push_us<100000 ? push_sign : 0.0f;
    if (mouse_debug()) printf("Mouse: follow t=%.4f dx=%.1f heading=%.2f shown=%.2f target=%.2f turn=%.2f total=%.2f arc=%.4f\n",
                              now*1e-6,dx,heading,shown,st->target,turn,total,
                              arc_offset ? *(const float *)(uintptr_t)(camera+arc_offset) : -1.0f);
    if (total!=0.0f) {
        const float a=total*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
        const float x=dir[0], z=dir[2];
        dir[0]=x*c-z*s;
        dir[2]=x*s+z*c;
        if (SDL_fabsf(total)>=0.15f) {
            st->before=heading;
            st->after=heading_of(dir);
            st->pending=1;
        }
    }
    if (dy!=0.0f && arc_offset) {
        /* The mouse stops below the top of the arc (arc_max): at the top the camera looks straight
         * down and a small sideways move spins the whole view around Drake. BB_MOUSE_ARC_MAX=<0..1>
         * sets it for every game. Where the game itself has the camera higher, the mouse only lowers it. */
        static float arc_env=-2.0f;
        if (arc_env<-1.0f) { const char *e=getenv("BB_MOUSE_ARC_MAX"); arc_env=e ? (float)atof(e) : -1.0f; }
        const float top_limit=arc_env>=0.0f ? arc_env : arc_max;
        float *arc=(float *)(uintptr_t)(camera+arc_offset);
        const float cur=*arc, top=cur>top_limit ? cur : top_limit;
        float v=cur+(input.mouse_invert_y ? -dy : dy)*(MOUSE_DEGREES_PER_PIXEL/arc_degrees)*input.mouse_sensitivity;
        *arc=v<0.0f ? 0.0f : v>top ? top : v;
    }
}
/* The cover camera (CameraControlCover) turns with the right stick in its own update: its
 * horizontal direction (x, 0, z) turns around the vertical, and its pitch (degrees) moves within
 * limits (up to limits[0], down to -limits[1]). The stick's top speed made the mouse slow there;
 * at the start of the update the mouse turns both directly, the same amount as the follow camera. */
static void cover_camera(float *dir, float *pitch, const float *limits) {
    const uint64_t now=now_us();
    if (cover_hook_us && now-cover_hook_us>100000 && !(camera_hook_us && now-camera_hook_us<100000)) {
        float ignore_x, ignore_y;
        camera_take(0,&ignore_x,&ignore_y);
    }
    cover_hook_us=now;
    follow_push=0.0f;
    float dx, dy;
    frame_motion(now,&dx,&dy);
    if (aim_held) return; /* the aiming camera takes the mouse */
    if (mouse_debug()) printf("Mouse: cover t=%.4f dx=%.1f dy=%.1f heading=%.2f pitch=%.2f limits=%.1f..%.1f\n",
                              now*1e-6,dx,dy,heading_of(dir),*pitch,limits ? -limits[1] : 0.0f,limits ? limits[0] : 0.0f);
    const float turn=dx*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
    if (turn!=0.0f) {
        const float a=turn*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
        const float x=dir[0], z=dir[2];
        dir[0]=x*c-z*s;
        dir[2]=x*s+z*c;
    }
    if (dy!=0.0f && limits) {
        float v=*pitch-(input.mouse_invert_y ? -dy : dy)*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
        if (v>limits[0]) v=limits[0];
        if (v<-limits[1]) v=-limits[1];
        *pitch=v;
    }
}
/* Turns an aim direction (x, y, z) by yaw and pitch (degrees), keeping its length; pitch stays
 * within +-70 degrees. Same yaw sense as the follow camera: a positive yaw is a turn right. */
static void aim_turn(float *v, float yaw, float pitch) {
    const float x=v[0], y=v[1], z=v[2];
    const float len=SDL_sqrtf(x*x+y*y+z*z), h=SDL_sqrtf(x*x+z*z);
    if (!(len>0.5f && len<1.5f) || h<1e-4f) return;
    const float a=yaw*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
    const float hx=(x*c-z*s)/h, hz=(x*s+z*c)/h;
    float p=SDL_atan2f(y,h)+pitch*0.017453293f;
    if (p>1.2217f) p=1.2217f;
    if (p<-1.2217f) p=-1.2217f;
    v[0]=hx*SDL_cosf(p)*len; v[1]=SDL_sinf(p)*len; v[2]=hz*SDL_cosf(p)*len;
}
/* Raw aim, all three games (US 1.02). The aim controller (U1 0x276a10, U2 0x71a1b0, U3 0x9285c0;
 * rsi = the controller) gets the aiming camera's remembered aim direction as its 4th argument
 * (rcx): it turns it by the stick, the camera saves it back, and the shown direction adds aim
 * assist and recoil on top. Called from the aiming camera's own call site (return address
 * main_return), the mouse turns that direction directly; other callers (U3 also computes a
 * temporary direction elsewhere) are left alone and take no mouse motion. regs: the hook's saved
 * registers (rbx r11 r10 r9 r8 rdx rcx rax rflags, return address). */
static void raw_aim(const char *game, uint64_t controller, const uint64_t *regs, uint64_t image,
                    uint64_t main_return) {
    static float aim_scale=-1.0f; /* BB_AIM_SCALE: raw aim speed against the follow camera's (1.0) */
    static int aim_off=-1;        /* BB_AIM_DIRECT=0: the old stick aiming, for comparing */
    if (aim_scale<0.0f) { const char *e=getenv("BB_AIM_SCALE"); aim_scale=e ? (float)atof(e) : 1.0f; }
    if (aim_off<0) { const char *e=getenv("BB_AIM_DIRECT"); aim_off=e && e[0]=='0'; }
    const uint64_t ret=regs[9]-image;
    float *dir=(float *)(uintptr_t)regs[6];
    static uint64_t other_logged;
    if (ret!=main_return) {
        if (other_logged!=ret) {
            other_logged=ret;
            printf("Mouse: %s aim controller from another caller (return %#llx), left alone\n",game,
                   (unsigned long long)ret);
        }
        return;
    }
    static int first=1;
    if (first) {
        first=0;
        printf("Mouse: %s raw aim on: controller %#llx, remembered direction %#llx (controller %+lld)\n",
               game,(unsigned long long)controller,(unsigned long long)(uintptr_t)dir,
               (long long)((uintptr_t)dir-controller));
    }
    if (aim_off) return;
    const uint64_t now=now_us();
    pthread_mutex_lock(&camera_lock);
    const float dx=aim_dx, dy=aim_dy;
    aim_dx=aim_dy=0.0f;
    pthread_mutex_unlock(&camera_lock);
    aim_direct_us=now;
    const float k=MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity*aim_scale;
    if (mouse_debug()) printf("Mouse: aim t=%.4f ctl=%#llx yaw=%.3f pitch=%.3f dx=%.1f dy=%.1f\n",now*1e-6,(unsigned long long)controller,
                              SDL_atan2f(-dir[0],-dir[2])*57.29578f,SDL_atan2f(dir[1],SDL_sqrtf(dir[0]*dir[0]+dir[2]*dir[2]))*57.29578f,dx,dy);
    /* While aim is held the game may swap in new aiming cameras (after a roll: a new one about
     * every 100 ms while Drake gets up), which threw away the turns given to the one before. A new
     * controller in the same aim press gets the mouse's turns of this press on top of the game's
     * own direction: copying the last camera's direction kept the first camera of a roll, which
     * still faced the way Drake looked before the roll. On the same controller the game's own
     * changes (aim assist, recoil) stay. */
    /* Two aiming cameras can also take turns within one frame (blending): each controller keeps
     * how much of this press's turns it already has, so each gets every turn exactly once. */
    static uint64_t session_press;
    static float turned_yaw, turned_pitch; /* the mouse's turns in this aim press (degrees) */
    static struct { uint64_t controller, last_us; float yaw, pitch; } given[4];
    if (session_press!=aim_press_us) {
        session_press=aim_press_us;
        turned_yaw=turned_pitch=0.0f;
        memset(given,0,sizeof(given));
    }
    if (!aim_held) return;
    int slot=0;
    for (int i=0;i<4;++i) {
        if (given[i].controller==controller) { slot=i; break; }
        if (given[i].last_us<given[slot].last_us) slot=i;
    }
    /* A new controller that starts where the last one ended (U1 swaps aiming cameras while aiming)
     * copied the turns already; one that starts elsewhere (after a roll: the game's own direction)
     * gets them. */
    static float last_dir[3];
    static int have_last;
    if (given[slot].controller!=controller) {
        given[slot].controller=controller;
        given[slot].yaw=given[slot].pitch=0.0f;
        if (have_last && dir[0]*last_dir[0]+dir[1]*last_dir[1]+dir[2]*last_dir[2]>0.9986f) { /* within 3 degrees */
            given[slot].yaw=turned_yaw; given[slot].pitch=turned_pitch;
        }
    }
    given[slot].last_us=now;
    if (dx!=0.0f || dy!=0.0f) {
        turned_yaw+=dx*k;
        turned_pitch+=-(input.mouse_invert_y ? -dy : dy)*k;
    }
    const float yaw=turned_yaw-given[slot].yaw, pitch=turned_pitch-given[slot].pitch;
    if (yaw!=0.0f || pitch!=0.0f) aim_turn(dir,yaw,pitch);
    given[slot].yaw=turned_yaw; given[slot].pitch=turned_pitch;
    last_dir[0]=dir[0]; last_dir[1]=dir[1]; last_dir[2]=dir[2]; have_last=1;
}
static void __attribute__((ms_abi)) u1_102_aim_controller_c(uint64_t output, uint64_t controller, uint64_t image,
                                                            uint64_t r14, const uint64_t *regs) {
    (void)output; (void)r14;
    /* U1 1.02: called from the aiming camera's update (0xc43b0) at 0xc4c52 */
    raw_aim("U1",controller,regs,image,0xc4c57);
}
static void __attribute__((ms_abi)) u2_102_aim_controller(uint64_t output, uint64_t controller, uint64_t image,
                                                          uint64_t r14, const uint64_t *regs) {
    (void)output; (void)r14;
    /* U2 1.02: the aiming camera calls it at 0x45124b (controller camera+0x160, direction +0x1f30) */
    raw_aim("U2",controller,regs,image,0x451250);
}
static void __attribute__((ms_abi)) u3_102_aim_controller(uint64_t output, uint64_t controller, uint64_t image,
                                                          uint64_t r14, const uint64_t *regs) {
    (void)output; (void)r14;
    /* U3 1.02: the aiming camera calls it at 0x51188f (controller camera+0x1b0, direction +0x11e0);
     * 0x4d09ce computes a temporary direction and is left alone */
    raw_aim("U3",controller,regs,image,0x511894);
}
static void __attribute__((ms_abi)) u1_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14, const uint64_t *regs) {
    (void)regs;
    (void)unused; (void)image; (void)r14;
    follow_camera(camera,0x1310,0x12bc,61.0f,1.0f);
}
static void __attribute__((ms_abi)) u2_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14, const uint64_t *regs) {
    (void)regs;
    (void)unused; (void)image; (void)r14;
    follow_camera(camera,0x9ec0,0x9e60,135.0f,0.75f);
}
static void __attribute__((ms_abi)) u3_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14, const uint64_t *regs) {
    (void)regs;
    (void)unused; (void)image; (void)r14;
    follow_camera(camera,0x78c0,0x7874,105.0f,0.75f);
}
/* U1 and U2: the limits come from the camera's settings (+0x8 up, +0xc down). */
static void cover_camera_settings(uint64_t camera, uint32_t dir_offset, uint32_t pitch_offset,
                                  uint32_t settings_offset) {
    const float *settings=*(const float *const *)(uintptr_t)(camera+settings_offset);
    cover_camera((float *)(uintptr_t)(camera+dir_offset),(float *)(uintptr_t)(camera+pitch_offset),
                 settings ? settings+2 : NULL);
}
static void __attribute__((ms_abi)) u1_102_cover_camera(uint64_t output, uint64_t camera, uint64_t image,
                                                        uint64_t r14, const uint64_t *regs) {
    (void)output; (void)image; (void)r14; (void)regs;
    cover_camera_settings(camera,0x240,0x230,0x1300);
}
static void __attribute__((ms_abi)) u2_102_cover_camera(uint64_t output, uint64_t camera, uint64_t image,
                                                        uint64_t r14, const uint64_t *regs) {
    (void)output; (void)image; (void)r14; (void)regs;
    cover_camera_settings(camera,0x1c0,0x1b0,0x1290);
}
/* U3: the camera is the first argument and holds its limits (+0x218 up, +0x21c down). */
static void __attribute__((ms_abi)) u3_102_cover_camera(uint64_t camera, uint64_t unused, uint64_t image,
                                                        uint64_t r14, const uint64_t *regs) {
    (void)unused; (void)image; (void)r14; (void)regs;
    cover_camera((float *)(uintptr_t)(camera+0x2f0),(float *)(uintptr_t)(camera+0x2e0),
                 (const float *)(uintptr_t)(camera+0x218));
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
/* U1 US 1.02 (CUSA02320) has the same code with other RIP-relative displacements. */
static const unsigned char u1_102_follow_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x28,0x01,0x00,0x00,
    0x44,0x89,0x8d,0x00,0xff,0xff,0xff,0x49,0x89,0xcd,0x48,0x89,0xd3,0x49,0x89,0xf7,0x49,0x89,0xfc,
    0x4c,0x8b,0x35,0x42,0xbd,0x89,0x00,0x49,0x8b,
};
static const unsigned char u1_102_pad_update_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xb8,0x00,0x00,0x00,
    0x49,0x89,0xfe,0x4c,0x8b,0x2d,0xe2,0x37,0x83,0x00,0x49,0x8b,
};
/* U2 and U3 US 1.02 (CUSA02320) follow camera: the same function, recompiled (found by its
 * instruction patterns; it uses the same direction and arc fields on rdi). */
static const unsigned char u2_102_follow_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x28,0x01,0x00,0x00,
    0x45,0x89,0xcc,0x48,0x89,0x8d,0xf8,0xfe,0xff,0xff,0x49,0x89,0xd6,0x49,0x89,0xfd,
};
static const unsigned char u3_102_follow_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xb8,0x01,0x00,0x00,
    0x45,0x89,0xcc,0x48,0x89,0x8d,0x68,0xfe,0xff,0xff,0x49,0x89,0xd6,0x49,0x89,0xfd,
};
static const unsigned char u2_102_aim_controller_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x18,0x01,0x00,0x00,
    0xc5,0xfa,0x11,0x45,0x90,0x44,0x89,0x85,0x60,0xff,
};
static const unsigned char u3_102_aim_controller_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xd8,0x00,0x00,0x00,
    0xc5,0xfa,0x11,0x85,0x40,0xff,0xff,0xff,0x4c,0x89,
};
/* 1.02 cover camera updates (CameraControlCover). U1 (vtable 0x927320) and U2 (vtable 0xfff0d0,
 * slot 28): rsi = the camera; U3 (vtable 0x1670210, slot 40): rdi = the camera. */
static const unsigned char u1_102_cover_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x88,0x01,0x00,0x00,
    0x49,0x89,0xd7,0x48,0x89,0xf3,0x49,0x89,0xfe,
};
static const unsigned char u2_102_cover_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x68,0x01,0x00,0x00,
    0x49,0x89,0xd7,0x48,0x89,0xf3,0x49,0x89,0xfe,
};
static const unsigned char u3_102_cover_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xf8,0x00,0x00,0x00,
    0x49,0x89,0xf6,0x49,0x89,0xff,
};
static const unsigned char u1_102_aim_controller_c_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x08,0x03,0x00,0x00,
    0x45,0x89,0xc4,0x48,0x89,0x8d,0x00,0xfd,0xff,0xff,
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
static void __attribute__((ms_abi)) u1_pad_update(uint64_t state, uint64_t unused, uint64_t image, uint64_t r14, const uint64_t *regs) {
    (void)regs;
    (void)unused; (void)r14;
    if (mouse_debug() && joypad_state!=state)
        printf("Mouse: joypad state %#llx (image %#llx, offset %#llx)\n",(unsigned long long)state,
               (unsigned long long)image,(unsigned long long)(state-image));
    joypad_state=state;
}
static void __attribute__((ms_abi)) u1_joypad_sticks(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t r14, const uint64_t *regs) {
    (void)regs;
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
        if (raw_aim_armed(now)) dx=dy=0.0f; /* raw aim turns it */
        float push=0.0f;
        if (!aim_held && following && follow_direct) { /* the follow camera turned itself */
            dx=dy=0.0f;
            push=follow_push;
        }
        if (!aim_held && cover_hook_us && now-cover_hook_us<50000) dx=dy=0.0f; /* the cover camera too */
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
    {"U1 1.02 follow camera (mouse)",0xaede0,u1_102_follow_camera_code,sizeof(u1_102_follow_camera_code),6,u1_follow_camera,NULL},
    {"U1 1.02 pad update (mouse)",0x117350,u1_102_pad_update_code,sizeof(u1_102_pad_update_code),6,u1_pad_update,NULL},
    {"U1 1.02 joypad sticks (mouse)",0x11799e,u1_joypad_sticks_code,sizeof(u1_joypad_sticks_code),5,u1_joypad_sticks,NULL},
    {"U1 1.02 aim controller (mouse)",0x276a10,u1_102_aim_controller_c_code,sizeof(u1_102_aim_controller_c_code),6,u1_102_aim_controller_c,NULL},
    {"U2 1.02 aim controller (mouse)",0x71a1b0,u2_102_aim_controller_code,sizeof(u2_102_aim_controller_code),6,u2_102_aim_controller,NULL},
    {"U3 1.02 aim controller (mouse)",0x9285c0,u3_102_aim_controller_code,sizeof(u3_102_aim_controller_code),6,u3_102_aim_controller,NULL},
    {"U2 follow camera (mouse)",0x171fe0,u2_follow_camera_code,sizeof(u2_follow_camera_code),6,u2_follow_camera,NULL},
    {"U3 follow camera (mouse)",0x121b90,u3_follow_camera_code,sizeof(u3_follow_camera_code),6,u3_follow_camera,NULL},
    {"U2 1.02 follow camera (mouse)",0x42c690,u2_102_follow_camera_code,sizeof(u2_102_follow_camera_code),6,u2_follow_camera,NULL},
    {"U1 1.02 cover camera (mouse)",0xa4670,u1_102_cover_camera_code,sizeof(u1_102_cover_camera_code),6,u1_102_cover_camera,NULL},
    {"U2 1.02 cover camera (mouse)",0x417450,u2_102_cover_camera_code,sizeof(u2_102_cover_camera_code),6,u2_102_cover_camera,NULL},
    {"U3 1.02 cover camera (mouse)",0x4cd030,u3_102_cover_camera_code,sizeof(u3_102_cover_camera_code),6,u3_102_cover_camera,NULL},
    {"U3 1.02 follow camera (mouse)",0x4e7970,u3_102_follow_camera_code,sizeof(u3_102_follow_camera_code),6,u3_follow_camera,NULL},
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
    const int cover=cover_hook_us && now-cover_hook_us<100000;
    for (int q=0;q<2;++q) {
        if (captured && (following || joypad || cover)) { camera_dx[q]+=mdx; camera_dy[q]+=mdy; }
        else { camera_dx[q]=camera_dy[q]=0; }
    }
    /* kept while aiming: at low frame rates the aim controller runs only every 100-200 ms */
    if (captured && raw_aim_armed(now)) { aim_dx+=mdx; aim_dy+=mdy; }
    else { aim_dx=aim_dy=0.0f; }
    pthread_mutex_unlock(&camera_lock);
    const int raw_aiming=raw_aim_armed(now);
    const int direct_x=joypad || raw_aiming || (cover && !aim_held) || (following && !aim_held && follow_direct);
    const int direct_y=joypad || raw_aiming || (cover && !aim_held) ||
                       (following && !aim_held && follow_direct && follow_has_arc);
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
    {
        const int held=(d->buttons & BTN_L2)!=0;
        if (held && !aim_held) aim_press_us=now_us();
        aim_held=held;
    }
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
