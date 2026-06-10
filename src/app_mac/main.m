#import <Cocoa/Cocoa.h>
#import "AppDelegate.h"
#include "headless.h"
#include <string.h>

int main(int argc, const char *argv[]) {
    /* Headless daemon mode: AppSandbox.app/Contents/MacOS/AppSandbox --headless
       (normal launches pass no args, so the GUI path below is unaffected). */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0)
            return run_headless(argc, argv);
    }

    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];

        /* Single-instance: one AppSandbox (GUI or headless) at a time. The
           daemon acquires this SAME flock, so GUI + headless are mutually
           exclusive -- both own the one per-user vms.cfg (no file locking).
           LaunchServices blocks a second GUI instance of the bundle but NOT a
           direct-binary --headless launch, hence the explicit lock. The fd is
           intentionally held (leaked) for the process lifetime; the OS
           releases it on crash. Mirrors src/app_win/main.c. */
        if (asb_instance_lock_acquire() < 0) {
            [app setActivationPolicy:NSApplicationActivationPolicyRegular];
            NSAlert *alert = [[NSAlert alloc] init];
            alert.alertStyle = NSAlertStyleInformational;
            alert.messageText = @"AppSandbox is already running";
            alert.informativeText = @"Either the app window or the headless "
                                    @"daemon is already running. Only one can "
                                    @"run at a time.";
            [alert runModal];
            return 0;
        }

        AppDelegate *delegate = [[AppDelegate alloc] init];
        [app setDelegate:delegate];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    return 0;
}
