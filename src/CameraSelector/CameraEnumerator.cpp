#include "CameraEnumerator.h"

#ifdef _WIN32

#include <iostream>

namespace CameraSelector {

std::vector<CameraInfo> EnumerateCameras() {
    std::vector<CameraInfo> cameras;

    // 初始化 COM (如果尚未初始化)
    const HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = (initHr == S_OK || initHr == S_FALSE);
    if (FAILED(initHr) && initHr != RPC_E_CHANGED_MODE) {
        std::cerr << "[CameraEnumerator] COM initialization failed: 0x"
                  << std::hex << initHr << std::dec << std::endl;
        return cameras;
    }

    ICreateDevEnum* pDevEnum = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ICreateDevEnum, (void**)&pDevEnum);
    if (FAILED(hr)) {
        std::cerr << "[CameraEnumerator] Failed to create device enumerator: 0x"
                  << std::hex << hr << std::dec << std::endl;
        if (needUninit) CoUninitialize();
        return cameras;
    }

    IEnumMoniker* pEnum = nullptr;
    hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
    if (hr == S_FALSE || !pEnum) {
        // 没有找到任何设备
        pDevEnum->Release();
        if (needUninit) CoUninitialize();
        return cameras;
    }
    if (FAILED(hr)) {
        std::cerr << "[CameraEnumerator] Failed to enumerate video devices: 0x"
                  << std::hex << hr << std::dec << std::endl;
        pDevEnum->Release();
        if (needUninit) CoUninitialize();
        return cameras;
    }

    IMoniker* pMoniker = nullptr;
    int index = 0;
    while (pEnum->Next(1, &pMoniker, nullptr) == S_OK) {
        CameraInfo info;
        info.index = index++;

        // 获取属性包
        IPropertyBag* pPropBag = nullptr;
        hr = pMoniker->BindToStorage(nullptr, nullptr, IID_IPropertyBag, (void**)&pPropBag);
        if (SUCCEEDED(hr)) {
            VARIANT var;
            VariantInit(&var);

            // 获取友好名称
            hr = pPropBag->Read(L"FriendlyName", &var, nullptr);
            if (SUCCEEDED(hr)) {
                info.name = var.bstrVal;
                VariantClear(&var);
            } else {
                info.name = L"Unknown Camera";
            }

            // 获取设备路径
            VariantInit(&var);
            hr = pPropBag->Read(L"DevicePath", &var, nullptr);
            if (SUCCEEDED(hr)) {
                info.devicePath = var.bstrVal;
                VariantClear(&var);
            }

            pPropBag->Release();
        } else {
            info.name = L"Unknown Camera";
        }

        cameras.push_back(info);
        pMoniker->Release();
    }

    pEnum->Release();
    pDevEnum->Release();

    if (needUninit) CoUninitialize();

    std::cout << "[CameraEnumerator] Found " << cameras.size() << " camera(s)" << std::endl;
    for (const auto& cam : cameras) {
        std::wcout << L"  [" << cam.index << L"] " << cam.name << std::endl;
    }

    return cameras;
}

} // namespace CameraSelector

#endif // _WIN32
