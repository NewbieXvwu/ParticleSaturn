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

} // namespace

bool OpenGLToneMapper::Initialize(const char* shaderDirectory) {
    if (shaderDirectory == nullptr) return false;
    const std::filesystem::path directory{shaderDirectory};
    const std::string vertexSource = ReadFile(directory / "FullscreenTriangle.vert");
    const std::string fragmentSource = ReadFile(directory / "ToneMap.frag");
    GLuint vertex = Compile(GL_VERTEX_SHADER, vertexSource);
    GLuint fragment = Compile(GL_FRAGMENT_SHADER, fragmentSource);
    if (vertex == 0 || fragment == 0) {
        if (vertex != 0) glDeleteShader(vertex);
        if (fragment != 0) glDeleteShader(fragment);
        return false;
    }
    program_ = glCreateProgram();
    glAttachShader(program_, vertex);
    glAttachShader(program_, fragment);
    glLinkProgram(program_);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (linked == GL_TRUE) return true;
    glDeleteProgram(program_);
    program_ = 0;
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

} // namespace ParticleSaturn::Gpu::OpenGL41
