// user/tests/fb_mmio_map.c
//
// Phase 29 Session D — SYS_CONSOLE_GFX_MAP_FB exclusive-owner gate.
//
// 4 asserts:
//   1. With no recorded owner, first MAP_FB call succeeds + writes dims
//      with non-zero width/height/pitch + size_bytes covers WxHx4
//   2. A second call from the same caller succeeds (idempotent)
//   3. Force a different "owner" via DEBUG_FB_OWNER_SET to a sentinel pid,
//      then MAP_FB returns -EPERM (-1) for us
//   4. Restoring our pid as owner makes MAP_FB succeed again

#include "../libtap.h"
#include "../syscalls.h"
#include <stdint.h>

extern int printf(const char *fmt, ...);

void _start(void) {
    tap_plan(4);

    (void)syscall_pledge(PLEDGE_SYS_QUERY | PLEDGE_SYS_CONTROL |
                         PLEDGE_IPC_SEND | PLEDGE_IPC_RECV);

    // Reset owner so test is rerunnable.
    int32_t my_pid = (int32_t)syscall_getpid();
    (void)syscall_debug_fb_owner_set(-1);

    uint64_t handle1 = 0;
    fb_dims_u_t dims = {0};
    long rc = syscall_console_gfx_map_fb(&handle1, &dims);
    if (rc != 0) printf("# first map rc=%ld\n", rc);
    int dims_ok = (dims.width_px > 0 && dims.height_px > 0 &&
                   dims.pitch_bytes > 0 &&
                   dims.size_bytes >= (uint64_t)dims.pitch_bytes * dims.height_px);
    TAP_ASSERT(rc == 0 && handle1 != 0 && dims_ok,
               "1. first MAP_FB succeeds with sane dims");

    // 2. Same caller: should succeed again.
    uint64_t handle2 = 0;
    fb_dims_u_t dims2 = {0};
    rc = syscall_console_gfx_map_fb(&handle2, &dims2);
    TAP_ASSERT(rc == 0, "2. same-caller MAP_FB is idempotent");

    // 3. Force owner = some other pid.
    (void)syscall_debug_fb_owner_set(my_pid + 1000);
    uint64_t handle3 = 0;
    fb_dims_u_t dims3 = {0};
    rc = syscall_console_gfx_map_fb(&handle3, &dims3);
    TAP_ASSERT(rc != 0, "3. different-owner MAP_FB returns error");

    // 4. Restore our pid, MAP_FB succeeds again.
    (void)syscall_debug_fb_owner_set(my_pid);
    uint64_t handle4 = 0;
    fb_dims_u_t dims4 = {0};
    rc = syscall_console_gfx_map_fb(&handle4, &dims4);
    TAP_ASSERT(rc == 0, "4. restored owner MAP_FB succeeds");

    tap_done();
    syscall_exit(0);
}
