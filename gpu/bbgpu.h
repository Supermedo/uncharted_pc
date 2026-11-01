/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef BBGPU_H
#define BBGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} BbGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void bbgpu_register_kernel(void);
/* Co-op: the game folder and param.sfo identity for the network and PSN libraries. */
void bbnet_configure(const char *app0, const char *serial, const char *title, const char *app_ver);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int bbgpu_init(const BbGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t bbgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int bbgpu_handle_fault(void *ucontext, void *address);
/* BB_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void bbgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int bbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int bbgpu_text_input_poll(char *out_utf8, uint64_t size);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int bbgpu_overlay_captures_input(void);
/* Button prompts (button_prompts.cpp), the device used last: 0 keyboard or mouse (pictures of the
 * bound keys), 1 a PlayStation controller (the game's own buttons), 2 an Xbox or other controller
 * (Xbox buttons). */
void bbgpu_prompts_controller(int device);
/* Mouse controls: look=1 captures the cursor while the game window has the focus (relative
 * mode). sample returns the motion (pixels) and wheel steps since the last call and the held
 * buttons (1 left, 2 right, 4 middle, 8 X1, 16 X2); 1 while the cursor is captured. */
void bbgpu_mouse_configure(int look);
/* Heading (degrees, about the vertical axis) of the camera the game last rendered with and how old
 * it is; 0 before the first frame with a known camera. */
int bbgpu_camera_yaw(float *yaw_degrees, double *age_seconds);
/* 1 while the menu's Gyro aiming setting is on (bbport.ini gyro_aim). */
int bbgpu_gyro_aim(void);
int bbgpu_mouse_sample(float *dx, float *dy, uint32_t *buttons, int *wheel);
/* Built-in gyro (runtime_pad.c, runtime_gyro.c). Queued gyro samples, oldest first: 3 rates per
 * sample (rad/s, SDL's axes), the accelerometer's latest reading with it (m/s^2, 0 before the first) and the
 * sensor's time (ns); returns how many (window.cpp). */
int bbgpu_gyro_take(float *rates, float *accels, uint64_t *ns, int max);
/* 1 while the game's window has the focus. */
int bbgpu_window_focused(void);
/* The launcher's gyro settings (bbport.ini gyro_*): speed deadzone and precision speed in deg/s, builtin
 * on/off, button (SDL button, -2/-3 the triggers, -1 none: always on), mode (0 hold to enable, 1 hold to
 * suppress, 2 toggle), space (3DOF to 2D Conversion Style, GYRO_SPACE_*), Local Space's axis angle
 * (degrees) and complement (0..1). */
void bbgpu_gyro_settings(float *sensitivity, int *invert_x, int *invert_y, float *speed_deadzone,
                         float *precision_speed, int *builtin, int *button, int *mode, int *space,
                         float *local_angle, float *local_complement);
/* What the menu shows: 0 no controller, 1 Steam or no gyro on this controller, 2 built in. */
void bbgpu_gyro_status(int source, const char *controller);
/* 1 once after the menu's Calibrate was pressed. */
int bbgpu_gyro_calibrate_requested(void);
/* The learned gyro offset (rad/s): stored, and written to bbport.ini when save is 1; and read back. */
void bbgpu_gyro_bias(const float bias[3], int save);
void bbgpu_gyro_saved_bias(float bias[3]);
/* In-game trophy notice (the overlay draws it for a few seconds, one at a time): grade 1 platinum,
 * 2 gold, 3 silver, 4 bronze; png is the trophy's icon (a PNG file) or empty. */
void bbgpu_trophy_notify(int grade, const char *game, const char *name, const void *png, uint64_t png_size);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned bbgpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif
