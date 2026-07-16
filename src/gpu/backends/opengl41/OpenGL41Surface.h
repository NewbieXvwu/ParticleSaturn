#pragma once

namespace ParticleSaturn::Gpu::OpenGL41 {

class OpenGL41Surface {
public:
    explicit OpenGL41Surface(void* nativeView);
    ~OpenGL41Surface();

    OpenGL41Surface(const OpenGL41Surface&) = delete;
    OpenGL41Surface& operator=(const OpenGL41Surface&) = delete;

    bool MakeCurrent();
    void Present();

private:
    void* context_ = nullptr;
};

} // namespace ParticleSaturn::Gpu::OpenGL41
