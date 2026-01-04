#pragma once
// CameraSelector - 摄像头选择对话框
// 当存在多个摄像头时，显示实时预览供用户选择

#ifdef _WIN32

#include <Windows.h>
#include <string>

namespace CameraSelector {

// 显示摄像头选择对话框
// 返回用户选择的摄像头索引
// 返回 -1 表示用户取消或没有可用摄像头
// 如果只有一个摄像头，直接返回 0，不显示对话框
//
// rememberChoice: 如果用户勾选"记住选择"，将保存到注册表
// 下次调用时，如果有保存的选择且摄像头仍然存在，直接返回该索引
int ShowCameraSelectorDialog(HINSTANCE hInstance = nullptr);

// 清除保存的摄像头选择
void ClearSavedCameraChoice();

// 获取保存的摄像头索引 (-1 表示没有保存)
int GetSavedCameraChoice();

} // namespace CameraSelector

#endif // _WIN32
