#pragma once
// 粒子系统 - 粒子初始化和管理

#include <ctime>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "Shaders.h"
#include "Utils.h"

const unsigned int MAX_PARTICLES = 1200000;
const unsigned int MIN_PARTICLES = 200000;
const unsigned int STAR_COUNT    = 50000;

// GPU 粒子数据结构 (优化: 32字节，从48字节减少33%)
struct GPUParticle {
    glm::vec4 pos;    // x, y, z, scale (16 字节)
    uint32_t  color;  // RGBA8 打包颜色 (4 字节)
    float     speed;  // 轨道速度 (4 字节)
    float     isRing; // 0=本体, 1=环 (4 字节)
    float     pad;    // 对齐到 32 字节 (4 字节)
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
        renderIdx     = readIdx;   // 上一帧计算完成的数据变为渲染数据
        readIdx       = writeIdx;  // 上一帧写入的变为下一帧读取
        writeIdx      = oldRender; // 渲染完的缓冲变为下一帧写入目标
    }
};

namespace ParticleSystem {

// 全局错误信息（用于向调用者传递详细错误原因）
extern std::string g_lastError;

// GPU 粒子初始化 (三缓冲)，返回是否成功
bool InitParticlesGPU(DoubleBufferSSBO& db);

// 兼容旧接口 (内部使用静态双缓冲)
bool InitParticlesGPU(unsigned int& ssbo, unsigned int& vao);

// 创建星空背景
void CreateStars(unsigned int& vao, unsigned int& vbo, int count = STAR_COUNT);

} // namespace ParticleSystem
