#pragma once
// CameraSelector - 摄像头选择对话框
// 当存在多个摄像头时，显示实时预览供用户选择

#ifdef _WIN32

#include <Windows.h>
#include <string>

namespace CameraSelector {

// 显示摄像头选择对话框
// 返回用户选择的摄像头索引
// 返回 -1 表示没有可用摄像头（严重错误）
// 如果只有一个摄像头，直接返回 0，不显示对话框
//
// parentHwnd: 父窗口句柄，关闭父窗口时会自动关闭此对话框
// 行为说明：
// - 会将“上一次确认的摄像头”保存到注册表，下次打开会默认选中它（没有则默认 0）
// - 点击“取消”/按 Esc/关闭窗口：不会中断流程，而是回退使用“上一次确认的摄像头”（没有则默认 0）
// - 勾选“记住我的选择”：下次调用将不会弹窗，直接返回上一次确认的摄像头（若有效）
int ShowCameraSelectorDialog(HWND parentHwnd = nullptr, HINSTANCE hInstance = nullptr);

// 清除保存的摄像头选择
// 同时清除“记住我的选择”开关
void ClearSavedCameraChoice();

// 获取保存的摄像头索引 (-1 表示没有保存)
int GetSavedCameraChoice();

} // namespace CameraSelector

#endif // _WIN32
