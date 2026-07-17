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
    const std::string fragmentSource = ReadFile(directory / "ToneMap.frag");
    const std::string presentFragmentSource = ReadFile(directory / "Present.frag");
    program_ = LinkProgram(vertexSource, fragmentSource);
    presentProgram_ = LinkProgram(vertexSource, presentFragmentSource);
    if (program_ != 0 && presentProgram_ != 0) return true;
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
    glUniform1i(glGetUniformLocation(program_, "uScene"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, targets.BloomPingPongTexture());
    glUniform1i(glGetUniformLocation(program_, "uBloom"), 1);
    glUniform1f(glGetUniformLocation(program_, "uBloomStrength"), std::max(0.0f, bloomStrength));
    glUniform1f(glGetUniformLocation(program_, "uTransparent"), transparent ? 1.0f : 0.0f);
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
    }
    glUseProgram(presentProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, targets.ToneMappedTexture());
    glUniform1i(glGetUniformLocation(presentProgram_, "uScene"), 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
    return glGetError() == GL_NO_ERROR;
}

} // namespace ParticleSaturn::Gpu::OpenGL41
