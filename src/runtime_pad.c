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
 *   mouse_look_sensitivity_x=1.0  mouse_look_sensitivity_y=1.0
 *   mouse_aim_sensitivity_x=1.0   mouse_aim_sensitivity_y=1.0
 * Mouse settings are reloaded while running; mouse_sensitivity is the legacy fallback.
 *   pad_vibration=1  pad_vibration_strength=100
 *
 * Defaults (no file): WASD move, mouse look, left button R2 (fire), right button L2 (aim),
 * Space Cross, C Circle, F Square, E Triangle, Q L1, G R1, Left Shift L3, V R3, Escape/Enter
 * Options, Tab touchpad, arrow keys and the wheel the d-pad, IJKL the right stick. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <math.h>
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
/* BIND_KEY: a physical key (SDL scancode, US position). BIND_LAYOUT: a character key the player
 * chose, found in the current keyboard layout (SDL keycode), so "Z" is the key labelled Z on an
 * AZERTY keyboard too. The default letters stay positions: WASD is ZQSD on AZERTY. */
enum { BIND_KEY=1, BIND_MOUSE, BIND_WHEEL, BIND_LAYOUT };
typedef struct { uint8_t kind; int32_t code; } Bind;
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
    int mouse_look, mouse_invert_y, pad_invert_y, pad_vibration, pad_vibration_strength;
    float mouse_sensitivity;
} input;

/* Pad sampling publishes one complete mouse profile. Camera callbacks can run on another
 * guest thread, so they take a snapshot instead of reading fields while the file is reloaded. */
typedef struct {
    float look_x, look_y, aim_x, aim_y;
    int enabled, invert_y;
} MouseSettings;
static pthread_mutex_t mouse_settings_lock=PTHREAD_MUTEX_INITIALIZER;
static MouseSettings mouse_options={1,1,1,1,1,0};
static const char *input_config_path;
static uint64_t mouse_config_poll_us, mouse_config_hash;
static int mouse_config_hash_valid;
static MouseSettings mouse_settings(void) {
    pthread_mutex_lock(&mouse_settings_lock);
    const MouseSettings settings=mouse_options;
    pthread_mutex_unlock(&mouse_settings_lock);
    return settings;
}
static void publish_mouse_settings(const MouseSettings *settings) {
    pthread_mutex_lock(&mouse_settings_lock);
    mouse_options=*settings;
    pthread_mutex_unlock(&mouse_settings_lock);
    input.mouse_look=settings->enabled;
    input.mouse_invert_y=settings->invert_y;
    bbgpu_mouse_configure(settings->enabled);
}
static int mouse_gain(const char *text, float *value) {
    char *end;
    const float number=strtof(text,&end);
    while (*end==' ' || *end=='\t' || *end=='\r' || *end=='\n') ++end;
    if (end==text || *end || !isfinite(number) || number<=0.0f) return 0;
    *value=number<0.1f ? 0.1f : number>10.0f ? 10.0f : number;
    return 1;
}
static int read_mouse_settings(const char *path, MouseSettings *settings, uint64_t *hash) {
    FILE *f=path && *path ? fopen(path,"r") : NULL;
    if (!f) return 0;
    MouseSettings next=*settings;
    float *axes[]={&next.look_x,&next.look_y,&next.aim_x,&next.aim_y};
    const char *keys[]={"mouse_look_sensitivity_x","mouse_look_sensitivity_y",
                        "mouse_aim_sensitivity_x","mouse_aim_sensitivity_y"};
    float legacy=1.0f;
    int have_legacy=0;
    unsigned explicit_axes=0;
    uint64_t fingerprint=14695981039346656037ull;
    char line[256];
    while (fgets(line,sizeof(line),f)) {
        for (const unsigned char *p=(const unsigned char *)line;*p;++p)
            fingerprint=(fingerprint^*p)*1099511628211ull;
        line[strcspn(line,"\r\n")]=0;
        char *eq=strchr(line,'=');
        if (line[0]=='#' || !eq) continue;
        *eq=0;
        const char *value=eq+1;
        if (!strcmp(line,"mouse_sensitivity")) have_legacy=mouse_gain(value,&legacy);
        else if (!strcmp(line,"mouse_look")) {
            if (!strcmp(value,"0") || !strcmp(value,"1")) next.enabled=value[0]=='1';
        } else if (!strcmp(line,"mouse_invert_y")) {
            if (!strcmp(value,"0") || !strcmp(value,"1")) next.invert_y=value[0]=='1';
        } else for (int axis=0;axis<4;++axis) if (!strcmp(line,keys[axis])) {
            /* Even an invalid explicit axis must not be replaced by a legacy value. */
            explicit_axes|=1u<<axis;
            mouse_gain(value,axes[axis]);
        }
    }
    const int failed=ferror(f);
    fclose(f);
    if (failed) return 0;
    if (have_legacy) for (int axis=0;axis<4;++axis)
        if (!(explicit_axes & (1u<<axis))) *axes[axis]=legacy;
    *settings=next;
    *hash=fingerprint;
    return 1;
}
static void reload_mouse_settings(uint64_t now) {
    if (now-mouse_config_poll_us<50000) return;
    mouse_config_poll_us=now;
    MouseSettings settings=mouse_settings();
    uint64_t hash;
    if (!read_mouse_settings(input_config_path,&settings,&hash)) return;
    if (mouse_config_hash_valid && hash==mouse_config_hash) return;
    mouse_config_hash=hash;
    mouse_config_hash_valid=1;
    publish_mouse_settings(&settings);
    if (getenv("BB_MOUSE_DEBUG"))
        printf("Mouse: live sensitivity look=%.2f/%.2f aim=%.2f/%.2f\n",
               settings.look_x,settings.look_y,settings.aim_x,settings.aim_y);
}

/* Whether `name` is one of the keys in `defaults` ("Escape|Return"). */
static int default_key(const char *name, const char *defaults) {
    const size_t length=strlen(name);
    for (const char *d=defaults; d && *d; ) {
        const char *end=strchr(d,'|');
        const size_t n=end ? (size_t)(end-d) : strlen(d);
        if (n==length && !SDL_strncasecmp(d,name,n)) return 1;
        d=end ? end+1 : NULL;
    }
    return 0;
}
static Bind parse_bind(const char *name, const char *defaults) {
    static const struct { const char *name; uint8_t kind; int16_t code; } mouse[]={
        {"Mouse Left",BIND_MOUSE,1}, {"Mouse Right",BIND_MOUSE,2}, {"Mouse Middle",BIND_MOUSE,4},
        {"Mouse 4",BIND_MOUSE,8}, {"Mouse 5",BIND_MOUSE,16}, {"Wheel Up",BIND_WHEEL,1}, {"Wheel Down",BIND_WHEEL,-1},
    };
    for (size_t i=0;i<sizeof(mouse)/sizeof(*mouse);++i)
        if (!SDL_strcasecmp(name,mouse[i].name)) return (Bind){mouse[i].kind,mouse[i].code};
    if (name[0]>' ' && name[0]<127 && !name[1] && !default_key(name,defaults)) {
        const SDL_Keycode key=SDL_GetKeyFromName(name);
        if (key!=SDLK_UNKNOWN) return (Bind){BIND_LAYOUT,(int32_t)key};
    }
    const SDL_Scancode code=SDL_GetScancodeFromName(name);
    return code==SDL_SCANCODE_UNKNOWN ? (Bind){0,0} : (Bind){BIND_KEY,(int32_t)code};
}
static void set_keys(int action, const char *value) {
    char text[128];
    snprintf(text,sizeof(text),"%s",value);
    input.keys[action][0]=input.keys[action][1]=(Bind){0,0};
    char *second=strchr(text,'|');
    if (second) *second++=0;
    if (*text) input.keys[action][0]=parse_bind(text,actions[action].keys);
    if (second && *second) input.keys[action][1]=parse_bind(second,actions[action].keys);
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
    input.mouse_invert_y=input.pad_invert_y=0;
    input.pad_vibration=1; input.pad_vibration_strength=100;
    const char *path=getenv("BB_INPUT_CONFIG");
    input_config_path=path;
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
            else if (!strcmp(key,"pad_vibration")) input.pad_vibration=atoi(value)!=0;
            else if (!strcmp(key,"pad_vibration_strength")) input.pad_vibration_strength=atoi(value);
            else for (int a=0;a<ACTIONS;++a) {
                if (!strncmp(key,"key.",4) && !strcmp(key+4,actions[a].name)) set_keys(a,value);
                else if (a<16 && !strncmp(key,"pad.",4) && !strcmp(key+4,actions[a].name)) input.pad[a]=parse_pad(value);
            }
        }
        fclose(f);
        printf("Runtime: controls from %s (mouse look %s, sensitivity %.2f)\n",path,
               input.mouse_look ? "on" : "off",input.mouse_sensitivity);
    }
    if (!isfinite(input.mouse_sensitivity) || input.mouse_sensitivity<=0.0f) input.mouse_sensitivity=1.0f;
    input.mouse_sensitivity=fminf(10.0f,fmaxf(0.1f,input.mouse_sensitivity));
    if (input.pad_vibration_strength<0) input.pad_vibration_strength=0;
    if (input.pad_vibration_strength>100) input.pad_vibration_strength=100;
    MouseSettings settings={input.mouse_sensitivity,input.mouse_sensitivity,
                            input.mouse_sensitivity,input.mouse_sensitivity,input.mouse_look,input.mouse_invert_y};
    mouse_config_hash_valid=read_mouse_settings(path,&settings,&mouse_config_hash);
    mouse_config_poll_us=now_us();
    publish_mouse_settings(&settings);
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
    case BIND_LAYOUT: {
        const SDL_Scancode code=k ? SDL_GetScancodeFromKey((SDL_Keycode)b->code,NULL) : SDL_SCANCODE_UNKNOWN;
        return code!=SDL_SCANCODE_UNKNOWN && k[code];
    }
    case BIND_MOUSE: return (mouse & (uint32_t)b->code)!=0;
    case BIND_WHEEL: return now<wheel_until_us[b->code>0 ? 0 : 1];
    default: return 0;
    }
}
/* Once the keyboard is up: where the layout put each character key the player chose. */
static void log_layout_keys(void) {
    for (int a=0;a<ACTIONS;++a) for (int i=0;i<2;++i) {
        const Bind *b=&input.keys[a][i];
        if (b->kind!=BIND_LAYOUT) continue;
        const SDL_Scancode code=SDL_GetScancodeFromKey((SDL_Keycode)b->code,NULL);
        printf("Runtime: key.%s %s: the key labelled so in this keyboard layout (US position %s)\n",
               actions[a].name,SDL_GetKeyName((SDL_Keycode)b->code),
               code==SDL_SCANCODE_UNKNOWN ? "none" : SDL_GetScancodeName(code));
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
 * 1.00 and 1.02; U2 and U3 have the same function, in code compiled differently in each build, so the
 * hooks are found by instruction patterns, see guest_callbacks): its horizontal direction (x, y, z) at +0x1310 (U2 +0x9ec0, U3 +0x78c0) and the
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
/* The follow camera's vertical arc (0..1) against the pitch of the camera the game shows (degrees, up is
 * positive), measured with slow mouse sweeps (BB_MOUSE_DEBUG prints the arc, BB_CAMERA_LOG the pitch the game shows): the arc is far from linear (U1: 16 degrees
 * per unit near the top of its range, 55 near the bottom). The mouse moves the pitch, not the arc, so a pixel
 * is the same turn everywhere. Outside the table the end slopes continue. */
typedef struct { int n; float arc[12], pitch[12]; } ArcTable;
static const ArcTable u1_arc={11,{0,.1f,.2f,.3f,.4f,.5f,.6f,.7f,.8f,.9f,1},
    {0.6f,-1.5f,-4.7f,-7.4f,-10.0f,-12.4f,-14.8f,-18.6f,-24.0f,-29.0f,-33.0f}};
static const ArcTable u2_arc={6,{.5f,.6f,.7f,.8f,.9f,1},{-8.0f,-19.4f,-33.7f,-48.3f,-57.2f,-64.5f}};
static const ArcTable u3_arc={6,{.5f,.6f,.7f,.8f,.9f,1},{-9.5f,-20.2f,-34.1f,-47.6f,-59.4f,-65.8f}};
static float arc_to_pitch(const ArcTable *t, float arc) {
    int i=0;
    while (i<t->n-2 && arc>t->arc[i+1]) ++i;
    const float slope=(t->pitch[i+1]-t->pitch[i])/(t->arc[i+1]-t->arc[i]);
    return t->pitch[i]+(arc-t->arc[i])*slope;
}
static float pitch_to_arc(const ArcTable *t, float pitch) {   /* pitch falls as the arc grows */
    int i=0;
    while (i<t->n-2 && pitch<t->pitch[i+1]) ++i;
    const float slope=(t->pitch[i+1]-t->pitch[i])/(t->arc[i+1]-t->arc[i]);
    return t->arc[i]+(pitch-t->pitch[i])/slope;
}
static void follow_camera(uint64_t camera, uint32_t dir_offset, uint32_t arc_offset, const ArcTable *arc_table, float arc_max) {
    const MouseSettings controls=mouse_settings();
    if (!controls.enabled) return;
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
    const float turn=camera_test_rate>0.0f ? camera_test_rate : dx*MOUSE_DEGREES_PER_PIXEL*controls.look_x;
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
        const float pitch=arc_to_pitch(arc_table,cur)-(controls.invert_y ? -dy : dy)*MOUSE_DEGREES_PER_PIXEL*controls.look_y;
        const float v=pitch_to_arc(arc_table,pitch);
        *arc=v<0.0f ? 0.0f : v>top ? top : v;
    }
}
/* The cover camera (CameraControlCover) turns with the right stick in its own update: its
 * horizontal direction (x, 0, z) turns around the vertical, and its pitch (degrees) moves within
 * limits (up to limits[0], down to -limits[1]). The stick's top speed made the mouse slow there;
 * at the start of the update the mouse turns both directly, the same amount as the follow camera. */
static void cover_camera(float *dir, float *pitch, const float *limits) {
    const MouseSettings controls=mouse_settings();
    if (!controls.enabled) return;
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
    const float turn=dx*MOUSE_DEGREES_PER_PIXEL*controls.look_x;
    if (turn!=0.0f) {
        const float a=turn*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
        const float x=dir[0], z=dir[2];
        dir[0]=x*c-z*s;
        dir[2]=x*s+z*c;
    }
    if (dy!=0.0f && limits) {
        float v=*pitch-(controls.invert_y ? -dy : dy)*MOUSE_DEGREES_PER_PIXEL*controls.look_y;
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
/* Raw aim, all three games (US 1.02 and the verified CUSA02343 EU layout). The aim controller (US:
 * U1 0x276a10, U2 0x71a1b0, U3 0x9285c0;
 * rsi = the controller) gets the aiming camera's remembered aim direction as its 4th argument
 * (rcx): it turns it by the stick, the camera saves it back, and the shown direction adds aim
 * assist and recoil on top. Called from the aiming camera's own call site (return address
 * main_return), the mouse turns that direction directly; other callers (U3 also computes a
 * temporary direction elsewhere) are left alone and take no mouse motion. regs: the hook's saved
 * RuntimeGuestRegs; at function entry the return address immediately follows that block. */
/* Ported from Umar-Qayyum-dev/uncharted_pc, commits bcf35c5 and 60432aa (GPL v2 or later). */
static void raw_aim(const char *game, uint64_t controller, const RuntimeGuestRegs *regs, uint64_t image,
                    uint64_t main_return) {
    static float aim_scale=-1.0f; /* BB_AIM_SCALE: raw aim speed against the follow camera's (1.0) */
    static int aim_off=-1;        /* BB_AIM_DIRECT=0: the old stick aiming, for comparing */
    if (aim_scale<0.0f) { const char *e=getenv("BB_AIM_SCALE"); aim_scale=e ? (float)atof(e) : 1.0f; }
    if (aim_off<0) { const char *e=getenv("BB_AIM_DIRECT"); aim_off=e && e[0]=='0'; }
    uint64_t return_address;
    memcpy(&return_address,regs+1,sizeof(return_address));
    const uint64_t ret=return_address-image;
    float *dir=(float *)(uintptr_t)regs->rcx;
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
    const MouseSettings controls=mouse_settings();
    if (!controls.enabled) return;
    const uint64_t now=now_us();
    pthread_mutex_lock(&camera_lock);
    const float dx=aim_dx, dy=aim_dy;
    aim_dx=aim_dy=0.0f;
    pthread_mutex_unlock(&camera_lock);
    aim_direct_us=now;
    const float k=MOUSE_DEGREES_PER_PIXEL*aim_scale;
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
    static float last_dir[3];
    static int have_last;
    static uint64_t session_press;
    static float turned_yaw, turned_pitch; /* the mouse's turns in this aim press (degrees) */
    static struct { uint64_t controller, last_us; float yaw, pitch; } given[4];
    if (session_press!=aim_press_us) {
        session_press=aim_press_us;
        turned_yaw=turned_pitch=0.0f;
        memset(given,0,sizeof(given));
        have_last=0;
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
    if (given[slot].controller!=controller) {
        given[slot].controller=controller;
        given[slot].yaw=given[slot].pitch=0.0f;
        if (have_last && dir[0]*last_dir[0]+dir[1]*last_dir[1]+dir[2]*last_dir[2]>0.9986f) { /* within 3 degrees */
            given[slot].yaw=turned_yaw; given[slot].pitch=turned_pitch;
        }
    }
    given[slot].last_us=now;
    if (dx!=0.0f || dy!=0.0f) {
        turned_yaw+=dx*k*controls.aim_x;
        turned_pitch+=-(controls.invert_y ? -dy : dy)*k*controls.aim_y;
    }
    const float yaw=turned_yaw-given[slot].yaw, pitch=turned_pitch-given[slot].pitch;
    if (yaw!=0.0f || pitch!=0.0f) aim_turn(dir,yaw,pitch);
    given[slot].yaw=turned_yaw; given[slot].pitch=turned_pitch;
    last_dir[0]=dir[0]; last_dir[1]=dir[1]; last_dir[2]=dir[2]; have_last=1;
}
static void __attribute__((ms_abi)) u1_102_aim_controller_c(uint64_t output, uint64_t controller, uint64_t image,
                                                            uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)output; (void)r14;
    /* U1 1.02: called from the aiming camera's update (0xc43b0) at 0xc4c52 */
    raw_aim("U1",controller,regs,image,0xc4c57);
}
static void __attribute__((ms_abi)) u2_102_aim_controller(uint64_t output, uint64_t controller, uint64_t image,
                                                          uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)output; (void)r14;
    /* U2 1.02: the aiming camera calls it at 0x45124b (controller camera+0x160, direction +0x1f30) */
    raw_aim("U2",controller,regs,image,0x451250);
}
static void __attribute__((ms_abi)) u3_102_aim_controller(uint64_t output, uint64_t controller, uint64_t image,
                                                          uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)output; (void)r14;
    /* U3 1.02: the aiming camera calls it at 0x51188f (controller camera+0x1b0, direction +0x11e0);
     * 0x4d09ce computes a temporary direction and is left alone */
    raw_aim("U3",controller,regs,image,0x511894);
}
/* CUSA02343 dump reporting 01.02: controllers and remembered-direction call sites were checked
 * in each executable. The direction is still rcx, and rsi is still the controller. The old
 * follow/joypad code in this layout matches the table's build-100 variants. */
static void __attribute__((ms_abi)) u1_eu_aim_controller(uint64_t output, uint64_t controller, uint64_t image,
                                                       uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)output; (void)r14;
    raw_aim("U1",controller,regs,image,0xc4b67);
}
static void __attribute__((ms_abi)) u2_eu_aim_controller(uint64_t output, uint64_t controller, uint64_t image,
                                                       uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)output; (void)r14;
    raw_aim("U2",controller,regs,image,0x18cc5f);
}
static void __attribute__((ms_abi)) u3_eu_aim_controller(uint64_t output, uint64_t controller, uint64_t image,
                                                       uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)output; (void)r14;
    raw_aim("U3",controller,regs,image,0x13c621);
}
static void __attribute__((ms_abi)) u1_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)regs;
    (void)unused; (void)image; (void)r14;
    follow_camera(camera,0x1310,0x12bc,&u1_arc,1.0f);
}
static void __attribute__((ms_abi)) u2_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)regs;
    (void)unused; (void)image; (void)r14;
    follow_camera(camera,0x9ec0,0x9e60,&u2_arc,0.75f);
}
static void __attribute__((ms_abi)) u3_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)regs;
    (void)unused; (void)image; (void)r14;
    follow_camera(camera,0x78c0,0x7874,&u3_arc,0.75f);
}
/* U1 and U2: the limits come from the camera's settings (+0x8 up, +0xc down). */
static void cover_camera_settings(uint64_t camera, uint32_t dir_offset, uint32_t pitch_offset,
                                  uint32_t settings_offset) {
    const float *settings=*(const float *const *)(uintptr_t)(camera+settings_offset);
    cover_camera((float *)(uintptr_t)(camera+dir_offset),(float *)(uintptr_t)(camera+pitch_offset),
                 settings ? settings+2 : NULL);
}
static void __attribute__((ms_abi)) u1_102_cover_camera(uint64_t output, uint64_t camera, uint64_t image,
                                                        uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)output; (void)image; (void)r14; (void)regs;
    cover_camera_settings(camera,0x240,0x230,0x1300);
}
static void __attribute__((ms_abi)) u2_102_cover_camera(uint64_t output, uint64_t camera, uint64_t image,
                                                        uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)output; (void)image; (void)r14; (void)regs;
    cover_camera_settings(camera,0x1c0,0x1b0,0x1290);
}
/* U3: the camera is the first argument and holds its limits (+0x218 up, +0x21c down). */
static void __attribute__((ms_abi)) u3_102_cover_camera(uint64_t camera, uint64_t unused, uint64_t image,
                                                        uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)unused; (void)image; (void)r14; (void)regs;
    cover_camera((float *)(uintptr_t)(camera+0x2f0),(float *)(uintptr_t)(camera+0x2e0),
                 (const float *)(uintptr_t)(camera+0x218));
}
/* U1: the game's pad update (image+0x117200) fills the caller's joypad state (its first argument):
 * the sticks as floats lx ly rx ry at +0 and again at +0x10, converted from the pad's bytes. Cameras
 * other than the follow camera (aiming: about 45 degrees/s * x|x|, no speed ramp) get the mouse as a
 * right stick value there, not limited to the stick's range. */
static uint64_t joypad_state;         /* the state the pad update fills (its first argument) */
static uint64_t sticks_last_us;
static float aim_test=-1.0f;          /* BB_MOUSE_AIM_TEST=<x>: right stick x while not following */
/* The aiming camera's response to the right stick: degrees/s at stick values 0.1.. (measured in real aiming
 * with a gun, per game and build, using BB_MOUSE_CAL). Version 1.02 turns the stick up to 1 and nothing above is
 * faster (about 46 degrees/s sideways, 30 or 20 up and down); 1.00 keeps getting faster above 1 and is slower
 * in the middle. U2 and U3 turn alike in 1.02; U2 1.00 is not measured (its dev menu task jump fails) and uses
 * the U3 1.00 numbers. The mouse asks for a rate; the stick that gives it is looked up here. */
typedef struct { int n; float x[12], rate[12]; } StickTable;
typedef struct { StickTable h, v; } AimCurve;
static const AimCurve u1_100_aim={
    {12,{.1f,.2f,.3f,.4f,.5f,.6f,.7f,.8f,.9f,1,1.5f,3},{0,1.1f,3.1f,6.0f,10.1f,15.3f,21.6f,29.3f,38.3f,47.8f,74.1f,157}},
    {12,{.1f,.2f,.3f,.4f,.5f,.6f,.7f,.8f,.9f,1,1.5f,3},{0,.5f,1.4f,2.5f,4.3f,6.4f,9.1f,12.3f,16.1f,19.8f,31,39.6f}}};
static const AimCurve u1_102_aim={
    {10,{.1f,.2f,.3f,.4f,.5f,.6f,.7f,.8f,.9f,1},{0,3.3f,7.1f,11.4f,16.2f,21.5f,27.0f,33.0f,39.1f,45.0f}},
    {10,{.1f,.2f,.3f,.4f,.5f,.6f,.7f,.8f,.9f,1},{0,1.5f,3.1f,5.1f,7.1f,9.5f,11.9f,14.6f,17.1f,19.9f}}};
static const AimCurve u23_100_aim={
    {12,{.1f,.2f,.3f,.4f,.5f,.6f,.7f,.8f,.9f,1,1.5f,3},{0,1.1f,3.0f,5.9f,9.8f,14.8f,21.0f,28.3f,37.0f,46.3f,72.1f,153.9f}},
    {12,{.1f,.2f,.3f,.4f,.5f,.6f,.7f,.8f,.9f,1,1.5f,3},{0,.65f,1.85f,3.65f,6.15f,9.4f,13.4f,18.1f,23.6f,29.5f,46.5f,62.6f}}};
static const AimCurve u23_102_aim={
    {10,{.1f,.2f,.3f,.4f,.5f,.6f,.7f,.8f,.9f,1},{0,3.4f,6.8f,11.2f,16.3f,21.5f,26.9f,33.1f,40.5f,46.0f}},
    {10,{.1f,.2f,.3f,.4f,.5f,.6f,.7f,.8f,.9f,1},{0,2.1f,4.5f,7.4f,10.5f,14.0f,17.7f,21.6f,25.8f,30.0f}}};
static float stick_for_rate(const StickTable *t, float rate) {
    const float r=rate<0 ? -rate : rate;
    float x=t->x[t->n-1];
    if (r<t->rate[t->n-1]) {
        int i=0;
        while (i<t->n-2 && r>t->rate[i+1]) ++i;
        x=t->x[i]+(r-t->rate[i])/(t->rate[i+1]-t->rate[i])*(t->x[i+1]-t->x[i]);
    }
    if (r<=0.0f) x=0.0f;
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
static void __attribute__((ms_abi)) u1_pad_update(uint64_t state, uint64_t unused, uint64_t image, uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)regs;
    (void)unused; (void)r14;
    if (mouse_debug() && joypad_state!=state)
        printf("Mouse: joypad state %#llx (image %#llx, offset %#llx)\n",(unsigned long long)state,
               (unsigned long long)image,(unsigned long long)(state-image));
    joypad_state=state;
}
/* The mouse as a right stick value in a float joypad state (lx ly rx ry at +0 and again at +0x10),
 * called once a frame by the hook of the game's pad update for pad 0. */
static void mouse_sticks(float *pad, const AimCurve *aim) {
    const uint64_t now=now_us();
    sticks_hook_us=now;
    const MouseSettings controls=mouse_settings();
    if (!controls.enabled) return;
    float dt=sticks_last_us ? (float)(now-sticks_last_us)*1e-6f : 1.0f/60.0f;
    sticks_last_us=now;
    if (dt<1.0f/480.0f) dt=1.0f/480.0f;
    if (dt>0.05f) dt=0.05f;
    static float follow_test=-1.0f;   /* BB_MOUSE_FOLLOW_TEST=<x>: right stick x everywhere (measuring) */
    if (follow_test<0.0f) { const char *t=getenv("BB_MOUSE_FOLLOW_TEST"); follow_test=t ? (float)atof(t) : 0.0f; }
    if (follow_test>0.0f) { pad[2]+=follow_test; pad[6]+=follow_test; return; }
    float dx, dy;
    camera_take(1,&dx,&dy);
    if (aim_test<0.0f) { const char *t=getenv("BB_MOUSE_AIM_TEST"); aim_test=t ? (float)atof(t) : 0.0f; }
    float x=0.0f, y=0.0f;
    static int cal=-1;                /* BB_MOUSE_CAL=<pixels/s per stick unit>: the mouse is a raw stick value (measuring) */
    static float cal_scale;
    if (cal<0) { const char *t=getenv("BB_MOUSE_CAL"); cal_scale=t ? (float)atof(t) : 0.0f; cal=cal_scale>0.0f; }
    if (cal) { x=dx/dt/cal_scale; y=dy/dt/cal_scale; }
    else if (aim_test>0.0f) x=aim_test;
    else {
        /* the turn rate (degrees/s) the mouse asks for, through the inverse of the aiming curve
         * measured in U1: 45 x^2 degrees/s up to x = 1, then about 54 x - 9 (x = 3: 152, x = 6: 324) */
        const float scale=MOUSE_DEGREES_PER_PIXEL/dt;
        const int following=camera_hook_us && now-camera_hook_us<50000;
        if (raw_aim_armed(now)) dx=dy=0.0f; /* the aim controller already takes the mouse */
        const int covering=!aim_held && cover_hook_us && now-cover_hook_us<50000;
        if (covering) dx=dy=0.0f;
        float push=0.0f;
        if (!aim_held && !covering && following && follow_direct) { /* the follow camera turned itself */
            dx=dy=0.0f;
            push=follow_push;
        }
        /* following through the joypad: the shortfall is made up within about a quarter second */
        const float extra=!aim_held && !covering && following && !follow_direct ? follow_correction*4.0f : 0.0f;
        const float rx=dx*scale*(aim_held ? controls.aim_x : controls.look_x);
        const float ry=(controls.invert_y ? -dy : dy)*scale*(aim_held ? controls.aim_y : controls.look_y);
        if (aim_held) {
            x=stick_for_rate(&aim->h,rx);
            y=stick_for_rate(&aim->v,ry);
        } else {
            x=follow_stick(rx+extra)+push;
            y=follow_stick(ry);
        }
    }
    if (mouse_debug()) printf("Mouse: aim t=%.4f dt=%.4f dx=%.1f x=%.3f game=%.3f\n",now*1e-6,dt,dx,x,pad[2]);
    if (x!=0.0f) { pad[2]+=x; pad[6]+=x; }
    if (y!=0.0f) { pad[3]+=y; pad[7]+=y; }
}
static void __attribute__((ms_abi)) u1_joypad_sticks(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)regs; (void)unused0; (void)unused1; (void)image;
    if (!joypad_state || r14-0x62!=joypad_state) return; /* pad 0 only */
    mouse_sticks((float *)(uintptr_t)joypad_state,&u1_100_aim);
}
static void __attribute__((ms_abi)) u1_joypad_sticks_102(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)regs; (void)unused0; (void)unused1; (void)image;
    if (!joypad_state || r14-0x62!=joypad_state) return; /* pad 0 only */
    mouse_sticks((float *)(uintptr_t)joypad_state,&u1_102_aim);
}
/* U2 and U3 fill the same float state (an array of pads in U3: pad 0 first, a pad every 0x64 bytes), in
 * code of their own; the hooks sit after the stick values are stored, in each build's registers. */
static void __attribute__((ms_abi)) u2_sticks_r14(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)rdi; (void)rsi; (void)image; (void)regs;
    mouse_sticks((float *)(uintptr_t)r14,&u23_100_aim);
}
static void __attribute__((ms_abi)) u2_sticks_r12(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)rdi; (void)rsi; (void)image; (void)r14;
    mouse_sticks((float *)(uintptr_t)regs->r12,&u23_102_aim);
}
static void __attribute__((ms_abi)) u3_sticks_r13(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)rdi; (void)rsi; (void)image; (void)r14;
    mouse_sticks((float *)(uintptr_t)regs->r13,&u23_100_aim);
}
static void __attribute__((ms_abi)) u3_sticks_pair_r13(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)rdi; (void)rsi; (void)image; (void)r14;
    if (regs->rcx==0x64) mouse_sticks((float *)(uintptr_t)regs->r13,&u23_100_aim); /* the pad at r13 + rcx - 0x64 */
}
static void __attribute__((ms_abi)) u3_sticks_r12_rdx(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)rdi; (void)rsi; (void)image; (void)r14;
    if (regs->rdx==0) mouse_sticks((float *)(uintptr_t)regs->r12,&u23_102_aim); /* the pad at r12 + rdx */
}
static void __attribute__((ms_abi)) u3_sticks_pair_r12(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14, const RuntimeGuestRegs *regs) {
    (void)rdi; (void)rsi; (void)image; (void)r14;
    if (regs->rdx==(uint64_t)-0x64) mouse_sticks((float *)(uintptr_t)regs->r12,&u23_102_aim); /* the pad at r12 + rdx + 0x64 */
}
/* U2 UpdateFocus (player-base.cpp:1101): the camera's look-at spring went NaN once at 240 Hz and
 * the game's assertion ended it. The assertion's call now restarts the spring at the player's
 * target ([rbp-0x60]): position (+0x2b0, y +0x2a4) and velocities (+0x2c0, +0x2a8); a NaN target
 * still stops at the trap. */
#define U2_FOCUS_RESET \
    0xc5,0xf8,0x28,0x45,0xa0,                            /* vmovaps xmm0, [rbp-0x60] (the target) */ \
    0x0f,0x2e,0xc0,0x7a,0x2c,                            /* ucomiss xmm0, xmm0; jp trap */ \
    0xc5,0xf8,0x29,0x83,0xb0,0x02,0x00,0x00,             /* vmovaps [rbx+0x2b0], xmm0 */ \
    0xc4,0xe3,0x79,0x17,0x83,0xa4,0x02,0x00,0x00,0x01,   /* vextractps [rbx+0x2a4], xmm0, 1 */ \
    0xc5,0xf8,0x29,0x45,0xd0,                            /* vmovaps [rbp-0x30], xmm0 */ \
    0xc5,0xf0,0x57,0xc9,                                 /* vxorps xmm1, xmm1, xmm1 */ \
    0xc5,0xf8,0x29,0x8b,0xc0,0x02,0x00,0x00,             /* vmovaps [rbx+0x2c0], xmm1 */ \
    0x83,0xa3,0xa8,0x02,0x00,0x00,0x00,                  /* and dword [rbx+0x2a8], 0 */ \
    0xeb,0x02,                                           /* jmp (store the focus) */ \
    0xcd,0x41                                            /* trap: int 0x41 */
static const unsigned char u2_focus_reset_100[]={U2_FOCUS_RESET};              /* replaces 56 bytes */
static const unsigned char u2_focus_reset_102[]={0x66,0x90,U2_FOCUS_RESET};    /* 58 bytes: a nop first */
_Static_assert(sizeof(u2_focus_reset_100)==56 && sizeof(u2_focus_reset_102)==58,"patch sizes");
/* U2 game/character-leg-ik.cpp:391, assertion IsFinite(deltaTransform): the root delta the leg IK
 * gets from its caller can be NaN for a frame (train wreck, chapter 1: issues #88, #253) and the
 * game stopped. The leg IK only adds its y to two accumulators, so a non-finite delta now counts as
 * no movement: it is zeroed (also in the caller's vector at [r15]) and the update goes on. Build 100
 * (US and EU 1.00) calls the assertion handler (5 bytes), build 102 loads it (7 bytes). */
#define U2_LEG_IK_ZERO(jump)     0xc5,0xf8,0x57,0xc0,                          /* vxorps xmm0, xmm0, xmm0 */     0xc4,0xc1,0x78,0x29,0x07,                     /* vmovaps [r15], xmm0 */     0xeb,jump                                     /* jmp (the update) */
#define CC8 0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc
static const unsigned char u2_leg_ik_zero_100[]={U2_LEG_IK_ZERO(0x2d),CC8,CC8,CC8,CC8,CC8};
static const unsigned char u2_leg_ik_zero_102[]={U2_LEG_IK_ZERO(0x2f),CC8,CC8,CC8,CC8,CC8,0xcc,0xcc};
/* U1 and U2 "pAction != NULL" (moveactioncontroller.cpp:40), from Mevlock's dev fork: the
 * player's move actions come from a pool of 64 slots. Two move states swapping every frame
 * (landing into a fight, often right after a jump) keep one action each swap; at 144 FPS bursts
 * took 16-30 slots, and a longer one filled the pool and stopped the game. The slot count passed
 * where the character is set up becomes 256 (+144 KB from the character's heap). One site in U1
 * and one in U2, in every build; U3 has no such pool. */
static const unsigned char move_pool_256[]={0xb9,0x00,0x01,0x00,0x00}; /* mov ecx, 256 */
_Static_assert(sizeof(u2_leg_ik_zero_100)==51 && sizeof(u2_leg_ik_zero_102)==53,"patch sizes");
/* U3 Player::UpdateFocus (player.cpp:1897, assertion m_lookAt.X() == m_lookAt.X(): the crash
 * climbing the fort in chapter 8). The point Nate looks at ([player+0xef0], velocity +0xf00)
 * springs towards a target; once NaN it stays NaN, and the function's own reset (taken when the
 * point is far off) never runs for it: NaN compares false. The assertion's path now takes that
 * reset when the target is a number (the velocity register zeroed first, as on the reset's usual
 * way in); a NaN target still stops the game. BilalReloaded's fix for 1.02 (his Uncharted port,
 * GPL v2, GitHub #13 there), and the same for 1.00 (reset at 0x390779, target at rbp-0xa0, zero in
 * xmm4). */
static const unsigned char u3_focus_reset_100[]={
    0xc5,0xd8,0x57,0xe4,                          /* vxorps xmm4, xmm4, xmm4 */
    0xc5,0xf8,0x28,0x85,0x60,0xff,0xff,0xff,      /* vmovaps xmm0, [rbp-0xa0] (the target) */
    0xc5,0xf8,0xc2,0xc8,0x00,                     /* vcmpeqps xmm1, xmm0, xmm0 */
    0xc5,0xf8,0x50,0xc1,                          /* vmovmskps eax, xmm1 */
    0x83,0xf8,0x0f,                               /* cmp eax, 15 */
    0x0f,0x84,0xa3,0xfe,0xff,0xff,                /* je the reset (0x390779) */
    0xcd,0x41,                                    /* the target is NaN too: trap as before */
    CC8,CC8,0xcc,0xcc,0xcc,
};
static const unsigned char u3_focus_reset_102[]={
    0xc5,0xd0,0x57,0xed,0xc5,0xf8,0x28,0x45,0x80,0xc5,0xf8,0xc2,0xc8,0x00,0xc5,0xf8,0x50,0xc1,
    0x83,0xf8,0x0f,0x0f,0x84,0xa5,0xfe,0xff,0xff,0xcd,0x41,CC8,CC8,CC8,
};
_Static_assert(sizeof(u3_focus_reset_100)==51 && sizeof(u3_focus_reset_102)==53,"patch sizes");
/* Render command contexts (ndgi), from Mevlock's dev fork: Begin and End keep a per-thread flag,
 * set while a job worker has a context open (runtime_ndgi_open, cleared too when a fiber leaves
 * the thread, runtime_fiber.c), used by the job wait patch in probe.c. A context records its
 * worker at Begin (+0xe810) and asserts the same one at End ("m_workerThreadIndex ==
 * GetCurrentWorkerThreadIndex()"). One site each in all three games. */
extern _Thread_local int runtime_ndgi_open; /* runtime_fiber.c */
static void __attribute__((ms_abi)) ndgi_begin(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                               const RuntimeGuestRegs *regs) {
    (void)rdi; (void)rsi; (void)image; (void)r14; (void)regs;
    runtime_ndgi_open=1;
}
static void __attribute__((ms_abi)) ndgi_end(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                             const RuntimeGuestRegs *regs) {
    (void)rdi; (void)rsi; (void)image; (void)r14; (void)regs;
    runtime_ndgi_open=0;
}
/* The camera code, the pad code and the assertion are found by their instructions, so a build of the
 * game that has them works (version 1.00 and 1.02 are known; "build" below). RIP-relative
 * displacements and call targets are wildcards. Entries with one name are variants for the builds. */
/* U2 Chapter Select weapons (issue #16, 2026-11-06): the menu's Chapter Select plays the chapter's
 * task group (PlayTask, mode 1). PlayTask first resets the player's settings to the default set
 * (459d0e4e: only the .45 Defender, 40 rounds) and then applies the weapon set of the task it played
 * or its parents; a group has none. The chapter's first checkpoint applies its own set when it
 * activates only if its tasks.bin flags carry 0x20, and the first checkpoints of chapters 8 and 13
 * (war-temple-1-inside-temple, valley-start) do not: Nate starts with the Defender, whose model
 * those levels never load, so it is invisible and silent (the overlay's checkpoint jump plays the
 * checkpoint itself and gets its Beretta, rifle and grenade). Here, after PlayTask(mode 1) of a task
 * without data (a group), the first playable task that activates gets its set as flag 0x20 would
 * give it (when the set has a gun); once. Continue (mode 2), checkpoint restarts and jumps to a
 * checkpoint do not arm it. Chapters it changes: 6-10, 13, 14, 18, 20, 23, 24, 26.
 * BB_CHAPTER_WEAPONS=0: off. */
typedef void (__attribute__((sysv_abi)) *U2ApplySettings)(uint32_t set);
static _Atomic int u2_chapter_start; /* armed by a group's PlayTask(mode 1) */
static int u2_chapter_weapons_on(void) {
    static int on=-1;
    if (on<0) { const char *e=getenv("BB_CHAPTER_WEAPONS"); on=!(e && e[0]=='0'); }
    return on;
}
static void u2_chapter_play(uint64_t task, uint64_t mode) {
    if (!u2_chapter_weapons_on() || task<0x10000) return;
    const int group=*(const uint64_t *)(uintptr_t)(task+0x50)==0;
    atomic_store(&u2_chapter_start,(uint32_t)mode==1 && group);
}
static const unsigned char u2_apply_settings_eu[]={0x55,0x48,0x89,0xe5,0x31,0xf6,0xe8,0x25,0x2c,0xfd,0xff,0x48,0x85,
                                                   0xc0,0x74,0x12,0x48,0x8d,0x3d,0xf1,0xd9,0xb9,0x00,0xb9};
static const unsigned char u2_apply_settings_us[]={0x55,0x48,0x89,0xe5,0xe8,0x97,0x4e,0xfb,0xff,0x48,0x85,0xc0,0x74,
                                                   0x17,0x48,0x8b,0x30,0x48,0x85,0xf6,0x74,0x0f,0x48,0x8d};
static void u2_chapter_activate(uint64_t task, uint64_t image, uint64_t apply, const unsigned char *code, size_t size) {
    if (!atomic_load(&u2_chapter_start) || task<0x10000) return;
    if (memcmp((const void *)(uintptr_t)(image+apply),code,size)) {
        atomic_store(&u2_chapter_start,0);
        printf("Chapter weapons: the settings function is not where expected; nothing applied\n");
        return;
    }
    const unsigned char *t=(const unsigned char *)(uintptr_t)task;
    const unsigned char *data=*(const unsigned char *const *)(t+0x50);
    if (!data || !(*(const uint32_t *)(data+0xd0)&6)) return; /* not playable (a group): wait */
    atomic_store(&u2_chapter_start,0);
    const uint32_t flags=*(const uint32_t *)(t+0x48), set=*(const uint32_t *)(data+0xc8);
    if (flags&0x20 || !set) return; /* the game applies it itself */
    /* Only a set with a gun: some first checkpoints name the empty set (65759d0e, the unarmed openings'),
     * where the Defender is better than nothing (chapter 12, war-escape-watertower-fight). The set: five
     * slots {name, weapon id +8, ammo +0x10} of 0x18 bytes; guns in the first three, f114b39d = none.
     * The lookup is the one the settings function calls (EU: (id, 0) -> set; 1.02: (id) -> &set). */
    const unsigned char *found=NULL;
    if (apply==0x43b7e0) {
        found=((const unsigned char *(__attribute__((sysv_abi)) *)(uint32_t, int))(uintptr_t)(image+0x40e410))(set,0);
    } else {
        const unsigned char *const *p=
            ((const unsigned char *const *(__attribute__((sysv_abi)) *)(uint32_t))(uintptr_t)(image+0xa15300))(set);
        found=p ? *p : NULL;
    }
    int gun=0;
    for (int i=0;found && i<3;++i) {
        const uint32_t weapon=*(const uint32_t *)(found+8+0x18*i);
        if (weapon && weapon!=0xf114b39du) gun=1;
    }
    if (!gun) {
        printf("Chapter weapons: Chapter Select start at task %08x: its set %08x has no gun; the game's "
               "default kept\n",*(const uint32_t *)(t+8),set);
        return;
    }
    printf("Chapter weapons: Chapter Select start at task %08x: its weapon set %08x applied (the game "
           "leaves the default .45 Defender there)\n",*(const uint32_t *)(t+8),set);
    fflush(stdout);
    ((U2ApplySettings)(uintptr_t)(image+apply))(set);
}
static void __attribute__((ms_abi)) u2_chapter_play_hook(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                       const RuntimeGuestRegs *regs) {
    (void)image; (void)r14; (void)regs;
    u2_chapter_play(rdi,rsi);
}
static void __attribute__((ms_abi)) u2_chapter_activate_eu(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                           const RuntimeGuestRegs *regs) {
    (void)rsi; (void)r14; (void)regs;
    u2_chapter_activate(rdi,image,0x43b7e0,u2_apply_settings_eu,sizeof(u2_apply_settings_eu));
}
static void __attribute__((ms_abi)) u2_chapter_activate_us(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                           const RuntimeGuestRegs *regs) {
    (void)rsi; (void)r14; (void)regs;
    u2_chapter_activate(rdi,image,0xa60460,u2_apply_settings_us,sizeof(u2_apply_settings_us));
}
static const RuntimeGuestCallback guest_callbacks[]={
    /* EU layout checked against CUSA02343: unique patterns plus verified image offsets. The cover
     * cameras use the same fields and argument registers as US 1.02, so share those callbacks. */
    {"U1 EU aim controller (mouse)",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 08 03 00 00 45 89 c4 48 89 8d 00 fd ff ff "
      "49 89 d5 49 89 f7 48 89 bd 10 fd ff ff 4c 8b 35 be 03 71 00 49 8b 06 48",
      0,6,u1_eu_aim_controller,NULL,0x2764c0},
    {"U2 EU aim controller (mouse)",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 28 03 00 00 c5 fa 11 85 f0 fc ff ff 4d 89 ce "
      "44 89 85 e0 fc ff ff 49 89 cc 49 89 d5 48 89 f3 48 89 bd 00 fd ff ff",
      0,6,u2_eu_aim_controller,NULL,0x334220},
    {"U3 EU aim controller (mouse)",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 28 03 00 00 c5 fa 11 85 00 fd ff ff 4d 89 cd "
      "44 89 85 e0 fc ff ff 48 89 8d 10 fd ff ff 49 89 d7 49 89 f4 48 89 bd f0 fc ff ff",
      0,6,u3_eu_aim_controller,NULL,0x317740},
    {"U1 EU cover camera (mouse)",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 88 01 00 00 49 89 d7 48 89 f3 49 89 fe "
      "4c 8b 25 fc 22 8e 00 49 8b 04 24 48 89 45 d0 80 bb f4 01",
      0,6,u1_102_cover_camera,NULL,0xa4590},
    {"U2 EU cover camera (mouse)",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec a8 01 00 00 49 89 d7 49 89 f5 49 89 fe "
      "4c 8b 25 94 04 d0 00 49 8b 04 24 48 89 45 d0 41 8a 9d 74",
      0,6,u2_102_cover_camera,NULL,0x161070},
    {"U3 EU cover camera (mouse)",
      "55 48 89 e5 41 57 41 56 41 54 53 48 81 ec 90 00 00 00 49 89 f6 49 89 ff 4c 8d 25 49 a1 2e 01 "
      "49 8b 04 24 48 89 45 d8 41 8b bf e4 00 00 00 e8 4d",
      0,6,u3_102_cover_camera,NULL,0x10e6d0},
    /* Raw aim and cover cameras from Umar-Qayyum-dev's US 1.02 mouse branch. */
    {"U1 1.02 aim controller (mouse)",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 08 03 00 00 45 89 c4 48 89 8d 00 fd ff ff",
      0, 6, u1_102_aim_controller_c, NULL,0x276a10},
    {"U2 1.02 aim controller (mouse)",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 18 01 00 00 c5 fa 11 45 90 44 89 85 60 ff",
      0, 6, u2_102_aim_controller, NULL,0x71a1b0},
    {"U3 1.02 aim controller (mouse)",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec d8 00 00 00 c5 fa 11 85 40 ff ff ff 4c 89",
      0, 6, u3_102_aim_controller, NULL,0x9285c0},
    {"U1 1.02 cover camera (mouse)",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 88 01 00 00 49 89 d7 48 89 f3 49 89 fe",
      0, 6, u1_102_cover_camera, NULL,0xa4670},
    {"U2 1.02 cover camera (mouse)",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 68 01 00 00 49 89 d7 48 89 f3 49 89 fe",
      0, 6, u2_102_cover_camera, NULL,0x417450},
    {"U3 1.02 cover camera (mouse)",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec f8 00 00 00 49 89 f6 49 89 ff",
      0, 6, u3_102_cover_camera, NULL,0x4cd030},
    /* U1 follow camera, build 100 */
    {"U1 follow camera (mouse)",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 28 01 00 00 "
      "44 89 8d 00 ff ff ff 49 89 cd 48 89 d3 49 89 f7 49 89 fc 4c "
      "8b 35 ?? ?? ?? ??",
      0x0, 6, u1_follow_camera, NULL,0},
    /* U1 pad update, build 100 */
    {"U1 pad update (mouse)",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec b8 00 00 00 "
      "49 89 fe 4c 8b 2d ?? ?? ?? ?? 49 8b 45 00 48 89 45 d0 4c 8d "
      "3d ?? ?? ?? ?? 41 8b 7f 70 48 8d 75 b0 e8 ?? ?? ?? ?? 89 c1 "
      "85 c9 74 15 48 8d 3d ?? ?? ?? ?? 31 c0 89 ce e8 ?? ?? ?? ?? "
      "e9 ?? ?? ?? ?? 44 8b 65 b4 41 c6 47 6e 00 80 7d bc 00 0f 84 "
      "?? ?? ?? ?? 41 8b 7f 70 48 8d b5 38 ff ff ff ba 01 00 00 00 "
      "e8 ?? ?? ?? ?? 89 c1 85 c9 0f 88 ?? ?? ?? ?? 41 c6 47 6e 01 "
      "41 c6 47 6c 01 41 c6 47 6d 01 85 c9 0f 84 ?? ?? ?? ?? 45 8b "
      "6f 60 41 8a 5f 5d 48 8b 85 38 ff ff ff 41 89 47 60 41 c6 47 "
      "5d 00 89 c1 c1 e9 1f 48 8d 15 ?? ?? ?? ?? 88 0a 48 89 c1",
      0x0, 6, u1_pad_update, NULL,0},
    /* U1 joypad sticks. The aiming response differs between 1.00 and 1.02, and the code is the same: the first
     * two entries tell the builds apart by the displacement of a constant (unmasked), the last one is any other
     * build (aimed like 1.02). Entries are tried in order, a site is hooked once. */
    {"U1 joypad sticks (mouse)",
      "c5 f9 6f 05 f0 f1 68 00 c5 f8 28 0d ?? ?? ?? ?? c5 f8 28 15 "
      "?? ?? ?? ?? 48 ba 00 00 00 00 00 00 80 bf c5 fa 10 1d ?? ?? "
      "?? ?? 66 66 66 66 66 2e 0f 1f 84 00 00 00 00 00 80 7c 01 6c "
      "00 0f 84 ?? ?? ?? ?? c4 e2 79 31 64 01 50 c5 d9 db e0 c5 f8 "
      "5b e4 c5 d8 59 e1 c5 d8 58 e2 c4 c1 78 11 66 9e c4 c1 78 11 "
      "66 ae 80 7c 01 6d 00 74 3b c5 e2 59 64 01 40 c4 c1 7a 11 66 "
      "be",
      102, 5, u1_joypad_sticks, NULL,0},
    {"U1 joypad sticks (mouse)",
      "c5 f9 6f 05 70 68 68 00 c5 f8 28 0d ?? ?? ?? ?? c5 f8 28 15 "
      "?? ?? ?? ?? 48 ba 00 00 00 00 00 00 80 bf c5 fa 10 1d ?? ?? "
      "?? ?? 66 66 66 66 66 2e 0f 1f 84 00 00 00 00 00 80 7c 01 6c "
      "00 0f 84 ?? ?? ?? ?? c4 e2 79 31 64 01 50 c5 d9 db e0 c5 f8 "
      "5b e4 c5 d8 59 e1 c5 d8 58 e2 c4 c1 78 11 66 9e c4 c1 78 11 "
      "66 ae 80 7c 01 6d 00 74 3b c5 e2 59 64 01 40 c4 c1 7a 11 66 "
      "be",
      102, 5, u1_joypad_sticks_102, NULL,0},
    {"U1 joypad sticks (mouse)",
      "80 7c 01 6d 00 74 3b c5 e2 59 64 01 40 c4 c1 7a 11 66 be",
      0x0, 5, u1_joypad_sticks_102, NULL,0},
    /* U2 follow camera, build 100 */
    {"U2 follow camera (mouse)",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 48 01 00 00 "
      "44 89 8d 18 ff ff ff 48 89 8d c8 fe ff ff 49 89 d5 4c 89 ad "
      "38 ff ff ff 48 89 f3",
      0x0, 6, u2_follow_camera, NULL,0},
    /* U2 follow camera, build 102 */
    {"U2 follow camera (mouse)",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 28 01 00 00 "
      "45 89 cc 48 89 8d f8 fe ff ff 49 89 d6 49 89 fd 48 8b 55 20 "
      "4c 8b 7d 18 48 8b 45 28",
      0x0, 6, u2_follow_camera, NULL,0},
    /* U3 follow camera, build 100 */
    {"U3 follow camera (mouse)",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec d8 01 00 00 "
      "44 89 8d a0 fe ff ff 48 89 8d 48 fe ff ff 49 89 d5 48 89 f3 "
      "49 89 ff 4c 8b 65 20",
      0x0, 6, u3_follow_camera, NULL,0},
    /* U3 follow camera, build 102 */
    {"U3 follow camera (mouse)",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec b8 01 00 00 "
      "45 89 cc 48 89 8d 68 fe ff ff 49 89 d6 49 89 fd 48 8b 55 20 "
      "48 8b 45 28",
      0x0, 6, u3_follow_camera, NULL,0},
    /* U2 joypad sticks, build 100 */
    {"U2 joypad sticks (mouse)",
      "c4 e2 79 31 40 50 c5 f9 db 05 ?? ?? ?? ?? c5 f8 5b c0 c5 f8 "
      "5e 05 ?? ?? ?? ?? c5 f8 58 05 ?? ?? ?? ?? c4 c1 78 11 06 c4 "
      "c1 78 11 46 10 80 78 6d 00 74 28 c5 fa 10 40 40 c4 c1 7a 11 "
      "46 20 c5 fa 10 40 44 c4 c1 7a 11 46 24 c5 fa 10 40 48 c4 c1 "
      "7a 11 46 28 c5 fa 10 40 4c eb 1a 48 b9 00 00 00 00 00 00 80 "
      "bf 49 89 4e 20 41 c7 46 28 00 00 00 00 c5 f8 57 c0 c4 c1 7a "
      "11 46 2c",
      0x75, 6, u2_sticks_r14, NULL,0},
    /* U2 joypad sticks, build 102 */
    {"U2 joypad sticks (mouse)",
      "c4 c1 7a 11 74 24 08 c5 d2 59 db c4 c1 7a 11 5c 24 0c c4 c1 "
      "7a 11 74 24 18 c4 c1 7a 11 5c 24 1c 80 7c 08 6d 00 74 36 c5 "
      "fa 10 5c 08 40 c4 c1 7a 11 5c 24 20",
      0x20, 5, u2_sticks_r12, NULL,0},
    /* U2 joypad sticks, build 102: the copy for one pad (the code above is the copy for several pads, which
     * the game does not use) */
    {"U2 joypad sticks (mouse)",
      "c4 c1 7a 11 2c 24 c5 e2 59 de c4 c1 7a 11 5c 24 04 c4 c1 7a "
      "11 6c 24 10 c4 c1 7a 11 5c 24 14 80 7c 08 6d 00 74 36 c5 fa "
      "10 5c 08 40 c4 c1 7a 11 5c 24 20",
      0x1f, 5, u2_sticks_r12, NULL,0},
    /* U3 joypad sticks, build 100 */
    {"U3 joypad sticks (mouse)",
      "c4 e2 79 31 40 50 c5 f9 db 05 ?? ?? ?? ?? c5 f8 5b c0 c5 f8 "
      "5e 05 ?? ?? ?? ?? c5 f8 58 05 ?? ?? ?? ?? c4 c1 78 11 45 00 "
      "c4 c1 78 11 45 10 80 78 75 00 74 28 c5 fa 10 40 40 c4 c1 7a "
      "11 45 20 c5 fa 10 40 44 c4 c1 7a 11 45 24 c5 fa 10 40 48 c4 "
      "c1 7a 11 45 28 c5 fa 10 40 4c eb 1a 48 b9 00 00 00 00 00 00 "
      "80 bf 49 89 4d 20 41 c7 45 28 00 00 00 00 c5 f8 57 c0 c4 c1 "
      "7a 11 45 2c",
      0x76, 6, u3_sticks_r13, NULL,0},
    /* U3 joypad sticks, build 102 */
    {"U3 joypad sticks (mouse)",
      "80 79 55 00 74 33 c5 fa 10 41 20 c4 c1 7a 11 44 14 20 c5 fa "
      "10 41 24 c4 c1 7a 11 44 14 24 c5 fa 10 41 28 c4 c1 7a 11 44 "
      "14 28 c5 fa 10 71 2c eb 1a 0f 1f 84 00 00 00 00 00 4d 89 44 "
      "14 20 41 c7 44 14 28 00 00 00 00 c5 c8 57 f6 c4 c1 7a 11 74 "
      "14 2c",
      0x4b, 7, u3_sticks_r12_rdx, NULL,0},
    /* U3 joypad sticks, two pads, build 100 */
    {"U3 joypad sticks, two pads (mouse)",
      "c4 e2 79 31 58 30 c5 e1 db d8 c5 f8 5b db c5 e0 5e d9 c5 e0 "
      "58 da c4 c1 78 11 5c 0d 9c c4 c1 78 11 5c 0d ac 80 78 55 00 "
      "74 2c c5 fa 10 58 20 c4 c1 7a 11 5c 0d bc c5 fa 10 58 24 c4 "
      "c1 7a 11 5c 0d c0 c5 fa 10 58 28 c4 c1 7a 11 5c 0d c4 c5 fa "
      "10 58 2c eb 13 90 49 89 54 0d bc 41 c7 44 0d c4 00 00 00 00 "
      "c5 e0 57 db c4 c1 7a 11 5c 0d c8",
      0x68, 7, u3_sticks_pair_r13, NULL,0},
    /* U3 joypad sticks, two pads, build 102 */
    {"U3 joypad sticks, two pads (mouse)",
      "80 79 55 00 74 3e c5 fa 10 41 20 c4 c1 7a 11 84 14 84 00 00 "
      "00 c5 fa 10 41 24 c4 c1 7a 11 84 14 88 00 00 00 c5 fa 10 41 "
      "28 c4 c1 7a 11 84 14 8c 00 00 00 c5 fa 10 71 2c eb 22 66 2e "
      "0f 1f 84 00 00 00 00 00 4d 89 84 14 84 00 00 00 41 c7 84 14 "
      "8c 00 00 00 00 00 00 00 c5 c8 57 f6 c4 c1 7a 11 b4 14 90 00 "
      "00 00",
      0x5c, 10, u3_sticks_pair_r12, NULL,0},
    /* U2 leg IK NaN delta, build 100 */
    {"U2 leg IK NaN delta",
      "e8 ?? ?? ?? ?? 48 85 c0 74 27 48 8d 3d ?? ?? ?? ?? 48 8d 35 "
      "?? ?? ?? ?? 48 8d 15 ?? ?? ?? ?? 48 8d 0d ?? ?? ?? ?? 41 b8 "
      "87 01 00 00 45 31 c9 ff d0 cd 41 c4 c1 78 28 07 c4 e3 79 04 c0 01",
      0x0, 51, NULL, u2_leg_ik_zero_100,0},
    /* U2 leg IK NaN delta, build 102 */
    {"U2 leg IK NaN delta",
      "48 8b 05 ?? ?? ?? ?? 48 85 c0 74 27 48 8d 3d ?? ?? ?? ?? 48 "
      "8d 35 ?? ?? ?? ?? 48 8d 15 ?? ?? ?? ?? 48 8d 0d ?? ?? ?? ?? "
      "41 b8 87 01 00 00 45 31 c9 ff d0 cd 41 c4 c1 78 28 07 c4 e3 79 04 c0 01",
      0x0, 53, NULL, u2_leg_ik_zero_102,0},
    /* U1 and U2 move action pool: mov edx,6; mov ecx,64 (slots); mov r8d,32 */
    {"U1/U2 move action pool 256 slots",
      "ba 06 00 00 00 b9 40 00 00 00 41 b8 20 00 00 00",
      0x5, 5, NULL, move_pool_256,0},
    /* U3 player focus NaN reset, build 100 */
    {"U3 player focus NaN reset",
      "c4 e3 79 04 cb 00 c5 f0 c2 c9 00 c5 f8 50 c1 83 f8 0f 74 38 "
      "e8 ?? ?? ?? ?? 48 85 c0 74 27 48 8d 3d ?? ?? ?? ?? 48 8d 35 "
      "?? ?? ?? ?? 48 8d 15 ?? ?? ?? ?? 48 8d 0d ?? ?? ?? ?? 41 b8 "
      "69 07 00 00 45 31 c9 ff d0 cd 41 c5 f8 28 45 c0",
      0x14, 51, NULL, u3_focus_reset_100,0},
    /* U3 player focus NaN reset, build 102 (BilalReloaded) */
    {"U3 player focus NaN reset",
      "c4 e3 79 04 cb 00 c5 f0 c2 c9 00 c5 f8 50 c1 83 f8 0f 74 3a "
      "48 8b 05 ?? ?? ?? ?? 48 85 c0 74 27 48 8d 3d ?? ?? ?? ?? 48 "
      "8d 35 ?? ?? ?? ?? 48 8d 15 ?? ?? ?? ?? 48 8d 0d ?? ?? ?? ?? "
      "41 b8 6a 07 00 00 45 31 c9 ff d0 cd 41",
      0x14, 53, NULL, u3_focus_reset_102,0},
    /* U2 camera focus NaN reset, build 100 */
    {"U2 camera focus NaN reset",
      "e8 ?? ?? ?? ?? 48 85 c0 74 27 48 8d 3d ?? ?? ?? ?? 48 8d 35 "
      "?? ?? ?? ?? 48 8d 15 ?? ?? ?? ?? 48 8d 0d ?? ?? ?? ?? 41 b8 "
      "4d 04 00 00 45 31 c9 ff d0 cd 41 c5 f8 28 45 d0",
      0x0, 56, NULL, u2_focus_reset_100,0},
    /* U2 camera focus NaN reset, build 102 */
    {"U2 camera focus NaN reset",
      "48 8b 05 ?? ?? ?? ?? 48 85 c0 74 27 48 8d 3d ?? ?? ?? ?? 48 "
      "8d 35 ?? ?? ?? ?? 48 8d 15 ?? ?? ?? ?? 48 8d 0d ?? ?? ?? ?? "
      "41 b8 4d 04 00 00 45 31 c9 ff d0 cd 41 c5 f8 28 45 d0",
      0x0, 58, NULL, u2_focus_reset_102,0},
    /* Render context Begin, build 100: mov [r12+0xe810], rax */
    {"Job render context begin", "49 89 84 24 10 e8 00 00", 0x0, 8, ndgi_begin, NULL,0},
    /* build 102: mov [r15+0xe810], r12 */
    {"Job render context begin", "4d 89 a7 10 e8 00 00", 0x0, 7, ndgi_begin, NULL,0},
    /* Render context End, build 100: mov rbx,[r14+0xe810]; call GetCurrentWorkerThreadIndex;
     * cmp rbx,rax; je */
    {"Job render context end", "49 8b 9e 10 e8 00 00 e8 ?? ?? ?? ?? 48 39 c3 74", 0x0, 7, ndgi_end, NULL,0},
    /* build 102: cmp byte [r15+0xe7dc],0; je; mov r12,[r15+0xe810] (the index call is inlined) */
    {"Job render context end", "41 80 bf dc e7 00 00 00 0f 84 ?? ?? ?? ?? 4d 8b a7 10 e8 00 00",
      14, 7, ndgi_end, NULL,0},

    /* U2 Chapter Select weapons (issue #16, Mevlock): PlayTask and TaskActivate by their first 40
     * bytes (EU 1.00, 1.02); the settings function is checked before it is called. */
    {"U2 Chapter Select weapons: PlayTask",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 e4 e0 ff ff ff 48 81 ec 60 01 00 00 89 f3 49 89 fd 4c 8b 3d 11 c0 a2 00 49",
      0x0, 6, u2_chapter_play_hook, NULL},
    {"U2 Chapter Select weapons: TaskActivate",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 e4 e0 ff ff ff 48 81 ec a0 02 00 00 49 89 fd 48 8b 05 73 81 a2 00 48 8b 00",
      0x0, 6, u2_chapter_activate_eu, NULL},
    {"U2 Chapter Select weapons: PlayTask",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 e4 e0 ff ff ff 48 81 ec 80 01 00 00 41 89 f6 48 89 fb 4c 8b 3d 18 2e 5f 00",
      0x0, 6, u2_chapter_play_hook, NULL},
    {"U2 Chapter Select weapons: TaskActivate",
      "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 e4 e0 ff ff ff 48 81 ec a0 02 00 00 48 89 fb 48 8b 05 bb e8 5e 00 48 8b 00",
      0x0, 6, u2_chapter_activate_us, NULL},
};
const RuntimeGuestCallback *runtime_guest_callbacks(size_t *count) {
    *count=sizeof(guest_callbacks)/sizeof(*guest_callbacks);
    return guest_callbacks;
}

/* The button pictures for this controller: 1 PlayStation (DualShock, DualSense: the game's own),
 * 2 Xbox (everything else; the buttons are named by position, so a Switch pad is shown as the
 * Xbox layout). */
static int prompts_pad_device(SDL_Gamepad *g) {
    const SDL_GamepadType type=SDL_GetGamepadType(g);
    if (type==SDL_GAMEPAD_TYPE_PS3 || type==SDL_GAMEPAD_TYPE_PS4 || type==SDL_GAMEPAD_TYPE_PS5) return 1;
    return SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_SOUTH)==SDL_GAMEPAD_BUTTON_LABEL_CROSS ? 1 : 2;
}

static void sample_host(PadData *d) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=now_us();
    static int loaded;
    if (!loaded) { load_input(); loaded=1; }
    else reload_mouse_settings(d->timestamp);
    const MouseSettings controls=mouse_settings();
    SDL_Gamepad *g=current_gamepad();
    float mdx=0, mdy=0;
    uint32_t mouse=0;
    int wheel=0, captured=bbgpu_mouse_sample(&mdx,&mdy,&mouse,&wheel);
    captured=captured && controls.enabled;
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
    static int layout_logged;
    if (k && !layout_logged) { layout_logged=1; log_layout_keys(); }
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
    /* The device used last picks the button prompts: the game's PlayStation buttons or pictures
     * of the bound keys (bbgpu_prompts_controller). A stick counts past half way, the mouse when
     * it moves more than a few pixels. */
    const int pad_used=g && (d->buttons || d->l2>60 || d->r2>60 || abs(d->left_x-128)>64 ||
                             abs(d->left_y-128)>64 || abs(d->right_x-128)>64 || abs(d->right_y-128)>64);
    int keys_used=mouse || wheel || (captured && SDL_fabsf(mdx)+SDL_fabsf(mdy)>3.0f);
    for (int a=0;a<ACTIONS && !keys_used;++a) keys_used=action_down(a,k,mouse,now);
    static int prompts_device=-1;
    if (pad_used!=keys_used) {
        const int device=pad_used ? prompts_pad_device(g) : 0;
        if (device!=prompts_device) {
            prompts_device=device;
            bbgpu_prompts_controller(device);
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
     * last 100 ms), the motion goes there: the follow camera turns with the mouse (vertically too where
     * its arc is known), with the right stick held a little the same way (follow_push). Raw aiming
     * and cover cameras take their own motion directly. Other cameras, unsupported aim builds and
     * a follow camera that keeps no turns get the mouse as a right stick value in the joypad state.
     * Where a build's hooks were not found, the mouse is a right stick byte here. */
    pthread_mutex_lock(&camera_lock);
    const int following=camera_hook_us && now-camera_hook_us<100000;
    const int joypad=sticks_hook_us && now-sticks_hook_us<100000;
    const int cover=cover_hook_us && now-cover_hook_us<100000;
    for (int q=0;q<2;++q) {
        if (captured && (following || joypad || cover)) { camera_dx[q]+=mdx; camera_dy[q]+=mdy; }
        else { camera_dx[q]=camera_dy[q]=0; }
    }
    /* The aim controller can run only every 100-200 ms at low frame rates. */
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
            const float alpha=1.0f-SDL_expf(-dt/0.012f), scale=MOUSE_DEGREES_PER_PIXEL;
            const float sx=aim_held ? controls.aim_x : controls.look_x;
            const float sy=aim_held ? controls.aim_y : controls.look_y;
            mouse_vx+=alpha*(mdx/dt*scale*sx-mouse_vx);
            mouse_vy+=alpha*((controls.invert_y ? -mdy : mdy)/dt*scale*sy-mouse_vy);
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
        if (raw_aim_armed(inject_now)) { aim_dx+=injected.mouse[0]*dt; aim_dy+=injected.mouse[1]*dt; }
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
    /* Builds without a joypad callback: the right stick held a little while the mouse turns the follow
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
    bbgpu_input_sample(); /* low latency mode: a new frame starts here and may wait (outside the lock) */
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
    SDL_Gamepad *g=input.pad_vibration ? current_gamepad() : NULL;
    if (g) {
        const unsigned strength=(unsigned)input.pad_vibration_strength;
        const uint16_t low=(uint16_t)((param[0]*257u*strength+50u)/100u);
        const uint16_t high=(uint16_t)((param[1]*257u*strength+50u)/100u);
        SDL_RumbleGamepad(g,low,high,1000);
    }
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

/* A loader command used by the launcher before the game starts: no Vulkan or guest memory. */
int runtime_pad_tool(int test, int strength) {
    const int already_ready=SDL_WasInit(SDL_INIT_GAMEPAD)!=0;
    if (!already_ready && !SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        fprintf(stderr,"Controller detection failed: %s\n",SDL_GetError());
        return 1;
    }
    int count=0;
    SDL_JoystickID *ids=SDL_GetGamepads(&count);
    SDL_Gamepad *pad=ids && count>0 ? SDL_OpenGamepad(ids[0]) : NULL;
    SDL_free(ids);
    if (!pad) {
        puts("connected=0\nrumble=0");
        if (!already_ready) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return 0;
    }
    const char *name=SDL_GetGamepadName(pad);
    const bool rumble=SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad),
                                             SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN,false);
    puts("connected=1");
    printf("name=%s\n",name ? name : "Game controller");
    printf("rumble=%d\n",rumble ? 1 : 0);
    if (test) {
        if (strength<0) strength=0;
        if (strength>100) strength=100;
        const uint16_t amount=(uint16_t)((65535u*(unsigned)strength+50u)/100u);
        const bool started=rumble && SDL_RumbleGamepad(pad,amount,amount,500);
        if (started) SDL_Delay(500);
        SDL_RumbleGamepad(pad,0,0,0);
        printf("tested=%d\n",started ? 1 : 0);
    }
    SDL_CloseGamepad(pad);
    if (!already_ready) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    return 0;
}
