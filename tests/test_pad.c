#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "../src/runtime_pad.c"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

static int capture;
_Thread_local int runtime_ndgi_open;
static int rumble_calls;
static uint16_t rumble_low, rumble_high;
int bbgpu_overlay_captures_input(void) { return capture; }
void bbgpu_mouse_configure(int look) { (void)look; }
int bbgpu_mouse_sample(float *dx, float *dy, uint32_t *buttons, int *wheel) {
    *dx=*dy=0; *buttons=0; *wheel=0;
    return 0;
}
int bbgpu_camera_yaw(float *yaw_degrees, double *age_seconds) { (void)yaw_degrees; (void)age_seconds; return 0; }
void bbgpu_prompts_controller(int device) { (void)device; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static bool SDLCALL rumble(void *userdata, Uint16 low, Uint16 high) {
    (void)userdata;
    ++rumble_calls;
    rumble_low=low;
    rumble_high=high;
    return true;
}

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    assert(_putenv_s(name,value)==0);
#else
    assert(setenv(name,value,1)==0);
#endif
}

static void inject(const char *path, const char *tokens) {
    FILE *f=fopen(path,"w");
    assert(f);
    fputs(tokens,f);
    fclose(f);
    usleep(25000);
}

static void test_u3_intro_skip(void) {
    assert(u3_intro_logo("/app0/shareddata/movies/ps4/sce-ndi-logos.bsf"));
    assert(u3_intro_logo("bp-logos.bsf"));
    assert(!u3_intro_logo("story-intro.bsf"));
    assert(!u3_intro_logo("sce-ndi-logos.bsf-extra"));
    char unterminated[256]; memset(unterminated,'x',sizeof(unterminated));
    assert(!u3_intro_logo(unterminated));
    const size_t size=0xe00000;
#ifdef _WIN32
    unsigned char *image=VirtualAlloc(NULL,size,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
#else
    unsigned char *image=mmap(NULL,size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(image!=MAP_FAILED);
#endif
    assert(image);
    const unsigned char code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x50,0x48,0x89,0xfb,
        0xc6,0x83,0x7a,0x25,0x02,0,1,0x89,0x73,0x10,0x58,0x5b,0x41,0x5e,0x41,0x5f,0x5d,0xc3};
    memcpy(image+0xdf37a0,code,sizeof(code));
    unsigned char *player=calloc(1,0x22580); assert(player);
    uintptr_t vtable[4]={0,0,0,(uintptr_t)(image+0xdf37a0)};
    *(const uintptr_t **)player=vtable;
    *(int *)(player+0x2256c)=5;
    strcpy((char *)player+0x22228,"story-intro.bsf");
    set_env("BB_SKIP_INTRO","1");
    u3_movie_update((uintptr_t)player,0,(uintptr_t)image,0,NULL);
    assert(!player[0x2257a]);
    strcpy((char *)player+0x22228,"sce-ndi-logos.bsf");
    set_env("BB_SKIP_INTRO","0");
    u3_movie_update((uintptr_t)player,0,(uintptr_t)image,0,NULL);
    assert(!player[0x2257a]);
    set_env("BB_SKIP_INTRO","1");
    *(int *)(player+0x2256c)=4;
    u3_movie_update((uintptr_t)player,0,(uintptr_t)image,0,NULL);
    assert(!player[0x2257a]);
    *(int *)(player+0x2256c)=5;
    u3_movie_update((uintptr_t)player,0,(uintptr_t)image,0,NULL);
    assert(player[0x2257a] && *(int *)(player+0x10)==1);
    player[0x2257a]=0; image[0xdf37a0]=0x90;
    u3_movie_update((uintptr_t)player,0,(uintptr_t)image,0,NULL);
    assert(!player[0x2257a]);
    free(player);
#ifdef _WIN32
    assert(VirtualFree(image,0,MEM_RELEASE));
#else
    assert(munmap(image,size)==0);
#endif
}


int main(void) {
    test_u3_intro_skip();
    char path[]="bbport-pad-test-XXXXXX";
    int fd=mkstemp(path);
    assert(fd>=0);
    close(fd);
    char config[]="bbport-input-test-XXXXXX";
    fd=mkstemp(config);
    assert(fd>=0);
    close(fd);
    inject(config,"pad_vibration=0\npad_vibration_strength=50\n");
    set_env("BB_PAD_FILE",path);
    set_env("BB_PAD_HOST","1");
    set_env("BB_INPUT_CONFIG",config);
    set_env("SDL_VIDEODRIVER","dummy");
    /* Only the virtual test controller is a gamepad, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);
    PadData data;
    inject(path,"cross l3 touchpad_left");
    assert(pad_read_state(1,&data)==0);
    assert(input.pad_vibration==0);
    assert(input.pad_vibration_strength==50);
    assert((data.buttons & (BTN_CROSS|BTN_L3|BTN_TOUCHPAD))==(BTN_CROSS|BTN_L3|BTN_TOUCHPAD));
    assert(data.touch_count==1 && data.touches[0].x==480 && data.touches[0].y==471);
    inject(path,"touchpad_right");
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==1440);
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.touch_count==0);

    SDL_VirtualJoystickTouchpadDesc touch={.nfingers=2};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="bbport test controller";
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    desc.ntouchpads=1;
    desc.touchpads=&touch;
    desc.Rumble=rumble;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);
    assert(joystick);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.75f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,true,0.25f,1.0f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(gamepad && data.touch_count==2 && (data.buttons & BTN_TOUCHPAD));
    const uint8_t vibration[]={64,192};
    assert(pad_vibration(1,vibration)==0 && rumble_calls==0);
    input.pad_vibration=1;
    assert(pad_vibration(1,vibration)==0 && rumble_calls==1);
    assert(rumble_low==(64*257+1)/2 && rumble_high==(192*257+1)/2);
    input.pad_vibration_strength=100;
    assert(pad_vibration(1,vibration)==0 && rumble_calls==2);
    assert(rumble_low==64*257 && rumble_high==192*257);
    assert(data.touches[0].x==1439 && data.touches[0].y==471 && data.touches[0].id==0);
    assert(data.touches[1].x==480 && data.touches[1].y==942 && data.touches[1].id==1);
    capture=1;
    assert(pad_read_state(1,&data)==0 && data.touch_count==0 && data.buttons==0);
    capture=0;
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,false,0,0,0));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==480);
    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    unlink(path);
    unlink(config);
    puts("PASS: pad ABI, debug camera chord, left/right clicks, SDL touch, vibration controls, overlay capture");
}
