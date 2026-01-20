#pragma once
// ShaderCache.h - 着色器缓存工具
// 提供跨 OpenGL/Diligent 版本的缓存路径管理和版本控制

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <KnownFolders.h>
#include <ShlObj.h>

namespace ShaderCache {

// 缓存版本号：当着色器源码变更时需要递增此版本
// 这会使旧缓存失效，强制重新编译
static constexpr uint32_t kCacheVersion = 1;

// 缓存文件魔数 "PSCX" (ParticleSaturn Cache X)
static constexpr uint32_t kCacheMagic = 0x58435350;

// 缓存文件头
struct CacheHeader {
    uint32_t magic       = kCacheMagic;
    uint32_t version     = kCacheVersion;
    uint32_t dataSize    = 0;
    uint32_t reserved    = 0;
};

// 获取缓存目录路径
// 返回 %LOCALAPPDATA%\ParticleSaturn\
inline std::filesystem::path GetCacheDirectory() {
    wchar_t* localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        std::filesystem::path cachePath = localAppData;
        CoTaskMemFree(localAppData);
        cachePath /= L"ParticleSaturn";

        // 确保目录存在
        std::error_code ec;
        std::filesystem::create_directories(cachePath, ec);
        if (!ec) {
            return cachePath;
        }
    }
    return {};
}

// 获取 OpenGL 着色器缓存路径
inline std::filesystem::path GetOpenGLCachePath() {
    auto dir = GetCacheDirectory();
    if (dir.empty()) return {};
    return dir / L"shader_cache_opengl.bin";
}

// 获取 Diligent 缓存路径（按后端区分）
inline std::filesystem::path GetDiligentCachePath(const char* backend) {
    auto dir = GetCacheDirectory();
    if (dir.empty()) return {};
    std::string filename = std::string("shader_cache_") + backend + ".bin";
    return dir / filename;
}

// 读取缓存文件
// 返回 true 并填充 outData（不含头部），如果缓存有效
// 返回 false 如果缓存不存在、版本不匹配或损坏
inline bool ReadCache(const std::filesystem::path& path, std::vector<uint8_t>& outData) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    CacheHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || file.gcount() != sizeof(header)) {
        return false;
    }

    // 验证魔数和版本
    if (header.magic != kCacheMagic || header.version != kCacheVersion) {
        return false;
    }

    if (header.dataSize == 0) {
        return false;
    }

    outData.resize(header.dataSize);
    file.read(reinterpret_cast<char*>(outData.data()), header.dataSize);
    if (!file || static_cast<uint32_t>(file.gcount()) != header.dataSize) {
        outData.clear();
        return false;
    }

    return true;
}

// 写入缓存文件
inline bool WriteCache(const std::filesystem::path& path, const void* data, size_t size) {
    if (data == nullptr || size == 0) {
        return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }

    CacheHeader header;
    header.dataSize = static_cast<uint32_t>(size);

    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(data), size);

    return file.good();
}

// 删除缓存文件
inline void InvalidateCache(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace ShaderCache
