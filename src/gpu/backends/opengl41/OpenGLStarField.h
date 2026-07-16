#pragma once

#include <cstdint>

namespace ParticleSaturn::Gpu::OpenGL41 {

class OpenGLStarField {
public:
    static constexpr std::uint32_t StarCount = 50000;

    OpenGLStarField() = default;
    ~OpenGLStarField();
    OpenGLStarField(const OpenGLStarField&) = delete;
    OpenGLStarField& operator=(const OpenGLStarField&) = delete;

    bool Initialize(const char* vertexShaderPath, const char* fragmentShaderPath, std::uint32_t seed = 1337U);
    void Draw(float timeSeconds, std::uint32_t width, std::uint32_t height) const;

private:
    std::uint32_t program_ = 0;
    std::uint32_t vertexArray_ = 0;
    std::uint32_t buffer_ = 0;
};

} // namespace ParticleSaturn::Gpu::OpenGL41
