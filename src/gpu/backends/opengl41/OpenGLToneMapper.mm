#import <OpenGL/gl3.h>

#include "OpenGLToneMapper.h"
#include "OpenGLRenderTargets.h"

#include <algorithm>
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

GLuint LinkProgram(const std::string& vertexSource, const std::string& fragmentSource) {
    GLuint vertex = Compile(GL_VERTEX_SHADER, vertexSource);
    GLuint fragment = Compile(GL_FRAGMENT_SHADER, fragmentSource);
    if (vertex == 0 || fragment == 0) {
        if (vertex != 0) glDeleteShader(vertex);
        if (fragment != 0) glDeleteShader(fragment);
        return 0;
    }
    const GLuint program = glCreateProgram();
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

} // namespace

bool OpenGLToneMapper::Initialize(const char* shaderDirectory) {
    if (shaderDirectory == nullptr) return false;
    const std::filesystem::path directory{shaderDirectory};
    const std::string vertexSource = ReadFile(directory / "FullscreenTriangle.vert");
    // 单源翻译产物（D-004 试点）：DXC→SPIR-V→SPIRV-Cross 生成，构建期产出。
    const std::string fragmentSource = ReadFile(directory / "ToneMap.gen.frag");
    const std::string presentFragmentSource = ReadFile(directory / "Present.frag");
    program_ = LinkProgram(vertexSource, fragmentSource);
    presentProgram_ = LinkProgram(vertexSource, presentFragmentSource);
    if (program_ != 0 && presentProgram_ != 0) {
        sceneLocation_ = glGetUniformLocation(program_, "SPIRV_Cross_CombinedSceneTextureSPIRV_Cross_DummySampler");
        bloomLocation_ = glGetUniformLocation(program_, "SPIRV_Cross_CombinedBloomTextureSPIRV_Cross_DummySampler");
        presentSceneLocation_ = glGetUniformLocation(presentProgram_, "uScene");
        const GLuint blockIndex = glGetUniformBlockIndex(program_, "type_ToneMapConstants");
        if (blockIndex == GL_INVALID_INDEX) return false;
        glUniformBlockBinding(program_, blockIndex, 0);
        glGenBuffers(1, &constantsBuffer_);
        glBindBuffer(GL_UNIFORM_BUFFER, constantsBuffer_);
        glBufferData(GL_UNIFORM_BUFFER, 4 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        return glGetError() == GL_NO_ERROR;
    }
    if (program_ != 0) glDeleteProgram(program_);
    if (presentProgram_ != 0) glDeleteProgram(presentProgram_);
    program_ = presentProgram_ = 0;
    return false;
}

bool OpenGLToneMapper::Apply(const OpenGLRenderTargets& targets, float bloomStrength, bool transparent) const {
    if (program_ == 0 || targets.Width() == 0 || targets.Height() == 0) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, targets.ToneMappedFramebuffer());
    glViewport(0, 0, targets.Width(), targets.Height());
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, targets.SceneTexture());
    glUniform1i(sceneLocation_, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, targets.BloomPingPongTexture());
    glUniform1i(bloomLocation_, 1);
    const float constants[4] = {std::max(0.0f, bloomStrength), transparent ? 1.0f : 0.0f, 0.0f, 0.0f};
    glBindBuffer(GL_UNIFORM_BUFFER, constantsBuffer_);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(constants), constants);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, constantsBuffer_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    return glGetError() == GL_NO_ERROR;
}

bool OpenGLToneMapper::Present(const OpenGLRenderTargets& targets, bool transparent) const {
    if (presentProgram_ == 0 || targets.Width() == 0 || targets.Height() == 0) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, targets.Width(), targets.Height());
    if (transparent) {
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        // The tone mapper emits premultiplied pixels in glass mode.  Compositing
        // them preserves the alpha channel required by NSVisualEffectView.
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glUseProgram(presentProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, targets.ToneMappedTexture());
    glUniform1i(presentSceneLocation_, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
    return glGetError() == GL_NO_ERROR;
}

} // namespace ParticleSaturn::Gpu::OpenGL41
