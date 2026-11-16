#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "../src/runtime_pad.c"

static int capture;
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

int main(void) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR); /* Assertions must not open a desktop dialog. */
#endif
    setvbuf(stdout,NULL,_IONBF,0);
    char path[]="bbport-pad-test-XXXXXX";
    int fd=mkstemp(path);
    assert(fd>=0);
    close(fd);
    char config[]="bbport-input-test-XXXXXX";
    fd=mkstemp(config);
    assert(fd>=0);
    close(fd);
    inject(config,"pad_vibration=0\npad_vibration_strength=50\npad.cross=a|paddle2\n");
    set_env("BB_PAD_FILE",path);
    set_env("BB_PAD_HOST","1");
    set_env("BB_INPUT_CONFIG",config);
    set_env("SDL_VIDEODRIVER","dummy");
    /* Only the virtual test controller is a gamepad, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189,0x1d50/0x6190");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    assert(controller_profile_for(SDL_GAMEPAD_TYPE_PS5,SDL_GAMEPAD_BUTTON_LABEL_CROSS,
                                  SDL_GAMEPAD_BUTTON_LABEL_CIRCLE)==PROFILE_PLAYSTATION);
    assert(controller_profile_for(SDL_GAMEPAD_TYPE_XBOXONE,SDL_GAMEPAD_BUTTON_LABEL_A,
                                  SDL_GAMEPAD_BUTTON_LABEL_B)==PROFILE_XBOX);
    assert(controller_profile_for(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO,SDL_GAMEPAD_BUTTON_LABEL_B,
                                  SDL_GAMEPAD_BUTTON_LABEL_A)==PROFILE_NINTENDO);
    assert(controller_profile_for(SDL_GAMEPAD_TYPE_UNKNOWN,SDL_GAMEPAD_BUTTON_LABEL_B,
                                  SDL_GAMEPAD_BUTTON_LABEL_A)==PROFILE_NINTENDO);
    assert(controller_profile_for(SDL_GAMEPAD_TYPE_STEAM,SDL_GAMEPAD_BUTTON_LABEL_A,
                                  SDL_GAMEPAD_BUTTON_LABEL_B)==PROFILE_STEAM);
    assert(controller_profile_for(SDL_GAMEPAD_TYPE_STANDARD,SDL_GAMEPAD_BUTTON_LABEL_A,
                                  SDL_GAMEPAD_BUTTON_LABEL_B)==PROFILE_GENERIC);
    assert(controller_profile_for(SDL_GAMEPAD_TYPE_GAMECUBE,SDL_GAMEPAD_BUTTON_LABEL_A,
                                  SDL_GAMEPAD_BUTTON_LABEL_X)==PROFILE_GAMECUBE);
    assert(!strcmp(controller_profile_name(PROFILE_GAMECUBE),"gamecube"));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);
    PadData data;
    inject(path,"cross l3 touchpad_left");
    assert(pad_read_state(1,&data)==0);
    assert(input.pad_vibration==0);
    assert(input.pad_vibration_strength==50);
    assert(input.pad[0][0]==SDL_GAMEPAD_BUTTON_SOUTH && input.pad[0][1]==SDL_GAMEPAD_BUTTON_LEFT_PADDLE1);
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
    desc.name="ignored test controller";
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    desc.ntouchpads=1;
    desc.touchpads=&touch;
    desc.Rumble=rumble;
    SDL_JoystickID first=SDL_AttachVirtualJoystick(&desc);
    assert(first!=0);
    desc.name="selected test controller";
    desc.product_id=0x6190;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    char selected[17];
    controller_key_text(id,selected);
    set_env("BB_PAD_DEVICE",selected);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);
    assert(joystick);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.75f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,true,0.25f,1.0f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(gamepad && !strcmp(SDL_GetGamepadName(gamepad),"selected test controller") &&
           data.touch_count==2 && (data.buttons & BTN_TOUCHPAD));
    const uint8_t vibration[]={64,192};
    assert(pad_vibration(1,vibration)==0 && rumble_calls==0);
    input.pad_vibration=1;
    assert(pad_vibration(1,vibration)==0 && rumble_calls==1);
    assert(rumble_low==(64*257+1)/2 && rumble_high==(192*257+1)/2);
    input.pad_vibration_strength=100;
    assert(pad_vibration(1,vibration)==0 && rumble_calls==2);
    assert(rumble_low==64*257 && rumble_high==192*257);
    assert(runtime_pad_tool(1,selected,10)==0);
    assert(rumble_calls>=3);
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
    /* A rear button supplements the face button; releasing either keeps the other held. */
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,false));
    SDL_UpdateJoysticks(); SDL_UpdateGamepads();
    /* Release the overlay's held-button guard before starting a new input sequence. */
    assert(pad_read_state(1,&data)==0 && data.buttons==0);
    set_pad(0,"a|paddle2");
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_SOUTH,true));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,true));
    SDL_UpdateJoysticks(); SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && (data.buttons & BTN_CROSS));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_SOUTH,false));
    SDL_UpdateJoysticks(); SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && (data.buttons & BTN_CROSS));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,false));
    SDL_UpdateJoysticks(); SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && !(data.buttons & BTN_CROSS));
    /* Explicit remapping must suppress the legacy touchpad-click passthrough. */
    set_pad(0,"touchpad");
    configure_native_touchpad();
    assert(!input.native_touchpad);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    SDL_UpdateJoysticks(); SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && (data.buttons & BTN_CROSS) && !(data.buttons & BTN_TOUCHPAD));
    set_pad(0,"a"); set_pad(11,"back|lefttrigger"); configure_native_touchpad();
    assert(!input.native_touchpad);
    set_pad(11,"back"); configure_native_touchpad();
    assert(input.native_touchpad);
    /* A secondary digital trigger wins over an analog source in either slot order. */
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,false));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,true));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,0));
    set_pad(6,"lefttrigger|paddle2");
    SDL_UpdateJoysticks(); SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.l2==255 && (data.buttons & BTN_L2));
    set_pad(6,"paddle2|lefttrigger");
    assert(pad_read_state(1,&data)==0 && data.l2==255);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,false));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-32768));
    SDL_UpdateJoysticks(); SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.l2==0 && !(data.buttons & BTN_L2));
    set_pad(0,"a|invalid");
    assert(input.pad[0][0]==SDL_GAMEPAD_BUTTON_SOUTH && input.pad[0][1]==-1);
    /* The launcher's live test reports physical buttons independently of game mappings. */
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_SOUTH,true));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,true));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767));
    SDL_UpdateJoysticks(); SDL_UpdateGamepads();
    FILE *sample=tmpfile();
    assert(sample);
    controller_pressed(gamepad,sample);
    rewind(sample);
    char state[512];
    assert(fgets(state,sizeof(state),sample));
    assert(strstr(state,"a,") && strstr(state,"paddle2,") && strstr(state,"righttrigger,"));
    assert(!strstr(state,"lefttrigger,"));
    fclose(sample);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_SOUTH,false));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,false));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768));
    SDL_UpdateJoysticks(); SDL_UpdateGamepads();
    sample=tmpfile();
    assert(sample);
    controller_pressed(gamepad,sample);
    rewind(sample);
    assert(fgets(state,sizeof(state),sample));
    assert(!strstr(state,"a,") && !strstr(state,"paddle2,") && !strstr(state,"righttrigger,"));
    fclose(sample);
    /* SDL's GameCube mapping uses physical A/X/B/Y, not Switch's B/A/Y/X. */
    assert(SDL_SetGamepadMapping(id,"00000000000000000000000000000000,GameCube test,type:gamecube,a:b0,b:b1,x:b2,y:b3,start:b6,"
        "rightshoulder:b10,lefttrigger:a4,righttrigger:a5,misc3:b21,misc4:b22,"));
    SDL_UpdateGamepads();
    assert(SDL_GetGamepadType(gamepad)==SDL_GAMEPAD_TYPE_GAMECUBE);
    assert(controller_profile(gamepad)==PROFILE_GAMECUBE);
    assert(SDL_GetGamepadButtonLabel(gamepad,SDL_GAMEPAD_BUTTON_SOUTH)==SDL_GAMEPAD_BUTTON_LABEL_A);
    assert(SDL_GetGamepadButtonLabel(gamepad,SDL_GAMEPAD_BUTTON_EAST)==SDL_GAMEPAD_BUTTON_LABEL_X);
    assert(SDL_GetGamepadButtonLabel(gamepad,SDL_GAMEPAD_BUTTON_WEST)==SDL_GAMEPAD_BUTTON_LABEL_B);
    assert(SDL_GetGamepadButtonLabel(gamepad,SDL_GAMEPAD_BUTTON_NORTH)==SDL_GAMEPAD_BUTTON_LABEL_Y);
    set_env("BB_PAD_PROFILE","auto");
    assert(prompts_pad_device(gamepad)==2);
    set_env("BB_PAD_PROFILE","gamecube");
    assert(prompts_pad_device(NULL)==2);
    set_env("BB_PAD_PROFILE","auto");
    assert(!SDL_GamepadHasButton(gamepad,SDL_GAMEPAD_BUTTON_LEFT_STICK));
    assert(!SDL_GamepadHasButton(gamepad,SDL_GAMEPAD_BUTTON_BACK));
    const uint32_t face_actions[]={BTN_CROSS,BTN_CIRCLE,BTN_SQUARE,BTN_TRIANGLE};
    for (int button=0;button<4;++button) {
        set_pad(button,(const char *[]) {"a","b","x","y"}[button]);
        assert(SDL_SetJoystickVirtualButton(joystick,button,true));
        SDL_UpdateJoysticks(); SDL_UpdateGamepads();
        assert(pad_read_state(1,&data)==0 && data.buttons==face_actions[button]);
        assert(SDL_SetJoystickVirtualButton(joystick,button,false));
        SDL_UpdateJoysticks(); SDL_UpdateGamepads();
    }
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    set_env("BB_PAD_DEVICE","missing-controller");
    assert(pad_read_state(1,&data)==0 && gamepad==NULL);
    SDL_CloseJoystick(joystick);
    assert(SDL_DetachVirtualJoystick(id));
    assert(SDL_DetachVirtualJoystick(first));
    SDL_Quit();
    unlink(path);
    unlink(config);
    puts("PASS: pad ABI, debug camera chord, left/right clicks, SDL touch, vibration controls, overlay capture");
}
