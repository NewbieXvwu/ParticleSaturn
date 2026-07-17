#import <Cocoa/Cocoa.h>

#include "MacOSApplication.h"

namespace ParticleSaturn::Platform::MacOS {

bool RestartApplication() {
    const auto* arguments = [[NSProcessInfo processInfo] arguments];
    if ([arguments count] == 0) return false;
    auto* task = [[NSTask alloc] init];
    [task setLaunchPath:[arguments objectAtIndex:0]];
    [task setArguments:[arguments subarrayWithRange:NSMakeRange(1, [arguments count] - 1)]];
    @try {
        [task launch];
    } @catch (NSException*) {
        [task release];
        return false;
    }
    [task release];
    return true;
}

} // namespace ParticleSaturn::Platform::MacOS
