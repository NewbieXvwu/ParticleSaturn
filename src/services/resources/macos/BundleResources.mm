#import <Foundation/Foundation.h>

#include "BundleResources.h"

namespace ParticleSaturn::Services::Resources::MacOS {

std::string LocateModel(const std::string& filename) {
    NSString* name = [NSString stringWithUTF8String:filename.c_str()];
    NSString* path = [[NSBundle mainBundle] pathForResource:[name stringByDeletingPathExtension]
                                                    ofType:[name pathExtension]];
    return path == nil ? std::string{} : std::string{[path UTF8String]};
}

} // namespace ParticleSaturn::Services::Resources::MacOS
