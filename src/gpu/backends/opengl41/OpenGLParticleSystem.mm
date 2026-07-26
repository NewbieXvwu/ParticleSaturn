#import <OpenGL/gl3.h>

#include "OpenGLParticleSystem.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>

namespace ParticleSaturn::Gpu::OpenGL41 {

namespace {

constexpr GLsizeiptr ParticleBytes = sizeof(OpenGLParticleSystem::ParticleSnapshot);
static_assert(sizeof(OpenGLParticleSystem::ParticleSnapshot) == ParticleBytes);

struct DrawArraysIndirectCommand {
    GLuint count;
    GLuint instanceCount;
    GLuint first;
    GLuint baseInstance;
};

float Random01(std::uint32_t& state) {
    const std::uint32_t pcgState = state * 747796405U + 2891336453U;
    const std::uint32_t word = ((pcgState >> ((pcgState >> 28U) + 4U)) ^ pcgState) * 277803737U;
    state = (word >> 22U) ^ word;
    return static_cast<float>(state) * (1.0f / 4294967296.0f);
}

void UnpackColor(std::uint32_t color, float& red, float& green, float& blue) {
    red = static_cast<float>((color >> 16U) & 0xffU) / 255.0f;
    green = static_cast<float>((color >> 8U) & 0xffU) / 255.0f;
    blue = static_cast<float>(color & 0xffU) / 255.0f;
}

std::uint32_t PackColor(float red, float green, float blue, float alpha) {
    const auto channel = [](float value) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    return (channel(alpha) << 24U) | (channel(blue) << 16U) | (channel(green) << 8U) | channel(red);
}

OpenGLParticleSystem::ParticleSnapshot InitializeDiligentParticle(std::uint32_t id, std::uint32_t seed) {
    constexpr float radius = 18.0f;
    std::uint32_t rng = id * 1973U + seed * 9277U + 26699U;
    OpenGLParticleSystem::ParticleSnapshot particle{};
    float red = 1.0f;
    float green = 1.0f;
    float blue = 1.0f;
    float alpha = 1.0f;
    if (Random01(rng) < 0.25f) {
        const float theta = 6.28318f * Random01(rng);
        const float phi = std::acos(2.0f * Random01(rng) - 1.0f);
        particle.position[0] = radius * std::sin(phi) * std::cos(theta);
        particle.position[1] = radius * std::cos(phi) * 0.9f;
        particle.position[2] = radius * std::sin(phi) * std::sin(theta);
        const float latitude = (particle.position[1] / (0.9f * radius) + 1.0f) * 0.5f;
        const int paletteIndex = static_cast<int>(latitude * 4.0f + std::cos(latitude * 40.0f) * 0.8f +
                                                  std::cos(latitude * 15.0f) * 0.4f);
        constexpr std::uint32_t palette[4] = {0xE3DAC5U, 0xC9A070U, 0xE3DAC5U, 0xB08D55U};
        UnpackColor(palette[(paletteIndex % 4 + 4) % 4], red, green, blue);
        particle.position[3] = 1.0f + Random01(rng) * 0.8f;
        alpha = 0.8f;
    } else {
        const float zone = Random01(rng);
        float ringRadius = 0.0f;
        float size = 1.0f;
        if (zone < 0.15f) {
            ringRadius = radius * (1.235f + Random01(rng) * 0.29f);
            UnpackColor(0x2A2520U, red, green, blue);
            size = 0.5f;
            alpha = 0.3f;
        } else if (zone < 0.65f) {
            const float mix = Random01(rng);
            ringRadius = radius * (1.525f + mix * 0.425f);
            red = (205.0f + (220.0f - 205.0f) * mix) / 255.0f;
            green = (191.0f + (203.0f - 191.0f) * mix) / 255.0f;
            blue = (160.0f + (186.0f - 160.0f) * mix) / 255.0f;
            size = 0.8f + Random01(rng) * 0.6f;
            alpha = std::sin(ringRadius * 2.0f) > 0.8f ? 1.02f : 0.85f;
        } else if (zone < 0.69f) {
            ringRadius = radius * (1.95f + Random01(rng) * 0.075f);
            UnpackColor(0x050505U, red, green, blue);
            size = 0.3f;
            alpha = 0.1f;
        } else if (zone < 0.99f) {
            ringRadius = radius * (2.025f + Random01(rng) * 0.245f);
            UnpackColor(0x989085U, red, green, blue);
            size = 0.7f;
            alpha = ringRadius > radius * 2.2f && ringRadius < radius * 2.21f ? 0.1f : 0.6f;
        } else {
            ringRadius = radius * (2.32f + Random01(rng) * 0.02f);
            UnpackColor(0xAFAFA0U, red, green, blue);
            alpha = 0.7f;
        }
        const float theta = Random01(rng) * 6.28318f;
        particle.position[0] = ringRadius * std::cos(theta);
        particle.position[1] = (Random01(rng) - 0.5f) * (ringRadius > radius * 2.3f ? 0.4f : 0.15f);
        particle.position[2] = ringRadius * std::sin(theta);
        particle.position[3] = size;
        particle.speed = 8.0f / std::sqrt(ringRadius);
        particle.isRing = 1U;
    }
    particle.color = PackColor(red, green, blue, alpha);
    return particle;
}

GLuint CompileShader(GLenum stage, const char* path) {
    std::ifstream stream{path};
    std::stringstream source;
    source << stream.rdbuf();
    const std::string text = source.str();
    if (text.empty()) return 0;
    const char* data = text.c_str();
    GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &data, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) { glDeleteShader(shader); return 0; }
    return shader;
}

GLuint BuildTransformFeedbackProgram(const char* path) {
    GLuint shader = CompileShader(GL_VERTEX_SHADER, path);
    if (shader == 0) return 0;
    GLuint program = glCreateProgram();
    glAttachShader(program, shader);
    const char* varyings[] = {"tfPosition", "tfColor", "tfSpeed", "tfIsRing", "tfPadding"};
    glTransformFeedbackVaryings(program, 5, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(program);
    glDeleteShader(shader);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) { glDeleteProgram(program); return 0; }
    return program;
}

GLuint BuildRenderProgram(const char* vertexPath, const char* fragmentPath) {
    GLuint vertex = CompileShader(GL_VERTEX_SHADER, vertexPath);
    GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, fragmentPath);
    if (vertex == 0 || fragment == 0) {
        if (vertex != 0) glDeleteShader(vertex);
        if (fragment != 0) glDeleteShader(fragment);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_TRUE) return program;
    glDeleteProgram(program);
    return 0;
}

void ConfigureVertexArray(GLuint vao, GLuint buffer) {
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    using Particle = OpenGLParticleSystem::ParticleSnapshot;
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, ParticleBytes,
                                                         reinterpret_cast<void*>(offsetof(Particle, position)));
    glEnableVertexAttribArray(1); glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, ParticleBytes,
                                                          reinterpret_cast<void*>(offsetof(Particle, color)));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, ParticleBytes,
                                                         reinterpret_cast<void*>(offsetof(Particle, speed)));
    glEnableVertexAttribArray(3); glVertexAttribIPointer(3, 1, GL_UNSIGNED_INT, ParticleBytes,
                                                          reinterpret_cast<void*>(offsetof(Particle, isRing)));
    glEnableVertexAttribArray(4); glVertexAttribIPointer(4, 1, GL_UNSIGNED_INT, ParticleBytes,
                                                          reinterpret_cast<void*>(offsetof(Particle, padding)));
}

} // namespace

OpenGLParticleSystem::~OpenGLParticleSystem() {
    if (transformFeedback_ != 0) glDeleteTransformFeedbacks(1, &transformFeedback_);
    if (analyticVertexArray_ != 0) glDeleteVertexArrays(1, &analyticVertexArray_);
    if (analyticBuffer_ != 0) glDeleteBuffers(1, &analyticBuffer_);
    if (indirectBuffer_ != 0) glDeleteBuffers(1, &indirectBuffer_);
    glDeleteVertexArrays(3, vertexArrays_);
    glDeleteBuffers(3, buffers_);
    if (program_ != 0) glDeleteProgram(program_);
    if (renderProgram_ != 0) glDeleteProgram(renderProgram_);
}

bool OpenGLParticleSystem::Initialize(const char* transformFeedbackVertexShader, const char* renderVertexShader,
                                      const char* renderFragmentShader, std::uint32_t seed) {
    program_ = BuildTransformFeedbackProgram(transformFeedbackVertexShader);
    renderProgram_ = BuildRenderProgram(renderVertexShader, renderFragmentShader);
    if (program_ == 0 || renderProgram_ == 0) return false;
    deltaTimeLocation_ = glGetUniformLocation(program_, "uDeltaTime");
    handScaleLocation_ = glGetUniformLocation(program_, "uHandScale");
    handTrackedLocation_ = glGetUniformLocation(program_, "uHandTracked");
    // 与 DrawIndirect 中 setFloat 的取值顺序一一对应。
    static constexpr const char* renderUniformNames[RenderUniformCount] = {
        "uTime", "uScale", "uRotationX", "uRotationY", "uAspect",
        "uScreenHeight", "uPixelRatio", "uDensityCompensation", "uAnalyticPhase"};
    for (std::uint32_t index = 0; index < RenderUniformCount; ++index)
        renderUniformLocations_[index] = glGetUniformLocation(renderProgram_, renderUniformNames[index]);
    glGenBuffers(3, buffers_);
    glGenVertexArrays(3, vertexArrays_);
    std::vector<ParticleSnapshot> initialParticles(ParticleCount);
    for (std::uint32_t index = 0; index < ParticleCount; ++index) initialParticles[index] = InitializeDiligentParticle(index, seed);
    for (std::uint32_t index = 0; index < 3; ++index) {
        glBindBuffer(GL_ARRAY_BUFFER, buffers_[index]);
        glBufferData(GL_ARRAY_BUFFER, ParticleCount * ParticleBytes, initialParticles.data(), GL_DYNAMIC_COPY);
        ConfigureVertexArray(vertexArrays_[index], buffers_[index]);
    }
    glGenBuffers(1, &analyticBuffer_);
    glGenVertexArrays(1, &analyticVertexArray_);
    glBindBuffer(GL_ARRAY_BUFFER, analyticBuffer_);
    glBufferData(GL_ARRAY_BUFFER, ParticleCount * ParticleBytes, initialParticles.data(), GL_STATIC_DRAW);
    ConfigureVertexArray(analyticVertexArray_, analyticBuffer_);
    glGenTransformFeedbacks(1, &transformFeedback_);
    // Match the Diligent vertex-pulling path: six vertices form one particle
    // quad and the indirect instance count selects the active particles.
    const DrawArraysIndirectCommand draw{6, ParticleCount, 0, 0};
    glGenBuffers(1, &indirectBuffer_);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirectBuffer_);
    glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(draw), &draw, GL_STATIC_DRAW);
    return glGetError() == GL_NO_ERROR;
}

void OpenGLParticleSystem::Simulate(float deltaTime, float handScale, bool handTracked) {
    if (simulationMode_ == SimulationMode::Analytic) {
        analyticPhase_ += deltaTime * (handTracked ? handScale : 1.0f);
        return;
    }
    glUseProgram(program_);
    glUniform1f(deltaTimeLocation_, deltaTime);
    glUniform1f(handScaleLocation_, handScale);
    glUniform1f(handTrackedLocation_, handTracked ? 1.0f : 0.0f);
    glEnable(GL_RASTERIZER_DISCARD);
    glBindVertexArray(vertexArrays_[readIndex_]);
    glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, transformFeedback_);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffers_[writeIndex_]);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, ParticleCount);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    glFlush();
    const auto previousRender = renderIndex_;
    renderIndex_ = readIndex_;
    readIndex_ = writeIndex_;
    writeIndex_ = previousRender;
}

void OpenGLParticleSystem::SetSimulationMode(SimulationMode mode) noexcept { simulationMode_ = mode; }
OpenGLParticleSystem::SimulationMode OpenGLParticleSystem::GetSimulationMode() const noexcept { return simulationMode_; }

bool OpenGLParticleSystem::ReadBack(std::vector<ParticleSnapshot>& particles, std::uint32_t count) const {
    const std::uint32_t source = simulationMode_ == SimulationMode::Analytic ? analyticBuffer_ : buffers_[renderIndex_];
    if (count == 0 || count > ParticleCount || source == 0) return false;
    particles.resize(count);
    glBindBuffer(GL_ARRAY_BUFFER, source);
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(count) * ParticleBytes, particles.data());
    if (simulationMode_ == SimulationMode::Analytic) {
        for (auto& particle : particles) {
            const float angle = (particle.isRing == 0U ? 0.03f : particle.speed * 0.2f) * analyticPhase_;
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            const float x = particle.position[0];
            particle.position[0] = x * c - particle.position[2] * s;
            particle.position[2] = x * s + particle.position[2] * c;
        }
    }
    return glGetError() == GL_NO_ERROR;
}

std::uint32_t OpenGLParticleSystem::RenderVertexArray() const noexcept { return vertexArrays_[renderIndex_]; }
std::uint32_t OpenGLParticleSystem::IndirectBuffer() const noexcept { return indirectBuffer_; }

void OpenGLParticleSystem::DrawIndirect(float timeSeconds, std::uint32_t width, std::uint32_t height, float scale,
                                        float rotationX, float rotationY, float pixelRatio,
                                        float densityCompensation, std::uint32_t particleCount) const {
    glUseProgram(renderProgram_);
    // 顺序与 Initialize 中 renderUniformNames 一致。
    const float renderUniformValues[RenderUniformCount] = {
        timeSeconds, scale, rotationX, rotationY,
        static_cast<float>(std::max(width, 1U)) / static_cast<float>(std::max(height, 1U)),
        static_cast<float>(std::max(height, 1U)), pixelRatio, densityCompensation,
        simulationMode_ == SimulationMode::Analytic ? analyticPhase_ : 0.0f};
    for (std::uint32_t index = 0; index < RenderUniformCount; ++index) {
        if (renderUniformLocations_[index] >= 0) glUniform1f(renderUniformLocations_[index], renderUniformValues[index]);
    }
    glBindVertexArray(simulationMode_ == SimulationMode::Analytic ? analyticVertexArray_ : vertexArrays_[renderIndex_]);
    for (GLuint attribute = 0; attribute < 5; ++attribute) glVertexAttribDivisor(attribute, 1);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirectBuffer_);
    const DrawArraysIndirectCommand draw{6, std::clamp(particleCount, 1U, ParticleCount), 0, 0};
    glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, sizeof(draw), &draw);
    glDrawArraysIndirect(GL_TRIANGLES, nullptr);
    for (GLuint attribute = 0; attribute < 5; ++attribute) glVertexAttribDivisor(attribute, 0);
}

} // namespace ParticleSaturn::Gpu::OpenGL41
