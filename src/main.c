#include "app.h"
#include "config.h"
#include "ui.h"
#include "util.h"

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/touch.h>
#include <vita2d.h>

#include <string.h>

/*
 * Locks CPU/BUS/GPU clocks at their maximum so playback keeps up with the
 * screen off: with the display asleep the OS drops the clocks into a
 * power-saving profile and the decode/download threads starve exactly when
 * the audio backlog is still filling. Re-asserted periodically because the
 * system resets the profile on power-state changes (screen off/on). Returns
 * the last error code seen (0 when every request succeeded).
 */
static int lock_performance(void)
{
    int err = 0;

    /* 444 MHz CPU / 222 MHz BUS is the Vita maximum; GPU 222/111 MHz. */
    if (scePowerSetArmClockFrequency(444) < 0)
        err = -1;
    if (scePowerSetBusClockFrequency(222) < 0)
        err = -1;
    if (scePowerSetGpuClockFrequency(222) < 0)
        err = -1;
    if (scePowerSetGpuXbarClockFrequency(111) < 0)
        err = -1;
    return err;
}

/* Re-arm the performance profile while playback is active; the system drops
 * the clocks when the screen turns off, which is exactly the scenario the
 * user reported. */
static void performance_watchdog(void)
{
    static int frame_count = 0;

    /* Every ~5 seconds at 60 fps. */
    if (++frame_count < 300)
        return;
    frame_count = 0;

    if (scePowerGetArmClockFrequency() < 444 ||
        scePowerGetBusClockFrequency() < 222)
        lock_performance();
}

int main(int argc, char *argv[])
{
    int quit = 0;
    SceAppUtilInitParam appUtilParam;
    SceAppUtilBootParam appUtilBoot;
    SceCommonDialogConfigParam cmnDlgCfgParam;

    (void)argc;
    (void)argv;

    vm_log_init();
    vm_log("main: starting\n");

    /* Full clocks from the first frame; audio decode and Wi-Fi transfers
     * need the headroom, especially with the screen off. */
    if (lock_performance() < 0)
        vm_log("main: performance lock rejected (some clocks stayed lower)\n");

    sceSysmoduleLoadModule(SCE_SYSMODULE_APPUTIL);
    sceSysmoduleLoadModule(SCE_SYSMODULE_IME);

    memset(&appUtilParam, 0, sizeof(appUtilParam));
    memset(&appUtilBoot, 0, sizeof(appUtilBoot));
    sceAppUtilInit(&appUtilParam, &appUtilBoot);
    memset(&cmnDlgCfgParam, 0, sizeof(cmnDlgCfgParam));
    sceCommonDialogSetConfigParam(&cmnDlgCfgParam);

    /* The pad needs analog mode for the stick-driven list scrolling, and the
     * touchscreen has to be explicitly started before it reports anything. */
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,
                             SCE_TOUCH_SAMPLING_STATE_START);

    if (vita2d_init() < 0) {
        vm_log("main: vita2d_init failed\n");
        sceKernelExitProcess(1);
        return 1;
    }
    vita2d_set_clear_color(RGBA8(0x10, 0x10, 0x18, 0xff));
    vita2d_set_vblank_wait(1);

    /* Font + boot phases on screen so a hang leaves its phase visible. */
    ui_hud_load();
    ui_hud_show("vita2d init");

    vm_log("main: app_init\n");
    app_init();
    vm_log("main: app_init done\n");
    ui_hud_show("app init done");

    ui_init();
    vm_log("main: ui_init done\n");
    ui_hud_show("ui init done");

    while (!quit) {
        app_tick();
        performance_watchdog();
        ui_frame(&quit);
    }

    vm_log("main: shutting down\n");

    ui_fini();
    app_shutdown();
    vita2d_fini();

    sceKernelExitProcess(0);
    return 0;
}
