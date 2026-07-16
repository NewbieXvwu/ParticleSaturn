#import <Cocoa/Cocoa.h>

#include "OpenGL41Surface.h"

namespace ParticleSaturn::Gpu::OpenGL41 {

OpenGL41Surface::OpenGL41Surface(void* nativeView) {
    const NSOpenGLPixelFormatAttribute attributes[] = {
        NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion4_1Core,
        NSOpenGLPFAColorSize, 24,
        NSOpenGLPFAAlphaSize, 8,
        NSOpenGLPFADoubleBuffer,
        0,
    };
    auto* format = [[NSOpenGLPixelFormat alloc] initWithAttributes:attributes];
    auto* context = [[NSOpenGLContext alloc] initWithFormat:format shareContext:nil];
    [format release];
    [context setView:(NSView*)nativeView];
    context_ = context;
}

OpenGL41Surface::~OpenGL41Surface() {
    [(NSOpenGLContext*)context_ clearDrawable];
    [(NSOpenGLContext*)context_ release];
}

bool OpenGL41Surface::MakeCurrent() {
    if (context_ == nullptr) {
        return false;
    }
    [(NSOpenGLContext*)context_ makeCurrentContext];
    return true;
}

void OpenGL41Surface::Present() {
    [(NSOpenGLContext*)context_ flushBuffer];
}

} // namespace ParticleSaturn::Gpu::OpenGL41
