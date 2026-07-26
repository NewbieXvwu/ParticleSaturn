#import <OpenGL/gl3.h>

#include "OpenGLParticleSystem.h"

#include "shaders/abi/ParticleInit.h"

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
    for (std::uint32_t index = 0; index < ParticleCount; ++index) initialParticles[index] = ShaderAbi::InitializeDiligentParticle(index, seed);
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
