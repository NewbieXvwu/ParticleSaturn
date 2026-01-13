// Renderer.cpp - 渲染器实现
// 将大型函数从头文件移到 cpp 文件，减少编译时间

#include "pch.h"

namespace Renderer {

// 检查 shader 编译状态
static bool CheckShaderCompile(unsigned int shader, const char* type) {
    int success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
        std::cerr << "[Renderer] " << type << " shader compile error: " << infoLog << std::endl;
        return false;
    }
    return true;
}

// 检查 program 链接状态
static bool CheckProgramLink(unsigned int program) {
    int success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
        std::cerr << "[Renderer] Program link error: " << infoLog << std::endl;
        return false;
    }
    return true;
}

// 公开的 shader 编译检查函数 (供外部使用，如 Compute Shader)
bool CheckShaderCompileStatus(unsigned int shader, const char* type) {
    return CheckShaderCompile(shader, type);
}

// 公开的 program 链接检查函数
bool CheckProgramLinkStatus(unsigned int program) {
    return CheckProgramLink(program);
}

// 创建着色器程序，失败时返回 0
unsigned int CreateProgramImpl(const char* vertexSrc, const char* fragmentSrc) {
    unsigned int vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vertexSrc, 0);
    glCompileShader(vs);
    if (!CheckShaderCompile(vs, "Vertex")) {
        glDeleteShader(vs);
        return 0;
    }

    unsigned int fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fragmentSrc, 0);
    glCompileShader(fs);
    if (!CheckShaderCompile(fs, "Fragment")) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }

    unsigned int program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!CheckProgramLink(program)) {
        glDeleteProgram(program);
        return 0;
    }

    return program;
}

} // namespace Renderer
