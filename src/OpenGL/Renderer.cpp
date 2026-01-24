// Renderer.cpp - 渲染器实现
// 将大型函数从头文件移到 cpp 文件，减少编译时间

#include "pch.h"

#include <unordered_map>

#include "../ShaderCache.h"

namespace Renderer {

// ============================================================================
// 着色器二进制缓存系统
// ============================================================================

// 缓存条目：存储单个程序的二进制数据
struct ProgramCacheEntry {
    uint32_t             keyHash = 0; // 着色器源码哈希
    GLenum               format  = 0; // 二进制格式
    std::vector<uint8_t> binary;      // 二进制数据
};

// 缓存文件格式：
// [CacheHeader] (来自 ShaderCache.h)
// [uint32_t entryCount]
// [ProgramCacheEntry entries...]
//   每个 entry: [keyHash:4][format:4][binarySize:4][binary:binarySize]

// 全局缓存
static std::unordered_map<uint32_t, ProgramCacheEntry> g_programCache;
static bool                                            g_cacheLoaded = false;
static bool                                            g_cacheDirty  = false;

// 简单的字符串哈希（FNV-1a）
static uint32_t HashString(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= static_cast<uint8_t>(*str++);
        hash *= 16777619u;
    }
    return hash;
}

// 计算两个着色器源码的组合哈希
static uint32_t HashShaderSources(const char* vs, const char* fs) {
    uint32_t h1 = HashString(vs);
    uint32_t h2 = HashString(fs);
    return h1 ^ (h2 * 31);
}

// 加载缓存
static void LoadCache() {
    if (g_cacheLoaded) {
        return;
    }
    g_cacheLoaded = true;

    auto cachePath = ShaderCache::GetOpenGLCachePath();
    if (cachePath.empty()) {
        return;
    }

    std::vector<uint8_t> data;
    if (!ShaderCache::ReadCache(cachePath, data)) {
        return;
    }

    if (data.size() < sizeof(uint32_t)) {
        return;
    }

    const uint8_t* ptr = data.data();
    const uint8_t* end = ptr + data.size();

    uint32_t entryCount = *reinterpret_cast<const uint32_t*>(ptr);
    ptr += sizeof(uint32_t);

    for (uint32_t i = 0; i < entryCount && ptr + 12 <= end; ++i) {
        ProgramCacheEntry entry;
        entry.keyHash = *reinterpret_cast<const uint32_t*>(ptr);
        ptr += 4;
        entry.format = *reinterpret_cast<const uint32_t*>(ptr);
        ptr += 4;
        uint32_t binSize = *reinterpret_cast<const uint32_t*>(ptr);
        ptr += 4;

        if (ptr + binSize > end) {
            break;
        }

        entry.binary.assign(ptr, ptr + binSize);
        ptr += binSize;

        g_programCache[entry.keyHash] = std::move(entry);
    }

    std::cerr << "[Renderer] Loaded " << g_programCache.size() << " cached shader programs" << std::endl;
}

// 保存缓存
static void SaveCache() {
    if (!g_cacheDirty) {
        return;
    }

    auto cachePath = ShaderCache::GetOpenGLCachePath();
    if (cachePath.empty()) {
        return;
    }

    std::vector<uint8_t> data;

    // 预留空间
    size_t totalSize = sizeof(uint32_t);
    for (const auto& [key, entry] : g_programCache) {
        totalSize += 12 + entry.binary.size();
    }
    data.reserve(totalSize);

    // 写入条目数
    uint32_t       entryCount = static_cast<uint32_t>(g_programCache.size());
    const uint8_t* countPtr   = reinterpret_cast<const uint8_t*>(&entryCount);
    data.insert(data.end(), countPtr, countPtr + sizeof(uint32_t));

    // 写入每个条目
    for (const auto& [key, entry] : g_programCache) {
        const uint8_t* hashPtr = reinterpret_cast<const uint8_t*>(&entry.keyHash);
        data.insert(data.end(), hashPtr, hashPtr + 4);

        uint32_t       format    = static_cast<uint32_t>(entry.format);
        const uint8_t* formatPtr = reinterpret_cast<const uint8_t*>(&format);
        data.insert(data.end(), formatPtr, formatPtr + 4);

        uint32_t       binSize = static_cast<uint32_t>(entry.binary.size());
        const uint8_t* sizePtr = reinterpret_cast<const uint8_t*>(&binSize);
        data.insert(data.end(), sizePtr, sizePtr + 4);

        data.insert(data.end(), entry.binary.begin(), entry.binary.end());
    }

    if (ShaderCache::WriteCache(cachePath, data.data(), data.size())) {
        std::cerr << "[Renderer] Saved " << g_programCache.size() << " shader programs to cache" << std::endl;
        g_cacheDirty = false;
    }
}

// 尝试从缓存加载程序
static unsigned int TryLoadFromCache(uint32_t keyHash) {
    auto it = g_programCache.find(keyHash);
    if (it == g_programCache.end()) {
        return 0;
    }

    const auto&  entry   = it->second;
    unsigned int program = glCreateProgram();
    glProgramBinary(program, entry.format, entry.binary.data(), static_cast<GLsizei>(entry.binary.size()));

    GLint success = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (success) {
        return program;
    }

    // 加载失败（驱动版本变化等），删除缓存条目
    glDeleteProgram(program);
    g_programCache.erase(it);
    g_cacheDirty = true;
    return 0;
}

// 保存程序到缓存
static void SaveToCache(unsigned int program, uint32_t keyHash) {
    GLint binaryLength = 0;
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &binaryLength);
    if (binaryLength <= 0) {
        return;
    }

    ProgramCacheEntry entry;
    entry.keyHash = keyHash;
    entry.binary.resize(binaryLength);

    GLsizei actualLength = 0;
    glGetProgramBinary(program, binaryLength, &actualLength, &entry.format, entry.binary.data());

    if (actualLength > 0) {
        entry.binary.resize(actualLength);
        g_programCache[keyHash] = std::move(entry);
        g_cacheDirty            = true;
    }
}

// ============================================================================
// 原有函数
// ============================================================================

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
// 支持二进制缓存：首次编译后缓存，后续直接加载
unsigned int CreateProgramImpl(const char* vertexSrc, const char* fragmentSrc) {
    // 确保缓存已加载
    LoadCache();

    // 计算源码哈希
    uint32_t keyHash = HashShaderSources(vertexSrc, fragmentSrc);

    // 尝试从缓存加载
    unsigned int cachedProgram = TryLoadFromCache(keyHash);
    if (cachedProgram != 0) {
        return cachedProgram;
    }

    // 缓存未命中，正常编译
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

    // 启用程序二进制可获取性
    glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);

    glLinkProgram(program);

    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!CheckProgramLink(program)) {
        glDeleteProgram(program);
        return 0;
    }

    // 保存到缓存
    SaveToCache(program, keyHash);

    return program;
}

// 保存所有缓存到磁盘（应在程序退出前调用）
void FlushShaderCache() {
    SaveCache();
}

// 清除着色器缓存
void ClearShaderCache() {
    g_programCache.clear();
    g_cacheDirty = false;

    auto cachePath = ShaderCache::GetOpenGLCachePath();
    if (!cachePath.empty()) {
        ShaderCache::InvalidateCache(cachePath);
    }
}

// 检查缓存是否为空（用于判断是否需要编译）
bool IsCacheEmpty() {
    LoadCache();
    return g_programCache.empty();
}

// 创建 Compute Shader 程序，支持二进制缓存
unsigned int CreateComputeProgramImpl(const char* computeSrc) {
    // 确保缓存已加载
    LoadCache();

    // 计算源码哈希
    uint32_t keyHash = HashString(computeSrc);

    // 尝试从缓存加载
    unsigned int cachedProgram = TryLoadFromCache(keyHash);
    if (cachedProgram != 0) {
        return cachedProgram;
    }

    // 缓存未命中，正常编译
    unsigned int cs = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(cs, 1, &computeSrc, 0);
    glCompileShader(cs);
    if (!CheckShaderCompile(cs, "Compute")) {
        glDeleteShader(cs);
        return 0;
    }

    unsigned int program = glCreateProgram();
    glAttachShader(program, cs);

    // 启用程序二进制可获取性
    glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);

    glLinkProgram(program);

    glDeleteShader(cs);

    if (!CheckProgramLink(program)) {
        glDeleteProgram(program);
        return 0;
    }

    // 保存到缓存
    SaveToCache(program, keyHash);

    return program;
}

} // namespace Renderer
