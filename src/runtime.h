#ifndef BB_RUNTIME_H
#define BB_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#include "compat_win.h"
/* No signals on Windows: the exception handler resumes the thread in bb_longjmp
 * (gpu/shim/bbport_toggles.h uses the same buffer type). */
typedef bb_jmp_buf sigjmp_buf;
#define sigsetjmp(buffer, save) bb_setjmp(buffer)
#else
#include <setjmp.h>
#endif
/* Recovery point for speculative guest memory reads on this thread (probe.c fault handler). */
extern __thread sigjmp_buf *runtime_fault_recover;
/* Restarts the game (in-game settings menu, render resolution change). */
void runtime_restart(void);
#define ABI __attribute__((sysv_abi))
typedef void (ABI *GuestCallback)(void);
void runtime_start(uint64_t capabilities);
uintptr_t runtime_resolve(const char *name, int is_data);
void runtime_report(void);
void runtime_finalize(void *dso);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
const char *runtime_import_name(const char *name);
uintptr_t runtime_rwlock_resolve(const char *name);
void runtime_rwlock_report(void);
void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_procparam(void *param);
void **runtime_application_heap_api(void);
uintptr_t runtime_thread_resolve(const char *name);
void runtime_thread_report(void);
void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
int32_t *runtime_errno(void);
uintptr_t runtime_sema_resolve(const char *name);
void runtime_sema_report(void);
unsigned runtime_sema_waiters(uint32_t id);
uintptr_t runtime_time_resolve(const char *name);
void runtime_content_configure(const uint32_t values[5]);
uintptr_t runtime_content_resolve(const char *name);
void runtime_content_report(void);
typedef struct { const char *name; void *function; } RuntimeExport;
#define RUNTIME_LOOKUP(table, nid) runtime_lookup(table, sizeof(table)/sizeof(*(table)), nid)
const char *runtime_symbol(const char *nid);
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid);
uintptr_t runtime_kernel_resolve(const char *name);
uintptr_t runtime_file_resolve(const char *name);
uintptr_t runtime_services_resolve(const char *name);
void runtime_file_report(void);
void runtime_file_configure(const char *app0, const char *user);
int runtime_file_mount(const char *guest, const char *host);
void runtime_file_unmount(const char *guest);
int runtime_file_translate(const char *guest, char *out, size_t size);
int64_t runtime_file_open(const char *path, int flags, int mode);
int64_t runtime_file_close(int fd);
int64_t runtime_file_read(int fd, void *buffer, uint64_t size);
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset);
int64_t runtime_file_write(int fd, const void *buffer, uint64_t size);
int64_t runtime_file_lseek(int fd, int64_t offset, int whence);
int64_t runtime_file_stat(const char *path, void *guest_stat);
int64_t runtime_file_fstat(int fd, void *guest_stat);
int64_t runtime_file_getdents(int fd, char *buffer, uint64_t size, int64_t *base);
void runtime_thread_keys_cleanup(void);
/* The PS4's floating-point state for guest code on this thread (MXCSR, x87 control word). */
void runtime_set_guest_fpu(void);
/* Guest-visible errno values are FreeBSD's. */
int32_t runtime_guest_errno(int host_errno);
void *runtime_low_map(size_t size, int prot);
void *runtime_image_map(size_t size, int prot);
/* Windows: reserves the PS4 user address range as a placeholder (no-op elsewhere). */
void runtime_memory_reserve(void);
uintptr_t runtime_ajm_resolve(const char *name);
void runtime_ajm_report(void);
uintptr_t runtime_audio_resolve(const char *name);
void runtime_audio_report(void);
uintptr_t runtime_pad_resolve(const char *name);
void runtime_pad_report(void);
uintptr_t runtime_rtc_resolve(const char *name);
const char *runtime_file_user_dir(void);
void runtime_savedata_configure(const char *title);
uintptr_t runtime_savedata_resolve(const char *name);
/* Trophies (runtime_trophy.c): the folder run.py prepared for the running game (scripts/trophies.py). */
void runtime_trophy_configure(const char *dir);
uintptr_t runtime_trophy_resolve(const char *name);
void runtime_savedata_report(void);
int runtime_savedata_pick(int32_t user, const char *title, const char (*names)[32], uint32_t count,
                          int which, const char *focus, char out[32], void *param_out);
void runtime_thread_attach_host(const char *name);
uintptr_t runtime_fiber_resolve(const char *name);
void runtime_fiber_report(void);
/* Host functions the loader runs at the start of matching guest functions (probe.c): the code
 * at `offset` must equal `signature`; `moved` bytes of whole position-independent instructions
 * move to the thunk. The callback gets the guest's rdi, rsi and r14 there and the image base.
 * Without a callback, `patch` replaces the first `moved` bytes instead (a fix in the game's code). */
typedef struct {
    const char *name;
    uint64_t offset;
    const unsigned char *signature;
    size_t signature_size, moved;
    void (__attribute__((ms_abi)) *callback)(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14);
    const unsigned char *patch;
} RuntimeGuestCallback;
const RuntimeGuestCallback *runtime_guest_callbacks(size_t *count);
#endif
