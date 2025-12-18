#pragma once
// 粒子系统 - 粒子初始化和管理

#include "Utils.h"
#include "Shaders.h"
#include <ctime>

const unsigned int MAX_PARTICLES = 1200000;
const unsigned int MIN_PARTICLES = 200000;

// GPU 粒子数据结构
struct GPUParticle {
    glm::vec4 pos;
    glm::vec4 col;
    glm::vec4 vel;
    float     isRing;
    float     pad[3];
};

namespace ParticleSystem {

// GPU 粒子初始化，返回是否成功
inline bool InitParticlesGPU(unsigned int& ssbo, unsigned int& vao) {
    ssbo = 0;
    vao  = 0;

    // 1. 创建并分配 SSBO 内存 (仅分配，不上传数据)
    glGenBuffers(1, &ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_PARTICLES * sizeof(GPUParticle), nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);

    // 2. 编译初始化 Compute Shader
    unsigned int cs = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(cs, 1, &Shaders::ComputeInitSaturn, 0);
    glCompileShader(cs);

    // 检查编译错误
    int  success;
    char infoLog[512];
    glGetShaderiv(cs, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(cs, 512, NULL, infoLog);
        std::cerr << "Init Shader Compilation Failed:\n" << infoLog << std::endl;
        glDeleteShader(cs);
        glDeleteBuffers(1, &ssbo);
        ssbo = 0;
        return false;
    }

    unsigned int pInit = glCreateProgram();
    glAttachShader(pInit, cs);
    glLinkProgram(pInit);

    // 检查链接错误
    glGetProgramiv(pInit, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(pInit, 512, NULL, infoLog);
        std::cerr << "Init Program Linking Failed:\n" << infoLog << std::endl;
        glDeleteShader(cs);
        glDeleteProgram(pInit);
        glDeleteBuffers(1, &ssbo);
        ssbo = 0;
        return false;
    }

    // 3. 执行初始化 Shader
    glUseProgram(pInit);
    glUniform1ui(glGetUniformLocation(pInit, "uSeed"), (unsigned int)time(0));
    glUniform1ui(glGetUniformLocation(pInit, "uMaxParticles"), MAX_PARTICLES);
    glDispatchCompute((MAX_PARTICLES + 255) / 256, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);

    // 4. 清理 Shader
    glDeleteShader(cs);
    glDeleteProgram(pInit);

    // 5. 设置 VAO
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, ssbo);
    for (int i = 0; i < 4; i++) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, (i == 3 ? 1 : 4), GL_FLOAT, 0, sizeof(GPUParticle), (void*)(i * 16));
    }
    glBindVertexArray(0);

    return true;
}

// 创建星空背景
inline void CreateStars(unsigned int& vao, unsigned int& vbo, int count = 50000) {
    std::default_random_engine            gen;
    std::uniform_real_distribution<float> rnd(0, 1);
    std::vector<glm::vec3> cols = {HexToRGB(0xE3DAC5), HexToRGB(0xC9A070), HexToRGB(0xE3DAC5), HexToRGB(0xB08D55)};

    std::vector<float> starData;
    for (int i = 0; i < count; i++) {
        float     r  = 400 + rnd(gen) * 3000;
        float     th = rnd(gen) * 6.28f;
        float     ph = acos(2 * rnd(gen) - 1);
        glm::vec3 c  = cols[i % 4];
        starData.insert(starData.end(),
                        {r * sin(ph) * cos(th), r * cos(ph), r * sin(ph) * sin(th), c.x, c.y, c.z, 1 + rnd(gen) * 3});
    }

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, starData.size() * 4, starData.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, 0, 28, 0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, 0, 28, (void*)12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, 0, 28, (void*)24);
}

} // namespace ParticleSystem
