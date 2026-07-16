#import <OpenGL/gl3.h>

#include "OpenGLParticleSystem.h"

#include <fstream>
#include <sstream>

namespace ParticleSaturn::Gpu::OpenGL41 {

namespace {

constexpr GLsizeiptr ParticleBytes = 32;

struct DrawArraysIndirectCommand {
    GLuint count;
    GLuint instanceCount;
    GLuint first;
    GLuint baseInstance;
};

GLuint BuildProgram(const char* path) {
    std::ifstream stream{path};
    std::stringstream source;
    source << stream.rdbuf();
    const std::string text = source.str();
    const char* data = text.c_str();
    GLuint shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(shader, 1, &data, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) { glDeleteShader(shader); return 0; }
    GLuint program = glCreateProgram();
    glAttachShader(program, shader);
    const char* varyings[] = {"tfPosition", "tfColor", "tfSpeed", "tfIsRing"};
    glTransformFeedbackVaryings(program, 4, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(program);
    glDeleteShader(shader);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) { glDeleteProgram(program); return 0; }
    return program;
}

void ConfigureVertexArray(GLuint vao, GLuint buffer) {
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, ParticleBytes, nullptr);
    glEnableVertexAttribArray(1); glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, ParticleBytes, reinterpret_cast<void*>(16));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, ParticleBytes, reinterpret_cast<void*>(20));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, ParticleBytes, reinterpret_cast<void*>(24));
}

} // namespace

bool OpenGLParticleSystem::Initialize(const char* transformFeedbackVertexShader) {
    program_ = BuildProgram(transformFeedbackVertexShader);
    if (program_ == 0) return false;
    glGenBuffers(3, buffers_);
    glGenVertexArrays(3, vertexArrays_);
    for (std::uint32_t index = 0; index < 3; ++index) {
        glBindBuffer(GL_ARRAY_BUFFER, buffers_[index]);
        glBufferData(GL_ARRAY_BUFFER, ParticleCount * ParticleBytes, nullptr, GL_DYNAMIC_COPY);
        ConfigureVertexArray(vertexArrays_[index], buffers_[index]);
    }
    glGenTransformFeedbacks(1, &transformFeedback_);
    const DrawArraysIndirectCommand draw{ParticleCount, 1, 0, 0};
    glGenBuffers(1, &indirectBuffer_);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirectBuffer_);
    glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(draw), &draw, GL_STATIC_DRAW);
    return glGetError() == GL_NO_ERROR;
}

void OpenGLParticleSystem::Simulate(float deltaTime, float handScale, bool handTracked) {
    glUseProgram(program_);
    glUniform1f(glGetUniformLocation(program_, "uDeltaTime"), deltaTime);
    glUniform1f(glGetUniformLocation(program_, "uHandScale"), handScale);
    glUniform1f(glGetUniformLocation(program_, "uHandTracked"), handTracked ? 1.0f : 0.0f);
    glEnable(GL_RASTERIZER_DISCARD);
    glBindVertexArray(vertexArrays_[readIndex_]);
    glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, transformFeedback_);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffers_[writeIndex_]);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, ParticleCount);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    const auto previousRender = renderIndex_;
    renderIndex_ = readIndex_;
    readIndex_ = writeIndex_;
    writeIndex_ = previousRender;
}

std::uint32_t OpenGLParticleSystem::RenderVertexArray() const noexcept { return vertexArrays_[renderIndex_]; }
std::uint32_t OpenGLParticleSystem::IndirectBuffer() const noexcept { return indirectBuffer_; }

void OpenGLParticleSystem::DrawIndirect() const {
    glBindVertexArray(vertexArrays_[renderIndex_]);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirectBuffer_);
    glDrawArraysIndirect(GL_POINTS, nullptr);
}

} // namespace ParticleSaturn::Gpu::OpenGL41
