#pragma once
// CameraCapture - 跨平台摄像头捕获抽象层
// Windows: 优先使用 DirectShow 异步回调，失败时 fallback 到 OpenCV
// 其他平台: 使用 OpenCV

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <thread>

// 摄像头捕获接口
class ICameraCapture {
  public:
    virtual ~ICameraCapture() = default;

    // 打开摄像头
    virtual bool open(int cameraId, int width = 640, int height = 480) = 0;

    // 关闭摄像头
    virtual void close() = 0;

    // 是否已打开
    virtual bool isOpened() const = 0;

    // 获取最新帧 (非阻塞，返回是否有新帧)
    virtual bool getLatestFrame(cv::Mat& frame) = 0;

    // 等待新帧 (阻塞，带超时)
    // 返回: true = 获取到新帧, false = 超时或错误
    virtual bool waitForFrame(cv::Mat& frame, int timeout_ms) = 0;

    // 获取帧宽度
    virtual int getWidth() const = 0;

    // 获取帧高度
    virtual int getHeight() const = 0;
};

// OpenCV 异步实现 (跨平台 fallback)
// 使用专用读帧线程实现事件驱动语义
class OpenCVCapture : public ICameraCapture {
  public:
    OpenCVCapture();
    ~OpenCVCapture() override;

    bool open(int cameraId, int width = 640, int height = 480) override;
    void close() override;
    bool isOpened() const override;
    bool getLatestFrame(cv::Mat& frame) override;
    bool waitForFrame(cv::Mat& frame, int timeout_ms) override;

    int getWidth() const override { return m_width; }

    int getHeight() const override { return m_height; }

  private:
    void readerLoop();

    cv::VideoCapture m_cap;
    int              m_width  = 0;
    int              m_height = 0;

    // 异步读帧线程
    std::thread             m_readerThread;
    std::atomic<bool>       m_running{false};
    std::mutex              m_frameMutex;
    std::condition_variable m_frameCV;
    cv::Mat                 m_frameBuffer;
    std::atomic<bool>       m_hasNewFrame{false};
};

#ifdef _WIN32
// 前向声明回调类
class DSGrabberCallback;

// DirectShow 异步实现 (Windows 专用)
class DirectShowCapture : public ICameraCapture {
    friend class DSGrabberCallback; // 允许回调类访问私有成员

  public:
    DirectShowCapture();
    ~DirectShowCapture() override;

    bool open(int cameraId, int width = 640, int height = 480) override;
    void close() override;
    bool isOpened() const override;
    bool getLatestFrame(cv::Mat& frame) override;
    bool waitForFrame(cv::Mat& frame, int timeout_ms) override;

    int getWidth() const override { return m_width; }

    int getHeight() const override { return m_height; }

  private:
    struct Impl;
    std::unique_ptr<Impl>   m_impl;
    int                     m_width    = 0;
    int                     m_height   = 0;
    std::atomic<bool>       m_opened   = false;
    std::atomic<bool>       m_hasFrame = false;
    std::mutex              m_frameMutex;
    std::condition_variable m_frameCV;
    cv::Mat                 m_frameBuffer;
};
#endif

// 工厂函数：创建最佳可用的摄像头捕获实例
// Windows: 尝试 DirectShow，失败则 fallback 到 OpenCV
// 其他平台: 直接使用 OpenCV
std::unique_ptr<ICameraCapture> CreateCameraCapture();
