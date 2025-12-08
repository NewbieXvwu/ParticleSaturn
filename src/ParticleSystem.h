#pragma once
// 粒子系统 - 粒子初始化和管理

#include "Utils.h"

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

// 初始化粒子（土星本体 + 土星环）
inline void InitParticles(std::vector<GPUParticle>& particles) {
    struct TempParticle {
        float x, y, z, r, g, b, s, o, v, ring;
    };

    std::vector<TempParticle> tempParticles;
    tempParticles.reserve(MAX_PARTICLES);

    std::default_random_engine            gen;
    std::uniform_real_distribution<float> rnd(0, 1);
    std::vector<glm::vec3> cols = {HexToRGB(0xE3DAC5), HexToRGB(0xC9A070), HexToRGB(0xE3DAC5), HexToRGB(0xB08D55)};
    float                  R    = 18.0f;

    for (unsigned int i = 0; i < MAX_PARTICLES; i++) {
        TempParticle p;
        if (i < MAX_PARTICLES * 0.25) {
            // 土星本体粒子
            float th = 6.283f * rnd(gen), ph = acos(2 * rnd(gen) - 1);
            p.x       = R * sin(ph) * cos(th);
            p.y       = R * cos(ph) * 0.9f;
            p.z       = R * sin(ph) * sin(th);
            float lat = (p.y / 0.9f / R + 1) * 0.5f;
            int   ci  = (int)(lat * 4 + cos(lat * 40) * 0.8 + cos(lat * 15) * 0.4) % 4;
            if (ci < 0) {
                ci = 0;
            }
            p.r    = cols[ci].x;
            p.g    = cols[ci].y;
            p.b    = cols[ci].z;
            p.s    = 1 + rnd(gen) * 0.8f;
            p.o    = 0.8f;
            p.v    = 0;
            p.ring = 0;
        } else {
            // 土星环粒子（多层环带）
            float     z = rnd(gen), rad;
            glm::vec3 c;
            if (z < 0.15) {
                rad = R * (1.235f + rnd(gen) * 0.29f);
                c   = HexToRGB(0x2A2520);
                p.s = 0.5f;
                p.o = 0.3f;
            } else if (z < 0.65) {
                float t = rnd(gen);
                rad     = R * (1.525f + t * 0.425f);
                c       = glm::mix(HexToRGB(0xCDBFA0), HexToRGB(0xDCCBBA), t);
                p.s     = 0.8f + rnd(gen) * 0.6f;
                p.o     = 0.85f;
                if (sin(rad * 2) > 0.8) {
                    p.o *= 1.2f;
                }
            } else if (z < 0.69) {
                rad = R * (1.95f + rnd(gen) * 0.075f);
                c   = HexToRGB(0x050505);
                p.s = 0.3f;
                p.o = 0.1f;
            } else if (z < 0.99) {
                rad = R * (2.025f + rnd(gen) * 0.245f);
                c   = HexToRGB(0x989085);
                p.s = 0.7f;
                p.o = 0.6f;
                if (rad > R * 2.2 && rad < R * 2.21) {
                    p.o = 0.1f;
                }
            } else {
                rad = R * (2.32f + rnd(gen) * 0.02f);
                c   = HexToRGB(0xAFAFA0);
                p.s = 1.0f;
                p.o = 0.7f;
            }
            float th = rnd(gen) * 6.283f;
            p.x      = rad * cos(th);
            p.z      = rad * sin(th);
            p.y      = (rnd(gen) - 0.5f) * ((rad > R * 2.3) ? 0.4f : 0.15f);
            p.r      = c.x;
            p.g      = c.y;
            p.b      = c.z;
            p.v      = 8.0f / sqrt(rad);
            p.ring   = 1;
        }
        tempParticles.push_back(p);
    }

    // 打乱粒子顺序以优化渲染
    for (int i = MAX_PARTICLES - 1; i > 0; i--) {
        std::swap(tempParticles[i], tempParticles[std::uniform_int_distribution<int>(0, i)(gen)]);
    }

    // 转换为 GPU 格式
    particles.resize(MAX_PARTICLES);
    for (unsigned int i = 0; i < MAX_PARTICLES; i++) {
        auto& tp     = tempParticles[i];
        particles[i] = {{tp.x, tp.y, tp.z, tp.s}, {tp.r, tp.g, tp.b, tp.o}, {0, 0, 0, tp.v}, tp.ring};
    }
}

// 创建粒子 SSBO 和 VAO
inline void CreateBuffers(unsigned int& ssbo, unsigned int& vao, const std::vector<GPUParticle>& particles) {
    glGenBuffers(1, &ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, particles.size() * sizeof(GPUParticle), particles.data(), GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, ssbo);
    for (int i = 0; i < 4; i++) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, (i == 3 ? 1 : 4), GL_FLOAT, 0, sizeof(GPUParticle), (void*)(i * 16));
    }
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
