#import <OpenGL/gl3.h>

#include "OpenGLBloom.h"
#include "OpenGLRenderTargets.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

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
    // 生成的单源片段在 bundle 的 single/ 同级目录（".gen.frag" 后缀标识）。
    const std::filesystem::path fragmentPath = std::string_view{fragmentName}.find(".gen.") != std::string_view::npos
        ? directory.parent_path() / "single" / fragmentName
        : directory / fragmentName;
    const std::string fragment = ReadFile(fragmentPath);
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

} // namespace

void OpenGLBloom::DrawPass(unsigned int program, int sourceLocation, unsigned int sourceTexture,
                           unsigned int targetFramebuffer, std::uint32_t targetWidth, std::uint32_t targetHeight,
                           std::uint32_t sourceWidth, std::uint32_t sourceHeight, float offset,
                           float threshold) const {
    glBindFramebuffer(GL_FRAMEBUFFER, targetFramebuffer);
    glViewport(0, 0, targetWidth, targetHeight);
    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sourceTexture);
    glUniform1i(sourceLocation, 0);
    const float constants[8] = {1.0f / sourceWidth, 1.0f / sourceHeight,
                                1.0f / targetWidth, 1.0f / targetHeight,
                                offset, threshold, 0.0f, 0.0f};
    glBindBuffer(GL_UNIFORM_BUFFER, constantsBuffer_);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(constants), constants);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, constantsBuffer_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void OpenGLBloom::CompositePass(unsigned int sourceTexture, unsigned int targetFramebuffer, std::uint32_t width,
                                std::uint32_t height, bool weak) const {
    glBindFramebuffer(GL_FRAMEBUFFER, targetFramebuffer);
    glViewport(0, 0, width, height);
    glUseProgram(acrylicProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sourceTexture);
    glUniform1i(acrylicUniforms_.source, 0);
    if (weak) {
        glUniform3f(acrylicUniforms_.tint, 35.0f / 255.0f, 35.0f / 255.0f, 40.0f / 255.0f);
        glUniform1f(acrylicUniforms_.baseOpacity, 160.0f / 255.0f);
        glUniform1f(acrylicUniforms_.saturation, 1.30f);
        glUniform1f(acrylicUniforms_.adaptive, 0.30f);
    } else {
        glUniform3f(acrylicUniforms_.tint, 20.0f / 255.0f, 20.0f / 255.0f, 25.0f / 255.0f);
        glUniform1f(acrylicUniforms_.baseOpacity, 180.0f / 255.0f);
        glUniform1f(acrylicUniforms_.saturation, 1.35f);
        glUniform1f(acrylicUniforms_.adaptive, 0.35f);
    }
    glUniform1f(acrylicUniforms_.darkMode, 1.0f);
    glUniform1f(acrylicUniforms_.exclusion, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

bool OpenGLBloom::Initialize(const char* shaderDirectory) {
    if (shaderDirectory == nullptr) return false;
    const std::filesystem::path directory{shaderDirectory};
    downsampleProgram_ = BuildProgram(directory, "BloomDownsample.gen.frag");
    blurProgram_ = BuildProgram(directory, "KawaseBlur.gen.frag");
    acrylicProgram_ = BuildProgram(directory, "AcrylicComposite.frag");
    if (downsampleProgram_ == 0 || blurProgram_ == 0 || acrylicProgram_ == 0) return false;
    const auto bindConstants = [](GLuint program) {
        const GLuint blockIndex = glGetUniformBlockIndex(program, "type_BloomConstants");
        if (blockIndex == GL_INVALID_INDEX) return false;
        glUniformBlockBinding(program, blockIndex, 0);
        return true;
    };
    if (!bindConstants(downsampleProgram_) || !bindConstants(blurProgram_)) return false;
    downsampleSourceLocation_ =
        glGetUniformLocation(downsampleProgram_, "SPIRV_Cross_CombinedSourceTextureSPIRV_Cross_DummySampler");
    blurSourceLocation_ =
        glGetUniformLocation(blurProgram_, "SPIRV_Cross_CombinedSourceTextureSPIRV_Cross_DummySampler");
    glGenBuffers(1, &constantsBuffer_);
    glBindBuffer(GL_UNIFORM_BUFFER, constantsBuffer_);
    glBufferData(GL_UNIFORM_BUFFER, 8 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    acrylicUniforms_.source = glGetUniformLocation(acrylicProgram_, "uSource");
    acrylicUniforms_.tint = glGetUniformLocation(acrylicProgram_, "uTint");
    acrylicUniforms_.baseOpacity = glGetUniformLocation(acrylicProgram_, "uBaseOpacity");
    acrylicUniforms_.saturation = glGetUniformLocation(acrylicProgram_, "uSaturation");
    acrylicUniforms_.adaptive = glGetUniformLocation(acrylicProgram_, "uAdaptive");
    acrylicUniforms_.darkMode = glGetUniformLocation(acrylicProgram_, "uDarkMode");
    acrylicUniforms_.exclusion = glGetUniformLocation(acrylicProgram_, "uExclusion");
    return true;
}

bool OpenGLBloom::ApplyUiBlur(const OpenGLRenderTargets& targets, float blurStrength) const {
    if (downsampleProgram_ == 0 || blurProgram_ == 0 || acrylicProgram_ == 0 ||
        targets.Width() == 0 || targets.Height() == 0) return false;
    const std::uint32_t strongWidth = std::max(1U, targets.Width() / 6U);
    const std::uint32_t strongHeight = std::max(1U, targets.Height() / 6U);
    const std::uint32_t weakWidth = std::max(1U, targets.Width() / 12U);
    const std::uint32_t weakHeight = std::max(1U, targets.Height() / 12U);
    DrawPass(downsampleProgram_, downsampleSourceLocation_, targets.ToneMappedTexture(), targets.BloomStrongFramebuffer(), strongWidth, strongHeight,
         targets.Width(), targets.Height(), 0.0f, 0.0f);

    static constexpr float offsets[] = {0.0f, 1.0f, 2.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    const float scale = std::clamp(blurStrength, 0.0f, 5.0f) / 5.0f;
    GLuint source = targets.BloomStrongTexture();
    for (std::size_t index = 1; index < std::size(offsets); ++index) {
        const bool writePingPong = (index % 2U) == 1U;
        const float offset = scale * (offsets[index] + 0.5f) - 0.5f;
        DrawPass(blurProgram_, blurSourceLocation_, source, writePingPong ? targets.BloomPingPongFramebuffer() : targets.BloomStrongFramebuffer(),
             strongWidth, strongHeight, strongWidth, strongHeight, offset, 0.0f);
        source = writePingPong ? targets.BloomPingPongTexture() : targets.BloomStrongTexture();
    }

    DrawPass(downsampleProgram_, downsampleSourceLocation_, targets.BloomPingPongTexture(), targets.BloomWeakFramebuffer(), weakWidth, weakHeight,
         strongWidth, strongHeight, 0.0f, 0.0f);
    static constexpr float weakOffsets[] = {0.5f, 1.0f};
    source = targets.BloomWeakTexture();
    for (std::size_t index = 0; index < std::size(weakOffsets); ++index) {
        const bool writePingPong = index == 0U;
        const float offset = scale * (weakOffsets[index] + 0.5f) - 0.5f;
        DrawPass(blurProgram_, blurSourceLocation_, source, writePingPong ? targets.BloomWeakPingPongFramebuffer() : targets.BloomWeakFramebuffer(),
             weakWidth, weakHeight, weakWidth, weakHeight, offset, 0.0f);
        source = writePingPong ? targets.BloomWeakPingPongTexture() : targets.BloomWeakTexture();
    }

    CompositePass(targets.BloomPingPongTexture(), targets.BloomStrongFramebuffer(),
              strongWidth, strongHeight, false);
    CompositePass(targets.BloomWeakTexture(), targets.BloomWeakPingPongFramebuffer(),
              weakWidth, weakHeight, true);
    return glGetError() == GL_NO_ERROR;
}

bool OpenGLBloom::Apply(const OpenGLRenderTargets& targets, float blurStrength) const {
    if (downsampleProgram_ == 0 || blurProgram_ == 0 || targets.Width() == 0 || targets.Height() == 0) return false;
    const std::uint32_t strongWidth = std::max(1U, targets.Width() / 6U);
    const std::uint32_t strongHeight = std::max(1U, targets.Height() / 6U);
    const std::uint32_t weakWidth = std::max(1U, targets.Width() / 12U);
    const std::uint32_t weakHeight = std::max(1U, targets.Height() / 12U);
    DrawPass(downsampleProgram_, downsampleSourceLocation_, targets.SceneTexture(), targets.BloomStrongFramebuffer(), strongWidth, strongHeight,
         targets.Width(), targets.Height(), 0.0f, 1.0f);

    static constexpr float offsets[] = {0.0f, 1.0f, 2.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    const float scale = std::clamp(blurStrength, 0.0f, 5.0f) / 5.0f;
    GLuint source = targets.BloomStrongTexture();
    for (std::size_t index = 1; index < std::size(offsets); ++index) {
        const bool writePingPong = (index % 2U) == 1U;
        const float offset = scale * (offsets[index] + 0.5f) - 0.5f;
        DrawPass(blurProgram_, blurSourceLocation_, source, writePingPong ? targets.BloomPingPongFramebuffer() : targets.BloomStrongFramebuffer(),
             strongWidth, strongHeight, strongWidth, strongHeight, offset, 0.0f);
        source = writePingPong ? targets.BloomPingPongTexture() : targets.BloomStrongTexture();
    }

    DrawPass(downsampleProgram_, downsampleSourceLocation_, source, targets.BloomWeakFramebuffer(), weakWidth, weakHeight,
         strongWidth, strongHeight, 0.0f, 0.0f);
    source = targets.BloomWeakTexture();
    static constexpr float secondaryOffsets[] = {0.5f, 1.0f};
    for (std::size_t index = 0; index < std::size(secondaryOffsets); ++index) {
        const bool writePingPong = (index % 2U) == 0U;
        DrawPass(blurProgram_, blurSourceLocation_, source, writePingPong ? targets.BloomWeakPingPongFramebuffer() : targets.BloomWeakFramebuffer(),
             weakWidth, weakHeight, weakWidth, weakHeight, secondaryOffsets[index], 0.0f);
        source = writePingPong ? targets.BloomWeakPingPongTexture() : targets.BloomWeakTexture();
    }
    return glGetError() == GL_NO_ERROR;
}

} // namespace ParticleSaturn::Gpu::OpenGL41
