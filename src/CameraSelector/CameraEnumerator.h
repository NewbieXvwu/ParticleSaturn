#pragma once
// CameraEnumerator - 使用 DirectShow 枚举系统摄像头
// 返回摄像头列表供用户选择

#ifdef _WIN32

#include <dshow.h>
#include <string>
#include <vector>

#pragma comment(lib, "strmiids.lib")

namespace CameraSelector {

struct CameraInfo {
    int          index;      // 摄像头索引
    std::wstring name;       // 设备名称
    std::wstring devicePath; // 设备路径 (用于唯一标识)
};

// 枚举系统中所有可用的摄像头
// 返回摄像头信息列表
std::vector<CameraInfo> EnumerateCameras();

// 获取摄像头数量
inline int GetCameraCount() {
    return static_cast<int>(EnumerateCameras().size());
}

} // namespace CameraSelector

#endif // _WIN32
