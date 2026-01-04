#pragma once
// CameraPreview - 摄像头预览管理
// 使用 DirectShow 捕获多个摄像头的实时画面

#ifdef _WIN32

#include "D2DRenderer.h"
#include <dshow.h>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>

#pragma comment(lib, "strmiids.lib")

// ISampleGrabber 接口定义 (qedit.h 在新版 SDK 中已移除)
// GUID 声明 (定义在 CameraPreview.cpp 中)
extern const CLSID CLSID_SampleGrabber;
extern const CLSID CLSID_NullRenderer;
extern const IID IID_ISampleGrabber;
extern const IID IID_ISampleGrabberCB;

interface ISampleGrabberCB : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double SampleTime, IMediaSample* pSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double SampleTime, BYTE* pBuffer, long BufferLen) = 0;
};

interface ISampleGrabber : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL OneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL BufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* pBufferSize, long* pBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** ppSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* pCallback, long WhichMethodToCallback) = 0;
};

namespace CameraSelector {

// 前向声明
class PreviewGrabberCallback;

// 单个摄像头预览实例
class CameraPreviewInstance {
    friend class PreviewGrabberCallback;

public:
    CameraPreviewInstance();
    ~CameraPreviewInstance();

    // 启动预览
    bool Start(int cameraIndex, int width = 640, int height = 480, int fps = 30);

    // 停止预览
    void Stop();

    // 是否正在运行
    bool IsRunning() const { return m_running; }

    // 获取最新帧的 D2D 位图 (调用者负责 Release)
    // 返回 nullptr 如果没有新帧
    ID2D1Bitmap* GetLatestBitmap(D2DRenderer& renderer);

    // 获取帧尺寸
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

private:
    void OnFrame(const BYTE* data, int size, int width, int height);

    IGraphBuilder*       m_graphBuilder  = nullptr;
    ICaptureGraphBuilder2* m_captureBuilder = nullptr;
    IMediaControl*       m_mediaControl  = nullptr;
    IBaseFilter*         m_sourceFilter  = nullptr;
    IBaseFilter*         m_grabberFilter = nullptr;
    IBaseFilter*         m_nullRenderer  = nullptr;
    ISampleGrabber*      m_sampleGrabber = nullptr;

    PreviewGrabberCallback* m_callback = nullptr;

    std::mutex           m_frameMutex;
    std::vector<BYTE>    m_frameBuffer;
    std::atomic<bool>    m_hasNewFrame{false};
    std::atomic<bool>    m_running{false};

    int m_width  = 0;
    int m_height = 0;
};

// 多摄像头预览管理器
class CameraPreviewManager {
public:
    CameraPreviewManager();
    ~CameraPreviewManager();

    // 初始化所有摄像头预览
    bool Initialize(const std::vector<int>& cameraIndices,
                    int width = 640, int height = 480, int fps = 30);

    // 停止所有预览
    void StopAll();

    // 获取指定摄像头的最新位图
    ID2D1Bitmap* GetBitmap(int index, D2DRenderer& renderer);

    // 获取预览数量
    size_t GetCount() const { return m_previews.size(); }

private:
    std::vector<std::unique_ptr<CameraPreviewInstance>> m_previews;
};

} // namespace CameraSelector

#endif // _WIN32
