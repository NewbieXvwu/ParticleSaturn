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

// 创建着色器程序
unsigned int CreateProgramImpl(const char* vertexSrc, const char* fragmentSrc) {
    unsigned int program = glCreateProgram();
    unsigned int vs      = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vertexSrc, 0);
    glCompileShader(vs);
    CheckShaderCompile(vs, "Vertex");

    unsigned int fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fragmentSrc, 0);
    glCompileShader(fs);
    CheckShaderCompile(fs, "Fragment");

    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    CheckProgramLink(program);

    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

// 生成 FBM 噪声纹理 (用于行星表面)
unsigned int GenerateFBMTextureImpl(int width, int height) {
    // 辅助函数: 2D 哈希噪声
    auto hash = [](float x, float y) -> float {
        return fmodf(sinf(x * 12.9898f + y * 78.233f) * 43758.5453f, 1.0f);
    };

    // 辅助函数: 平滑插值噪声
    auto noise = [&](float x, float y) -> float {
        int   ix = (int)floorf(x);
        int   iy = (int)floorf(y);
        float fx = x - ix;
        float fy = y - iy;
        float ux = fx * fx * (3.0f - 2.0f * fx);
        float uy = fy * fy * (3.0f - 2.0f * fy);

        float a = hash((float)ix, (float)iy);
        float b = hash((float)(ix + 1), (float)iy);
        float c = hash((float)ix, (float)(iy + 1));
        float d = hash((float)(ix + 1), (float)(iy + 1));

        return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
    };

    // FBM: 5 层噪声叠加
    auto fbm = [&](float x, float y) -> float {
        float value = 0.0f;
        float amp   = 0.5f;
        for (int i = 0; i < 5; i++) {
            value += amp * noise(x, y);
            x *= 2.0f;
            y *= 2.0f;
            amp *= 0.5f;
        }
        return value;
    };

    std::vector<unsigned char> data(width * height);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float u     = (float)x / width * 16.0f;
            float v     = (float)y / height * 16.0f;
            float value = fbm(u, v);
            data[y * width + x] = (unsigned char)(value * 255.0f);
        }
    }

    unsigned int tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, data.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glGenerateMipmap(GL_TEXTURE_2D);
    return tex;
}

} // namespace Renderer
