#pragma once
// 粒子系统 - 粒子初始化和管理

#include "Utils.h"
#include "Shaders.h"
#include <ctime>

const unsigned int MAX_PARTICLES = 1200000;
const unsigned int MIN_PARTICLES = 200000;
const unsigned int STAR_COUNT    = 50000;

// GPU 粒子数据结构 (优化: 32字节，从48字节减少33%)
struct GPUParticle {
    glm::vec4 pos;      // x, y, z, scale (16 字节)
    uint32_t  color;    // RGBA8 打包颜色 (4 字节)
    float     speed;    // 轨道速度 (4 字节)
    float     isRing;   // 0=本体, 1=环 (4 字节)
    float     pad;      // 对齐到 32 字节 (4 字节)
};

// Indirect Draw 命令结构 (符合 glDrawArraysIndirect 规范)
struct DrawArraysIndirectCommand {
    unsigned int count;         // 顶点数量
    unsigned int instanceCount; // 实例数量 (通常为 1)
    unsigned int first;         // 第一个顶点索引
    unsigned int baseInstance;  // 基础实例 (通常为 0)
};

// 三缓冲粒子系统结构 (异步计算调度优化)
// 流水线化：渲染和计算可以更好地重叠执行
// 缓冲 0: 渲染中, 缓冲 1: 计算输入, 缓冲 2: 计算输出
struct DoubleBufferSSBO {
    unsigned int ssbo[3];        // 三个 SSBO
    unsigned int vao[3];         // 对应的三个 VAO
    unsigned int indirectBuffer; // Indirect Draw Buffer
    int          renderIdx;      // 当前用于渲染的缓冲索引
    int          readIdx;        // 当前用于计算读取的缓冲索引
    int          writeIdx;       // 当前用于计算写入的缓冲索引

    // 获取当前用于渲染的 VAO
    unsigned int GetRenderVAO() const { return vao[renderIdx]; }

    // 获取当前用于读取的 SSBO (计算着色器输入)
    unsigned int GetReadSSBO() const { return ssbo[readIdx]; }

    // 获取当前用于写入的 SSBO (计算着色器输出)
    unsigned int GetWriteSSBO() const { return ssbo[writeIdx]; }

    // 获取 Indirect Draw Buffer
    unsigned int GetIndirectBuffer() const { return indirectBuffer; }

    // 旋转缓冲索引 (三缓冲轮转)
    void Swap() {
        // 轮转: render <- read <- write <- render
        int oldRender = renderIdx;
        renderIdx = readIdx;   // 上一帧计算完成的数据变为渲染数据
        readIdx = writeIdx;    // 上一帧写入的变为下一帧读取
        writeIdx = oldRender;  // 渲染完的缓冲变为下一帧写入目标
    }
};

namespace ParticleSystem {

// GPU 粒子初始化 (三缓冲)，返回是否成功
inline bool InitParticlesGPU(DoubleBufferSSBO& db) {
    db.ssbo[0] = db.ssbo[1] = db.ssbo[2] = 0;
    db.vao[0] = db.vao[1] = db.vao[2] = 0;
    db.indirectBuffer = 0;
    db.renderIdx = 0;
    db.readIdx = 0;
    db.writeIdx = 1;

    // 1. 创建三个 SSBO (三缓冲)
    glGenBuffers(3, db.ssbo);
    for (int i = 0; i < 3; i++) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, db.ssbo[i]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_PARTICLES * sizeof(GPUParticle), nullptr, GL_DYNAMIC_DRAW);
    }

    // 1.5 创建 Indirect Draw Buffer
    glGenBuffers(1, &db.indirectBuffer);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, db.indirectBuffer);
    DrawArraysIndirectCommand cmd = {MAX_PARTICLES, 1, 0, 0};
    glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(DrawArraysIndirectCommand), &cmd, GL_DYNAMIC_DRAW);

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
        glDeleteBuffers(3, db.ssbo);
        db.ssbo[0] = db.ssbo[1] = db.ssbo[2] = 0;
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
        glDeleteBuffers(3, db.ssbo);
        db.ssbo[0] = db.ssbo[1] = db.ssbo[2] = 0;
        return false;
    }

    // 3. 对第一个 SSBO 执行初始化
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, db.ssbo[0]);
    glUseProgram(pInit);
    glUniform1ui(glGetUniformLocation(pInit, "uSeed"), (unsigned int)time(0));
    glUniform1ui(glGetUniformLocation(pInit, "uMaxParticles"), MAX_PARTICLES);
    glDispatchCompute((MAX_PARTICLES + 255) / 256, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);

    // 4. 清理 Shader
    glDeleteShader(cs);
    glDeleteProgram(pInit);

    // 5. 为三个 SSBO 设置 VAO (匹配优化后的数据结构)
    // 结构: vec4 pos(0), uint color(16), float speed(20), float isRing(24), pad(28)
    glGenVertexArrays(3, db.vao);
    for (int i = 0; i < 3; i++) {
        glBindVertexArray(db.vao[i]);
        glBindBuffer(GL_ARRAY_BUFFER, db.ssbo[i]);
        // location 0: pos (vec4, offset 0)
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(GPUParticle), (void*)0);
        // location 1: color (uint RGBA8, offset 16) - 使用 glVertexAttribIPointer 传递整数
        glEnableVertexAttribArray(1);
        glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(GPUParticle), (void*)16);
        // location 2: speed (float, offset 20)
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(GPUParticle), (void*)20);
        // location 3: isRing (float, offset 24)
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(GPUParticle), (void*)24);
    }
    glBindVertexArray(0);

    return true;
}

// 兼容旧接口 (内部使用静态双缓冲)
inline bool InitParticlesGPU(unsigned int& ssbo, unsigned int& vao) {
    static DoubleBufferSSBO db;
    if (!InitParticlesGPU(db)) return false;
    ssbo = db.ssbo[0];
    vao = db.vao[0];
    return true;
}

// 创建星空背景
inline void CreateStars(unsigned int& vao, unsigned int& vbo, int count = STAR_COUNT) {
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
