#import <OpenGL/gl3.h>

#include "OpenGLBloom.h"
#include "OpenGLRenderTargets.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace ParticleSaturn::Gpu::OpenGL41 {

namespace {

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input{path};
    std::stringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

GLuint Compile(GLenum stage, const std::string& source) {
    const char* text = source.c_str();
    GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;
    glDeleteShader(shader);
    return 0;
}

GLuint BuildProgram(const std::filesystem::path& directory, const char* fragmentName) {
    const std::string vertex = ReadFile(directory / "FullscreenTriangle.vert");
    const std::string fragment = ReadFile(directory / fragmentName);
    if (vertex.empty() || fragment.empty()) return 0;
    GLuint vertexShader = Compile(GL_VERTEX_SHADER, vertex);
    GLuint fragmentShader = Compile(GL_FRAGMENT_SHADER, fragment);
    if (vertexShader == 0 || fragmentShader == 0) {
        if (vertexShader != 0) glDeleteShader(vertexShader);
        if (fragmentShader != 0) glDeleteShader(fragmentShader);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_TRUE) return program;
    glDeleteProgram(program);
    return 0;
}

void Draw(GLuint program, GLuint sourceTexture, GLuint targetFramebuffer, std::uint32_t targetWidth,
          std::uint32_t targetHeight, std::uint32_t sourceWidth, std::uint32_t sourceHeight, float offset, float threshold) {
    glBindFramebuffer(GL_FRAMEBUFFER, targetFramebuffer);
    glViewport(0, 0, targetWidth, targetHeight);
    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sourceTexture);
    glUniform1i(glGetUniformLocation(program, "uSource"), 0);
    glUniform2f(glGetUniformLocation(program, "uTexelSize"), 1.0f / sourceWidth, 1.0f / sourceHeight);
    glUniform1f(glGetUniformLocation(program, "uOffset"), offset);
    glUniform1f(glGetUniformLocation(program, "uThreshold"), threshold);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

} // namespace

bool OpenGLBloom::Initialize(const char* shaderDirectory) {
    if (shaderDirectory == nullptr) return false;
    const std::filesystem::path directory{shaderDirectory};
    downsampleProgram_ = BuildProgram(directory, "BloomDownsample.frag");
    blurProgram_ = BuildProgram(directory, "KawaseBlur.frag");
    return downsampleProgram_ != 0 && blurProgram_ != 0;
}

bool OpenGLBloom::Apply(const OpenGLRenderTargets& targets) const {
    if (downsampleProgram_ == 0 || blurProgram_ == 0 || targets.Width() == 0 || targets.Height() == 0) return false;
    const std::uint32_t strongWidth = std::max(1U, targets.Width() / 6U);
    const std::uint32_t strongHeight = std::max(1U, targets.Height() / 6U);
    const std::uint32_t weakWidth = std::max(1U, targets.Width() / 12U);
    const std::uint32_t weakHeight = std::max(1U, targets.Height() / 12U);
    Draw(downsampleProgram_, targets.SceneTexture(), targets.BloomStrongFramebuffer(), strongWidth, strongHeight,
         targets.Width(), targets.Height(), 0.0f, 1.0f);

    static constexpr float offsets[] = {1.0f, 2.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    GLuint source = targets.BloomStrongTexture();
    for (std::size_t index = 0; index < std::size(offsets); ++index) {
        const bool writePingPong = (index % 2U) == 0U;
        Draw(blurProgram_, source, writePingPong ? targets.BloomPingPongFramebuffer() : targets.BloomStrongFramebuffer(),
             strongWidth, strongHeight, strongWidth, strongHeight, offsets[index], 0.0f);
        source = writePingPong ? targets.BloomPingPongTexture() : targets.BloomStrongTexture();
    }

    Draw(downsampleProgram_, source, targets.BloomWeakFramebuffer(), weakWidth, weakHeight,
         strongWidth, strongHeight, 0.0f, 0.0f);
    source = targets.BloomWeakTexture();
    static constexpr float secondaryOffsets[] = {0.5f, 1.0f};
    for (std::size_t index = 0; index < std::size(secondaryOffsets); ++index) {
        const bool writePingPong = (index % 2U) == 0U;
        Draw(blurProgram_, source, writePingPong ? targets.BloomWeakPingPongFramebuffer() : targets.BloomWeakFramebuffer(),
             weakWidth, weakHeight, weakWidth, weakHeight, secondaryOffsets[index], 0.0f);
        source = writePingPong ? targets.BloomWeakPingPongTexture() : targets.BloomWeakTexture();
    }
    return glGetError() == GL_NO_ERROR;
}

} // namespace ParticleSaturn::Gpu::OpenGL41
