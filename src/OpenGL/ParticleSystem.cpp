// ParticleSystem.cpp - 粒子系统实现

#include "pch.h"

#include "ParticleSystem.h"
#include "Renderer.h"

namespace ParticleSystem {

// 全局错误信息
std::string g_lastError;

// GPU 粒子初始化 (三缓冲)
bool InitParticlesGPU(DoubleBufferSSBO& db) {
    g_lastError.clear();
    db.ssbo[0] = db.ssbo[1] = db.ssbo[2] = 0;
    db.vao[0] = db.vao[1] = db.vao[2] = 0;
    db.indirectBuffer                 = 0;
    db.renderIdx                      = 0;
    db.readIdx                        = 0;
    db.writeIdx                       = 1;

    // 清除之前的 OpenGL 错误
    while (glGetError() != GL_NO_ERROR) {}

    // 1. 创建三个 SSBO (三缓冲)
    glGenBuffers(3, db.ssbo);
    size_t bufferSize = MAX_PARTICLES * sizeof(GPUParticle);
    for (int i = 0; i < 3; i++) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, db.ssbo[i]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, bufferSize, nullptr, GL_DYNAMIC_DRAW);

        GLenum err = glGetError();
        if (err == GL_OUT_OF_MEMORY) {
            std::ostringstream oss;
            oss << "GL_OUT_OF_MEMORY while allocating SSBO " << i << "\n"
                << "Requested: " << (bufferSize / 1024 / 1024) << " MB per buffer\n"
                << "Total: " << (bufferSize * 3 / 1024 / 1024) << " MB for triple buffering";
            g_lastError = oss.str();
            std::cerr << "[ParticleSystem] " << g_lastError << std::endl;
            glDeleteBuffers(3, db.ssbo);
            db.ssbo[0] = db.ssbo[1] = db.ssbo[2] = 0;
            return false;
        } else if (err != GL_NO_ERROR) {
            std::ostringstream oss;
            oss << "OpenGL error 0x" << std::hex << err << std::dec << " while allocating SSBO " << i;
            g_lastError = oss.str();
            std::cerr << "[ParticleSystem] " << g_lastError << std::endl;
            glDeleteBuffers(3, db.ssbo);
            db.ssbo[0] = db.ssbo[1] = db.ssbo[2] = 0;
            return false;
        }
    }

    // 1.5 创建 Indirect Draw Buffer
    glGenBuffers(1, &db.indirectBuffer);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, db.indirectBuffer);
    DrawArraysIndirectCommand cmd = {MAX_PARTICLES, 1, 0, 0};
    glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(DrawArraysIndirectCommand), &cmd, GL_DYNAMIC_DRAW);

    // 2. 编译初始化 Compute Shader（使用缓存）
    unsigned int pInit = Renderer::CreateComputeProgram(Shaders::ComputeInitSaturn);
    if (pInit == 0) {
        g_lastError = "Init compute shader creation failed";
        std::cerr << "[ParticleSystem] " << g_lastError << std::endl;
        glDeleteBuffers(3, db.ssbo);
        glDeleteBuffers(1, &db.indirectBuffer);
        db.ssbo[0] = db.ssbo[1] = db.ssbo[2] = 0;
        db.indirectBuffer                    = 0;
        return false;
    }

    // 3. 对第一个 SSBO 执行初始化
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, db.ssbo[0]);
    glUseProgram(pInit);
    glUniform1ui(glGetUniformLocation(pInit, "uSeed"), (unsigned int)time(0));
    glUniform1ui(glGetUniformLocation(pInit, "uMaxParticles"), MAX_PARTICLES);
    glDispatchCompute((MAX_PARTICLES + 255) / 256, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);

    // 4. 清理 Shader（不需要删除 cs，CreateComputeProgram 已处理）
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
bool InitParticlesGPU(unsigned int& ssbo, unsigned int& vao) {
    static DoubleBufferSSBO db;
    if (!InitParticlesGPU(db)) {
        return false;
    }
    ssbo = db.ssbo[0];
    vao  = db.vao[0];
    return true;
}

// 创建星空背景
void CreateStars(unsigned int& vao, unsigned int& vbo, int count) {
    std::default_random_engine            gen;
    std::uniform_real_distribution<float> rnd(0, 1);
    std::vector<glm::vec3> cols = {HexToRGB(0xE3DAC5), HexToRGB(0xC9A070), HexToRGB(0xE3DAC5), HexToRGB(0xB08D55)};

    std::vector<float> starData;
    for (int i = 0; i < count; i++) {
        float     r  = 400 + rnd(gen) * 3000;
        float     th = rnd(gen) * 6.28f;
        float     ph = acos(2 * rnd(gen) - 1);
        glm::vec3 c  = cols[i % 4];
        float     seed = rnd(gen);
        starData.insert(starData.end(),
                        {r * sin(ph) * cos(th), r * cos(ph), r * sin(ph) * sin(th), c.x, c.y, c.z, 1 + rnd(gen) * 3, seed});
    }

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, starData.size() * 4, starData.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, 0, 32, 0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, 0, 32, (void*)12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, 0, 32, (void*)24);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, 0, 32, (void*)28);
}

} // namespace ParticleSystem
