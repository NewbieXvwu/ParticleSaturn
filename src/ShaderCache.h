#pragma once
// ShaderCache.h - 着色器缓存工具
// 提供跨 OpenGL/Diligent 版本的缓存路径管理和版本控制

#include <cstdint>
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
    uint32_t magic    = kCacheMagic;
    uint32_t version  = kCacheVersion;
    uint32_t dataSize = 0;
    uint32_t reserved = 0;
};

// 获取缓存目录路径
// 返回 %LOCALAPPDATA%\ParticleSaturn\ (宽字符)
inline std::wstring GetCacheDirectory() {
    wchar_t* localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        std::wstring cachePath = localAppData;
        CoTaskMemFree(localAppData);
        cachePath += L"\\ParticleSaturn";

        // 确保目录存在
        CreateDirectoryW(cachePath.c_str(), nullptr);
        return cachePath;
    }
    return {};
}

// 获取 OpenGL 着色器缓存路径
inline std::wstring GetOpenGLCachePath() {
    std::wstring dir = GetCacheDirectory();
    if (dir.empty()) {
        return {};
    }
    return dir + L"\\shader_cache_opengl.bin";
}

// 获取 Diligent 缓存路径（按后端区分）
inline std::wstring GetDiligentCachePath(const char* backend) {
    std::wstring dir = GetCacheDirectory();
    if (dir.empty()) {
        return {};
    }

    // 转换 backend 名称为宽字符
    std::wstring filename = L"\\shader_cache_";
    while (*backend) {
        filename += static_cast<wchar_t>(*backend++);
    }
    filename += L".bin";
    return dir + filename;
}

// 读取缓存文件
// 返回 true 并填充 outData（不含头部），如果缓存有效
// 返回 false 如果缓存不存在、版本不匹配或损坏
inline bool ReadCache(const std::wstring& path, std::vector<uint8_t>& outData) {
    if (path.empty()) {
        return false;
    }

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
inline bool WriteCache(const std::wstring& path, const void* data, size_t size) {
    if (path.empty() || data == nullptr || size == 0) {
        return false;
    }
    if (size > static_cast<size_t>(UINT32_MAX)) {
        return false;
    }

    // 原子写入：先写临时文件，再替换目标文件，避免异常退出留下半截缓存导致后续读崩/读错。
    const std::wstring tmpPath = path + L".tmp";

    std::ofstream file(tmpPath, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }

    CacheHeader header;
    header.dataSize = static_cast<uint32_t>(size);

    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(data), size);

    file.flush();
    if (!file.good()) {
        file.close();
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    file.close();

    // MOVEFILE_WRITE_THROUGH：尽可能确保落盘，减少断电/崩溃导致的“0字节目标文件”。
    if (!MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        // 部分环境下 REPLACE_EXISTING 可能因为权限/杀软占用失败：尝试先删再移。
        DeleteFileW(path.c_str());
        if (!MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(tmpPath.c_str());
            return false;
        }
    }

    return true;
}

// 删除缓存文件
inline void InvalidateCache(const std::wstring& path) {
    if (!path.empty()) {
        DeleteFileW(path.c_str());
    }
}

} // namespace ShaderCache
