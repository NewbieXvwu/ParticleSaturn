#include "CameraPreview.h"

#include "CameraEnumerator.h"

#ifdef _WIN32

#include <iostream>

// GUID 定义 (头文件中是 extern 声明)
// {C1F400A0-3F08-11D3-9F0B-006008039E37}
const CLSID CLSID_SampleGrabber = {0xc1f400a0, 0x3f08, 0x11d3, {0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37}};
// {C1F400A4-3F08-11D3-9F0B-006008039E37}
const CLSID CLSID_NullRenderer = {0xc1f400a4, 0x3f08, 0x11d3, {0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37}};
// {6B652FFF-11FE-4FCE-92AD-0266B5D7C78F}
const IID IID_ISampleGrabber = {0x6b652fff, 0x11fe, 0x4fce, {0x92, 0xad, 0x02, 0x66, 0xb5, 0xd7, 0xc7, 0x8f}};
// {0579154A-2B53-4994-B0D0-E773148EFF85}
const IID IID_ISampleGrabberCB = {0x0579154a, 0x2b53, 0x4994, {0xb0, 0xd0, 0xe7, 0x73, 0x14, 0x8e, 0xff, 0x85}};

namespace CameraSelector {

// Sample Grabber 回调实现
class PreviewGrabberCallback : public ISampleGrabberCB {
  public:
    PreviewGrabberCallback(CameraPreviewInstance* owner) : m_owner(owner), m_refCount(1) {}

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
    STDMETHODIMP SampleCB(double time, IMediaSample* sample) override { return E_NOTIMPL; }

    STDMETHODIMP BufferCB(double time, BYTE* buffer, long size) override {
        if (m_owner && buffer && size > 0) {
            AM_MEDIA_TYPE mt;
            if (SUCCEEDED(m_owner->m_sampleGrabber->GetConnectedMediaType(&mt))) {
                if (mt.formattype == FORMAT_VideoInfo) {
                    VIDEOINFOHEADER* vih    = (VIDEOINFOHEADER*)mt.pbFormat;
                    int              width  = vih->bmiHeader.biWidth;
                    int              height = abs(vih->bmiHeader.biHeight);
                    m_owner->OnFrame(buffer, size, width, height);
                }
                if (mt.cbFormat > 0) {
                    CoTaskMemFree(mt.pbFormat);
                }
                if (mt.pUnk) {
                    mt.pUnk->Release();
                }
            }
        }
        return S_OK;
    }

  private:
    CameraPreviewInstance* m_owner;
    LONG                   m_refCount;
};

// CameraPreviewInstance 实现

CameraPreviewInstance::CameraPreviewInstance() {}

CameraPreviewInstance::~CameraPreviewInstance() {
    Stop();
}

bool CameraPreviewInstance::Start(int cameraIndex, int width, int height, int fps) {
    if (m_running) {
        Stop();
    }

    HRESULT hr;

    // 创建 Filter Graph
    hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_IGraphBuilder, (void**)&m_graphBuilder);
    if (FAILED(hr)) {
        std::cerr << "[CameraPreview] Failed to create filter graph" << std::endl;
        return false;
    }

    // 创建 Capture Graph Builder
    hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_ICaptureGraphBuilder2,
                          (void**)&m_captureBuilder);
    if (FAILED(hr)) {
        std::cerr << "[CameraPreview] Failed to create capture builder" << std::endl;
        Stop();
        return false;
    }

    m_captureBuilder->SetFiltergraph(m_graphBuilder);

    // 获取指定索引的摄像头
    ICreateDevEnum* devEnum = nullptr;
    hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum, (void**)&devEnum);
    if (FAILED(hr)) {
        Stop();
        return false;
    }

    IEnumMoniker* enumMoniker = nullptr;
    hr                        = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMoniker, 0);
    devEnum->Release();

    if (hr != S_OK || !enumMoniker) {
        Stop();
        return false;
    }

    IMoniker* moniker      = nullptr;
    int       currentIndex = 0;
    while (enumMoniker->Next(1, &moniker, nullptr) == S_OK) {
        if (currentIndex == cameraIndex) {
            hr = moniker->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)&m_sourceFilter);
            moniker->Release();
            break;
        }
        moniker->Release();
        currentIndex++;
    }
    enumMoniker->Release();

    if (!m_sourceFilter) {
        std::cerr << "[CameraPreview] Camera " << cameraIndex << " not found" << std::endl;
        Stop();
        return false;
    }

    // 添加源过滤器到图
    hr = m_graphBuilder->AddFilter(m_sourceFilter, L"Video Capture");
    if (FAILED(hr)) {
        Stop();
        return false;
    }

    // 设置视频格式
    IAMStreamConfig* streamConfig = nullptr;
    hr = m_captureBuilder->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, m_sourceFilter, IID_IAMStreamConfig,
                                         (void**)&streamConfig);
    if (SUCCEEDED(hr) && streamConfig) {
        AM_MEDIA_TYPE* pmt = nullptr;
        hr                 = streamConfig->GetFormat(&pmt);
        if (SUCCEEDED(hr) && pmt) {
            if (pmt->formattype == FORMAT_VideoInfo) {
                VIDEOINFOHEADER* vih    = (VIDEOINFOHEADER*)pmt->pbFormat;
                vih->bmiHeader.biWidth  = width;
                vih->bmiHeader.biHeight = height;
                vih->AvgTimePerFrame    = 10000000LL / fps; // 100ns units
                streamConfig->SetFormat(pmt);
            }
            if (pmt->cbFormat > 0) {
                CoTaskMemFree(pmt->pbFormat);
            }
            if (pmt->pUnk) {
                pmt->pUnk->Release();
            }
            CoTaskMemFree(pmt);
        }
        streamConfig->Release();
    }

    // 创建 Sample Grabber
    hr =
        CoCreateInstance(CLSID_SampleGrabber, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void**)&m_grabberFilter);
    if (FAILED(hr)) {
        Stop();
        return false;
    }

    hr = m_graphBuilder->AddFilter(m_grabberFilter, L"Sample Grabber");
    if (FAILED(hr)) {
        Stop();
        return false;
    }

    hr = m_grabberFilter->QueryInterface(IID_ISampleGrabber, (void**)&m_sampleGrabber);
    if (FAILED(hr)) {
        Stop();
        return false;
    }

    // 设置 Grabber 媒体类型 (RGB24)
    AM_MEDIA_TYPE mt;
    ZeroMemory(&mt, sizeof(mt));
    mt.majortype = MEDIATYPE_Video;
    mt.subtype   = MEDIASUBTYPE_RGB24;
    m_sampleGrabber->SetMediaType(&mt);
    m_sampleGrabber->SetBufferSamples(FALSE);
    m_sampleGrabber->SetOneShot(FALSE);

    // 创建 Null Renderer
    hr = CoCreateInstance(CLSID_NullRenderer, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void**)&m_nullRenderer);
    if (FAILED(hr)) {
        Stop();
        return false;
    }

    hr = m_graphBuilder->AddFilter(m_nullRenderer, L"Null Renderer");
    if (FAILED(hr)) {
        Stop();
        return false;
    }

    // 连接: Source -> Grabber -> Null Renderer
    hr = m_captureBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, m_sourceFilter, m_grabberFilter,
                                        m_nullRenderer);
    if (FAILED(hr)) {
        std::cerr << "[CameraPreview] Failed to render stream: 0x" << std::hex << hr << std::dec << std::endl;
        Stop();
        return false;
    }

    // 获取实际格式
    AM_MEDIA_TYPE connectedMt;
    hr = m_sampleGrabber->GetConnectedMediaType(&connectedMt);
    if (SUCCEEDED(hr) && connectedMt.formattype == FORMAT_VideoInfo) {
        VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)connectedMt.pbFormat;
        m_width              = vih->bmiHeader.biWidth;
        m_height             = abs(vih->bmiHeader.biHeight);
        if (connectedMt.cbFormat > 0) {
            CoTaskMemFree(connectedMt.pbFormat);
        }
        if (connectedMt.pUnk) {
            connectedMt.pUnk->Release();
        }
    } else {
        m_width  = width;
        m_height = height;
    }

    // 设置回调
    m_callback = new PreviewGrabberCallback(this);
    m_sampleGrabber->SetCallback(m_callback, 1); // 1 = BufferCB

    // 获取媒体控制
    hr = m_graphBuilder->QueryInterface(IID_IMediaControl, (void**)&m_mediaControl);
    if (FAILED(hr)) {
        Stop();
        return false;
    }

    // 开始捕获
    hr = m_mediaControl->Run();
    if (FAILED(hr)) {
        std::cerr << "[CameraPreview] Failed to start capture: 0x" << std::hex << hr << std::dec << std::endl;
        Stop();
        return false;
    }

    m_running = true;
    std::cout << "[CameraPreview] Started camera " << cameraIndex << " (" << m_width << "x" << m_height << ")"
              << std::endl;
    return true;
}

void CameraPreviewInstance::Stop() {
    m_running = false;

    if (m_sampleGrabber) {
        m_sampleGrabber->SetCallback(nullptr, 0);
    }

    if (m_mediaControl) {
        m_mediaControl->Stop();
        m_mediaControl->Release();
        m_mediaControl = nullptr;
    }

    if (m_callback) {
        m_callback->Release();
        m_callback = nullptr;
    }

    if (m_sampleGrabber) {
        m_sampleGrabber->Release();
        m_sampleGrabber = nullptr;
    }

    if (m_nullRenderer) {
        m_nullRenderer->Release();
        m_nullRenderer = nullptr;
    }

    if (m_grabberFilter) {
        m_grabberFilter->Release();
        m_grabberFilter = nullptr;
    }

    if (m_sourceFilter) {
        m_sourceFilter->Release();
        m_sourceFilter = nullptr;
    }

    if (m_captureBuilder) {
        m_captureBuilder->Release();
        m_captureBuilder = nullptr;
    }

    if (m_graphBuilder) {
        m_graphBuilder->Release();
        m_graphBuilder = nullptr;
    }
}

void CameraPreviewInstance::OnFrame(const BYTE* data, int size, int width, int height) {
    std::lock_guard<std::mutex> lock(m_frameMutex);

    // RGB24 -> BGRA 转换 (DirectShow 输出是倒置的 BGR)
    int expectedSize = width * height * 3;
    if (size < expectedSize) {
        return;
    }

    m_frameBuffer.resize(width * height * 4);

    // DirectShow 输出的图像是上下颠倒的，需要翻转
    for (int y = 0; y < height; y++) {
        const BYTE* srcRow = data + (height - 1 - y) * width * 3;
        BYTE*       dstRow = m_frameBuffer.data() + y * width * 4;

        for (int x = 0; x < width; x++) {
            dstRow[x * 4 + 0] = srcRow[x * 3 + 0]; // B
            dstRow[x * 4 + 1] = srcRow[x * 3 + 1]; // G
            dstRow[x * 4 + 2] = srcRow[x * 3 + 2]; // R
            dstRow[x * 4 + 3] = 255;               // A
        }
    }

    m_width       = width;
    m_height      = height;
    m_hasNewFrame = true;
}

ID2D1Bitmap* CameraPreviewInstance::GetLatestBitmap(D2DRenderer& renderer) {
    if (!m_hasNewFrame) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(m_frameMutex);
    if (m_frameBuffer.empty()) {
        return nullptr;
    }

    m_hasNewFrame = false;
    return renderer.CreateBitmapFromPixels(m_frameBuffer.data(), m_width, m_height);
}

// CameraPreviewManager 实现

CameraPreviewManager::CameraPreviewManager() {}

CameraPreviewManager::~CameraPreviewManager() {
    StopAll();
}

bool CameraPreviewManager::Initialize(const std::vector<int>& cameraIndices, int width, int height, int fps) {
    StopAll();

    for (int idx : cameraIndices) {
        auto preview = std::make_unique<CameraPreviewInstance>();
        if (preview->Start(idx, width, height, fps)) {
            m_previews.push_back(std::move(preview));
        } else {
            std::cerr << "[CameraPreviewManager] Failed to start camera " << idx << std::endl;
        }
    }

    return !m_previews.empty();
}

void CameraPreviewManager::StopAll() {
    for (auto& preview : m_previews) {
        preview->Stop();
    }
    m_previews.clear();
}

ID2D1Bitmap* CameraPreviewManager::GetBitmap(int index, D2DRenderer& renderer) {
    if (index < 0 || index >= static_cast<int>(m_previews.size())) {
        return nullptr;
    }
    return m_previews[index]->GetLatestBitmap(renderer);
}

} // namespace CameraSelector

#endif // _WIN32
