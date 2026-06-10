#import "vm_dir.h"

#include <pwd.h>
#include <unistd.h>

@implementation VmDir

+ (NSURL *)vmsRootDirectory {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSURL *appSupport = nil;

    /* Under sudo (the headless daemon's required launch mode), NSUserDomainMask
     * resolves to root's home (/var/root) -- a DIFFERENT VM registry and a
     * cold restore-image cache than the user's GUI. Resolve the INVOKING
     * user's real home instead (SUDO_USER -> passwd), so `sudo AppSandbox
     * --headless` manages the same VMs the user sees in the app. Dormant in
     * the GUI (never runs as root). */
    const char *sudo_user = getenv("SUDO_USER");
    if (geteuid() == 0 && sudo_user && *sudo_user) {
        struct passwd *pw = getpwnam(sudo_user);
        if (pw && pw->pw_dir && pw->pw_dir[0]) {
            appSupport = [NSURL fileURLWithPath:
                [[NSString stringWithUTF8String:pw->pw_dir]
                    stringByAppendingPathComponent:@"Library/Application Support"]
                                    isDirectory:YES];
        }
    }
    if (!appSupport) {
        appSupport = [fm URLForDirectory:NSApplicationSupportDirectory
                                inDomain:NSUserDomainMask
                       appropriateForURL:nil
                                  create:YES
                                   error:nil];
    }
    NSURL *root = [[appSupport URLByAppendingPathComponent:@"AppSandbox" isDirectory:YES]
                      URLByAppendingPathComponent:@"VMs" isDirectory:YES];
    [fm createDirectoryAtURL:root withIntermediateDirectories:YES attributes:nil error:nil];
    return root;
}

+ (NSURL *)directoryForVm:(NSString *)name {
    return [[self vmsRootDirectory] URLByAppendingPathComponent:name isDirectory:YES];
}

+ (NSURL *)diskImageURLFor:(NSString *)name {
    return [[self directoryForVm:name] URLByAppendingPathComponent:@"disk.img"];
}

+ (NSURL *)auxiliaryStorageURLFor:(NSString *)name {
    return [[self directoryForVm:name] URLByAppendingPathComponent:@"aux.img"];
}

+ (NSURL *)hardwareModelURLFor:(NSString *)name {
    return [[self directoryForVm:name] URLByAppendingPathComponent:@"hardware.bin"];
}

+ (NSURL *)machineIdentifierURLFor:(NSString *)name {
    return [[self directoryForVm:name] URLByAppendingPathComponent:@"machine-id.bin"];
}

+ (BOOL)ensureDirectoryFor:(NSString *)name error:(NSError **)error {
    NSURL *dir = [self directoryForVm:name];
    return [[NSFileManager defaultManager] createDirectoryAtURL:dir
                                    withIntermediateDirectories:YES
                                                     attributes:nil
                                                          error:error];
}

+ (BOOL)vmExists:(NSString *)name {
    BOOL isDir = NO;
    NSString *path = [self directoryForVm:name].path;
    return [[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&isDir] && isDir;
}

+ (BOOL)deleteVm:(NSString *)name error:(NSError **)error {
    NSURL *dir = [self directoryForVm:name];
    return [[NSFileManager defaultManager] removeItemAtURL:dir error:error];
}

@end
