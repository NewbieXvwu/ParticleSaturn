#import <OpenGL/gl3.h>

#include "OpenGLSevenSegmentFps.h"

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
    const GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;
    glDeleteShader(shader);
    return 0;
}

} // namespace

OpenGLSevenSegmentFps::~OpenGLSevenSegmentFps() {
    if (vertexArray_ != 0) glDeleteVertexArrays(1, &vertexArray_);
    if (program_ != 0) glDeleteProgram(program_);
}

bool OpenGLSevenSegmentFps::Initialize(const char* shaderDirectory) {
    if (shaderDirectory == nullptr) return false;
    const std::filesystem::path directory{shaderDirectory};
    const GLuint vertex = Compile(GL_VERTEX_SHADER, ReadFile(directory / "FullscreenTriangle.vert"));
    const GLuint fragment = Compile(GL_FRAGMENT_SHADER, ReadFile(directory / "SevenSegment.frag"));
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
    if (linked != GL_TRUE) return false;
    glGenVertexArrays(1, &vertexArray_);
    framesPerSecondLocation_ = glGetUniformLocation(program_, "uFramesPerSecond");
    outputSizeLocation_ = glGetUniformLocation(program_, "uOutputSize");
    return vertexArray_ != 0 && glGetError() == GL_NO_ERROR;
}

bool OpenGLSevenSegmentFps::Render(std::uint32_t framebuffer, std::uint32_t width, std::uint32_t height,
                                   std::uint32_t framesPerSecond) const {
    if (program_ == 0 || vertexArray_ == 0 || width == 0 || height == 0 || framesPerSecond > 999) return false;
    const std::uint32_t digitCount = framesPerSecond >= 100 ? 3U : (framesPerSecond >= 10 ? 2U : 1U);
    const std::uint32_t left = 60U + (digitCount - 1U) * 30U;
    if (width < left || height < 40U) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, width, height);
    glUseProgram(program_);
    glUniform1ui(framesPerSecondLocation_, framesPerSecond);
    glUniform2ui(outputSizeLocation_, width, height);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST);
    glScissor(static_cast<GLint>(width - left), static_cast<GLint>(height - 40U),
              static_cast<GLsizei>(20U + (digitCount - 1U) * 30U), 36);
    glBindVertexArray(vertexArray_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    return glGetError() == GL_NO_ERROR;
}

} // namespace ParticleSaturn::Gpu::OpenGL41
