#import <OpenGL/gl3.h>

#include "OpenGLStarField.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace ParticleSaturn::Gpu::OpenGL41 {

namespace {

struct Star {
    float position[3];
    float color[3];
    float size;
    float randomSeed;
};
static_assert(sizeof(Star) == 32);

std::string ReadFile(const char* path) {
    std::ifstream input{path == nullptr ? "" : path};
    std::stringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

GLuint Compile(GLenum stage, const std::string& source) {
    if (source.empty()) return 0;
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

OpenGLStarField::~OpenGLStarField() {
    if (program_ != 0) glDeleteProgram(program_);
    if (vertexArray_ != 0) glDeleteVertexArrays(1, &vertexArray_);
    if (buffer_ != 0) glDeleteBuffers(1, &buffer_);
}

bool OpenGLStarField::Initialize(const char* vertexShaderPath, const char* fragmentShaderPath, std::uint32_t seed) {
    const GLuint vertex = Compile(GL_VERTEX_SHADER, ReadFile(vertexShaderPath));
    const GLuint fragment = Compile(GL_FRAGMENT_SHADER, ReadFile(fragmentShaderPath));
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

    std::mt19937 generator{seed};
    std::uniform_real_distribution<float> random{0.0f, 1.0f};
    constexpr float colors[4][3] = {{0.890f, 0.855f, 0.773f}, {0.788f, 0.627f, 0.439f},
                                    {0.890f, 0.855f, 0.773f}, {0.690f, 0.553f, 0.333f}};
    std::vector<Star> stars(StarCount);
    for (std::uint32_t index = 0; index < StarCount; ++index) {
        const float radius = 400.0f + random(generator) * 3000.0f;
        const float theta = random(generator) * 6.28318530718f;
        const float phi = std::acos(2.0f * random(generator) - 1.0f);
        auto& star = stars[index];
        star.position[0] = radius * std::sin(phi) * std::cos(theta);
        star.position[1] = radius * std::cos(phi);
        star.position[2] = radius * std::sin(phi) * std::sin(theta);
        std::copy_n(colors[index % 4], 3, star.color);
        star.size = 1.0f + random(generator) * 3.0f;
        star.randomSeed = random(generator);
    }
    glGenVertexArrays(1, &vertexArray_);
    glGenBuffers(1, &buffer_);
    glBindVertexArray(vertexArray_);
    glBindBuffer(GL_ARRAY_BUFFER, buffer_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(stars.size() * sizeof(Star)), stars.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Star), nullptr);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Star), reinterpret_cast<void*>(12));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(Star), reinterpret_cast<void*>(24));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Star), reinterpret_cast<void*>(28));
    return glGetError() == GL_NO_ERROR;
}

void OpenGLStarField::Draw(float timeSeconds, std::uint32_t width, std::uint32_t height) const {
    if (program_ == 0 || vertexArray_ == 0 || height == 0) return;
    glUseProgram(program_);
    glUniform1f(glGetUniformLocation(program_, "uTime"), timeSeconds);
    glUniform1f(glGetUniformLocation(program_, "uAspect"), static_cast<float>(width) / static_cast<float>(height));
    glBindVertexArray(vertexArray_);
    glDrawArrays(GL_POINTS, 0, StarCount);
}

} // namespace ParticleSaturn::Gpu::OpenGL41
