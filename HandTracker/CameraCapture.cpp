// CameraCapture.cpp - 跨平台摄像头捕获实现

#include "CameraCapture.h"

#include <iostream>

#ifdef _WIN32
#include <Windows.h>
#include <dshow.h>
#include <strmif.h>

#pragma comment(lib, "strmiids.lib")

// DirectShow GUID 定义
DEFINE_GUID(CLSID_SampleGrabber, 0xc1f400a0, 0x3f08, 0x11d3, 0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37);
DEFINE_GUID(CLSID_NullRenderer, 0xc1f400a4, 0x3f08, 0x11d3, 0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37);
DEFINE_GUID(IID_ISampleGrabber, 0x6b652fff, 0x11fe, 0x4fce, 0x92, 0xad, 0x02, 0x66, 0xb5, 0xd7, 0xc7, 0x8f);
DEFINE_GUID(IID_ISampleGrabberCB, 0x0579154a, 0x2b53, 0x4994, 0xb0, 0xd0, 0xe7, 0x73, 0x14, 0x8e, 0xff, 0x85);

// ISampleGrabberCB 接口定义
MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85")
ISampleGrabberCB : public IUnknown {
  public:
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double SampleTime, IMediaSample * pSample)        = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double SampleTime, BYTE * pBuffer, long BufferLen) = 0;
};

// ISampleGrabber 接口定义
MIDL_INTERFACE("6B652FFF-11FE-4FCE-92AD-0266B5D7C78F")
ISampleGrabber : public IUnknown {
  public:
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL OneShot)                                    = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE * pType)                   = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE * pType)                = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL BufferThem)                           = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* pBufferSize, long* pBuffer)          = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample * *ppSample)                  = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB * pCallback, long WhichMethodToCallback) = 0;
};

// SampleGrabber 回调实现 (使用独特名称避免与 OpenCV 内部类冲突)
class DSGrabberCallback : public ISampleGrabberCB {
  public:
    DSGrabberCallback(DirectShowCapture* owner) : m_owner(owner), m_refCount(1) {}

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_ISampleGrabberCB) {
            *ppv = static_cast<ISampleGrabberCB*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_refCount); }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0) {
            delete this;
        }
        return count;
    }

    // ISampleGrabberCB
    STDMETHODIMP SampleCB(double SampleTime, IMediaSample* pSample) override {
        // 不使用此方法
        return S_OK;
    }

    STDMETHODIMP BufferCB(double SampleTime, BYTE* pBuffer, long BufferLen) override;

  private:
    DirectShowCapture* m_owner;
    LONG               m_refCount;
};

// DirectShowCapture 内部实现
struct DirectShowCapture::Impl {
    IGraphBuilder*         pGraph         = nullptr;
    ICaptureGraphBuilder2* pBuilder       = nullptr;
    IMediaControl*         pControl       = nullptr;
    IBaseFilter*           pCap           = nullptr;
    IBaseFilter*           pGrabberFilter = nullptr;
    IBaseFilter*           pNullRenderer  = nullptr;
    ISampleGrabber*        pGrabber       = nullptr;
    DSGrabberCallback*     pCallback      = nullptr;
    bool                   comInitialized = false;

    ~Impl() { Release(); }

    void Release() {
        if (pControl) {
            pControl->Stop();
            pControl->Release();
            pControl = nullptr;
        }
        if (pGrabber) {
            pGrabber->Release();
            pGrabber = nullptr;
        }
        if (pNullRenderer) {
            pNullRenderer->Release();
            pNullRenderer = nullptr;
        }
        if (pGrabberFilter) {
            pGrabberFilter->Release();
            pGrabberFilter = nullptr;
        }
        if (pCap) {
            pCap->Release();
            pCap = nullptr;
        }
        if (pBuilder) {
            pBuilder->Release();
            pBuilder = nullptr;
        }
        if (pGraph) {
            pGraph->Release();
            pGraph = nullptr;
        }
        if (pCallback) {
            pCallback->Release();
            pCallback = nullptr;
        }
        if (comInitialized) {
            CoUninitialize();
            comInitialized = false;
        }
    }
};

// BufferCB 实现 - 在 Impl 定义之后
STDMETHODIMP DSGrabberCallback::BufferCB(double SampleTime, BYTE* pBuffer, long BufferLen) {
    if (!m_owner || !pBuffer || BufferLen <= 0) {
        return S_OK;
    }

    int width  = m_owner->getWidth();
    int height = m_owner->getHeight();

    if (BufferLen < width * height * 3) {
        return S_OK;
    }

    // 复制帧数据到缓冲区 (BGR 格式，需要垂直翻转)
    std::lock_guard<std::mutex> lock(m_owner->m_frameMutex);
    if (m_owner->m_frameBuffer.empty() || m_owner->m_frameBuffer.cols != width ||
        m_owner->m_frameBuffer.rows != height) {
        m_owner->m_frameBuffer.create(height, width, CV_8UC3);
    }

    // DirectShow 返回的图像是上下颠倒的，需要翻转
    for (int y = 0; y < height; y++) {
        memcpy(m_owner->m_frameBuffer.ptr(height - 1 - y), pBuffer + y * width * 3, width * 3);
    }

    m_owner->m_hasFrame = true;
    return S_OK;
}

DirectShowCapture::DirectShowCapture() : m_impl(std::make_unique<Impl>()) {}

DirectShowCapture::~DirectShowCapture() { close(); }

bool DirectShowCapture::open(int cameraId, int width, int height) {
    close();

    m_width  = width;
    m_height = height;

    // 初始化 COM
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        std::cerr << "[DirectShow] COM initialization failed" << std::endl;
        return false;
    }
    m_impl->comInitialized = true;

    // 创建 Filter Graph
    hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_IGraphBuilder,
                          (void**)&m_impl->pGraph);
    if (FAILED(hr)) {
        std::cerr << "[DirectShow] Failed to create FilterGraph" << std::endl;
        close();
        return false;
    }

    // 创建 Capture Graph Builder
    hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_ICaptureGraphBuilder2,
                          (void**)&m_impl->pBuilder);
    if (FAILED(hr)) {
        std::cerr << "[DirectShow] Failed to create CaptureGraphBuilder" << std::endl;
        close();
        return false;
    }

    m_impl->pBuilder->SetFiltergraph(m_impl->pGraph);

    // 枚举视频捕获设备
    ICreateDevEnum* pDevEnum = nullptr;
    hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum,
                          (void**)&pDevEnum);
    if (FAILED(hr)) {
        std::cerr << "[DirectShow] Failed to create device enumerator" << std::endl;
        close();
        return false;
    }

    IEnumMoniker* pEnum = nullptr;
    hr                  = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
    pDevEnum->Release();

    if (hr != S_OK || !pEnum) {
        std::cerr << "[DirectShow] No video capture devices found" << std::endl;
        close();
        return false;
    }

    // 选择指定索引的设备
    IMoniker* pMoniker = nullptr;
    int       index    = 0;
    while (pEnum->Next(1, &pMoniker, nullptr) == S_OK) {
        if (index == cameraId) {
            hr = pMoniker->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)&m_impl->pCap);
            pMoniker->Release();
            break;
        }
        pMoniker->Release();
        index++;
    }
    pEnum->Release();

    if (!m_impl->pCap) {
        std::cerr << "[DirectShow] Camera " << cameraId << " not found" << std::endl;
        close();
        return false;
    }

    // 添加捕获设备到图
    hr = m_impl->pGraph->AddFilter(m_impl->pCap, L"Capture");
    if (FAILED(hr)) {
        std::cerr << "[DirectShow] Failed to add capture filter" << std::endl;
        close();
        return false;
    }

    // 设置分辨率
    IAMStreamConfig* pConfig = nullptr;
    hr = m_impl->pBuilder->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, m_impl->pCap, IID_IAMStreamConfig,
                                         (void**)&pConfig);
    if (SUCCEEDED(hr) && pConfig) {
        AM_MEDIA_TYPE* pmt = nullptr;
        if (SUCCEEDED(pConfig->GetFormat(&pmt)) && pmt) {
            if (pmt->formattype == FORMAT_VideoInfo && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
                VIDEOINFOHEADER* pVih  = (VIDEOINFOHEADER*)pmt->pbFormat;
                pVih->bmiHeader.biWidth  = width;
                pVih->bmiHeader.biHeight = height;
                pConfig->SetFormat(pmt);
            }
            if (pmt->pbFormat) {
                CoTaskMemFree(pmt->pbFormat);
            }
            CoTaskMemFree(pmt);
        }
        pConfig->Release();
    }

    // 创建 Sample Grabber
    hr = CoCreateInstance(CLSID_SampleGrabber, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter,
                          (void**)&m_impl->pGrabberFilter);
    if (FAILED(hr)) {
        std::cerr << "[DirectShow] Failed to create SampleGrabber" << std::endl;
        close();
        return false;
    }

    hr = m_impl->pGrabberFilter->QueryInterface(IID_ISampleGrabber, (void**)&m_impl->pGrabber);
    if (FAILED(hr)) {
        std::cerr << "[DirectShow] Failed to get ISampleGrabber interface" << std::endl;
        close();
        return false;
    }

    // 设置媒体类型为 RGB24
    AM_MEDIA_TYPE mt;
    ZeroMemory(&mt, sizeof(AM_MEDIA_TYPE));
    mt.majortype  = MEDIATYPE_Video;
    mt.subtype    = MEDIASUBTYPE_RGB24;
    mt.formattype = FORMAT_VideoInfo;
    m_impl->pGrabber->SetMediaType(&mt);

    // 添加 Sample Grabber 到图
    hr = m_impl->pGraph->AddFilter(m_impl->pGrabberFilter, L"SampleGrabber");
    if (FAILED(hr)) {
        std::cerr << "[DirectShow] Failed to add SampleGrabber filter" << std::endl;
        close();
        return false;
    }

    // 创建 Null Renderer
    hr = CoCreateInstance(CLSID_NullRenderer, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter,
                          (void**)&m_impl->pNullRenderer);
    if (FAILED(hr)) {
        std::cerr << "[DirectShow] Failed to create NullRenderer" << std::endl;
        close();
        return false;
    }

    hr = m_impl->pGraph->AddFilter(m_impl->pNullRenderer, L"NullRenderer");
    if (FAILED(hr)) {
        std::cerr << "[DirectShow] Failed to add NullRenderer filter" << std::endl;
        close();
        return false;
    }

    // 连接: Capture -> SampleGrabber -> NullRenderer
    hr = m_impl->pBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, m_impl->pCap, m_impl->pGrabberFilter,
                                        m_impl->pNullRenderer);
    if (FAILED(hr)) {
        std::cerr << "[DirectShow] Failed to render stream" << std::endl;
        close();
        return false;
    }

    // 获取实际分辨率
    AM_MEDIA_TYPE connectedMt;
    if (SUCCEEDED(m_impl->pGrabber->GetConnectedMediaType(&connectedMt))) {
        if (connectedMt.formattype == FORMAT_VideoInfo && connectedMt.cbFormat >= sizeof(VIDEOINFOHEADER)) {
            VIDEOINFOHEADER* pVih = (VIDEOINFOHEADER*)connectedMt.pbFormat;
            m_width               = pVih->bmiHeader.biWidth;
            m_height              = abs(pVih->bmiHeader.biHeight);
        }
        if (connectedMt.pbFormat) {
            CoTaskMemFree(connectedMt.pbFormat);
        }
    }

    // 设置回调
    m_impl->pCallback = new DSGrabberCallback(this);
    m_impl->pGrabber->SetBufferSamples(FALSE);
    m_impl->pGrabber->SetOneShot(FALSE);
    m_impl->pGrabber->SetCallback(m_impl->pCallback, 1); // 1 = BufferCB

    // 获取媒体控制接口并启动
    hr = m_impl->pGraph->QueryInterface(IID_IMediaControl, (void**)&m_impl->pControl);
    if (FAILED(hr)) {
        std::cerr << "[DirectShow] Failed to get IMediaControl" << std::endl;
        close();
        return false;
    }

    hr = m_impl->pControl->Run();
    if (FAILED(hr)) {
        std::cerr << "[DirectShow] Failed to start capture" << std::endl;
        close();
        return false;
    }

    m_opened = true;
    std::cout << "[DirectShow] Camera opened: " << m_width << "x" << m_height << std::endl;
    return true;
}

void DirectShowCapture::close() {
    m_opened   = false;
    m_hasFrame = false;
    m_impl->Release();
}

bool DirectShowCapture::isOpened() const { return m_opened; }

bool DirectShowCapture::getLatestFrame(cv::Mat& frame) {
    if (!m_opened || !m_hasFrame) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_frameMutex);
    if (m_frameBuffer.empty()) {
        return false;
    }

    m_frameBuffer.copyTo(frame);
    m_hasFrame = false;
    return true;
}

#endif // _WIN32

// OpenCV 实现
OpenCVCapture::OpenCVCapture() {}

OpenCVCapture::~OpenCVCapture() { close(); }

bool OpenCVCapture::open(int cameraId, int width, int height) {
    close();

    // 尝试不同的后端
    bool opened = false;

#ifdef _WIN32
    if (!opened) {
        try {
            m_cap.open(cameraId, cv::CAP_DSHOW);
            opened = m_cap.isOpened();
        } catch (...) {
        }
    }
    if (!opened) {
        try {
            m_cap.open(cameraId, cv::CAP_MSMF);
            opened = m_cap.isOpened();
        } catch (...) {
        }
    }
#endif

    if (!opened) {
        try {
            m_cap.open(cameraId, cv::CAP_ANY);
            opened = m_cap.isOpened();
        } catch (...) {
        }
    }

    if (!opened) {
        return false;
    }

    m_cap.set(cv::CAP_PROP_FRAME_WIDTH, width);
    m_cap.set(cv::CAP_PROP_FRAME_HEIGHT, height);

    m_width  = (int)m_cap.get(cv::CAP_PROP_FRAME_WIDTH);
    m_height = (int)m_cap.get(cv::CAP_PROP_FRAME_HEIGHT);

    std::cout << "[OpenCV] Camera opened: " << m_width << "x" << m_height << std::endl;
    return true;
}

void OpenCVCapture::close() {
    if (m_cap.isOpened()) {
        m_cap.release();
    }
    m_width  = 0;
    m_height = 0;
}

bool OpenCVCapture::isOpened() const { return m_cap.isOpened(); }

bool OpenCVCapture::getLatestFrame(cv::Mat& frame) {
    if (!m_cap.isOpened()) {
        return false;
    }
    return m_cap.read(frame);
}

// 工厂函数
std::unique_ptr<ICameraCapture> CreateCameraCapture() {
#ifdef _WIN32
    // Windows: 优先尝试 DirectShow
    auto dsCapture = std::make_unique<DirectShowCapture>();
    // 返回 DirectShow 实例，open() 时如果失败会在调用方处理
    return dsCapture;
#else
    // 其他平台: 使用 OpenCV
    return std::make_unique<OpenCVCapture>();
#endif
}
