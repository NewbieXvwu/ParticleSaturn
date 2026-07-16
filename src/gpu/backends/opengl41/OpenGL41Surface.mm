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

bool OpenGL41Surface::SetView(void* nativeView) {
    if (context_ == nullptr || nativeView == nullptr) return false;
    [(NSOpenGLContext*)context_ setView:(NSView*)nativeView];
    [(NSOpenGLContext*)context_ update];
    return true;
}

bool OpenGL41Surface::SetTransparent(bool transparent) {
    if (context_ == nullptr) return false;
    GLint opaque = transparent ? 0 : 1;
    [(NSOpenGLContext*)context_ setValues:&opaque forParameter:NSOpenGLContextParameterSurfaceOpacity];
    [(NSOpenGLContext*)context_ update];
    return true;
}

void OpenGL41Surface::Present() {
    [(NSOpenGLContext*)context_ flushBuffer];
}

} // namespace ParticleSaturn::Gpu::OpenGL41
