#pragma once

#include <Windows.h>

#include <string>

namespace i18n {

// Detect if system language is Chinese
inline bool IsChineseSystem() {
    LANGID langId      = GetUserDefaultUILanguage();
    WORD   primaryLang = PRIMARYLANGID(langId);
    return primaryLang == LANG_CHINESE;
}

// All user-visible strings
struct Strings {
    // Error dialog - titles
    const char* errorTitle;
    const char* warningTitle;
    const char* crashTitle;

    // Error dialog - common
    const char* expandDetails;
    const char* collapseDetails;
    const char* copyAll;
    const char* close;
    const char* closeProgram;

    // Error messages - recoverable
    const char* cameraInitFailed;
    const char* handTrackerLoadFailed;
    const char* shaderCompileFailed;
    const char* outOfVideoMemory;

    // Error messages - initialization
    const char* glfwInitFailed;
    const char* windowCreateFailed;
    const char* openglLoadFailed;
    const char* openglVersionUnsupported;
    const char* fboCreateFailed;
    const char* embeddedResourceFailed;

    // Error messages - camera details
    const char* cameraNotFound;
    const char* cameraInUse;
    const char* cameraPermissionDenied;

    // Error details (technical messages shown in expandable section)
    const char* detailWindowCreateFailed;
    const char* detailOpenGLLoadFailed;
    const char* detailOpenGLVersionLow;
    const char* detailOpenGLRequired;

    // Error messages - hand tracker details
    const char* palmModelLoadFailed;
    const char* handModelLoadFailed;

    // Error messages - fatal
    const char* unexpectedError;
    const char* calculationError;
    const char* stackOverflow;
    const char* accessViolation;

    // Crash report sections
    const char* sectionException;
    const char* sectionCallStack;
    const char* sectionSystem;
    const char* sectionGraphics;
    const char* sectionCamera;
    const char* sectionAppState;
    const char* sectionRecentLogs;

    // Crash report fields
    const char* fieldType;
    const char* fieldAddress;
    const char* fieldOperation;
    const char* fieldStage;
    const char* fieldOS;
    const char* fieldLanguage;
    const char* fieldMemory;
    const char* fieldGPU;
    const char* fieldDriver;
    const char* fieldOpenGL;
    const char* fieldVRAM;
    const char* fieldDisplay;
    const char* fieldCameraDevice;
    const char* fieldCameraIndex;
    const char* fieldCameraResolution;
    const char* fieldCameraStatus;
    const char* fieldVersion;
    const char* fieldUptime;
    const char* fieldFrame;
    const char* fieldParticles;
    const char* fieldLOD;
    const char* fieldHandTracking;

    // Status values
    const char* statusActive;
    const char* statusInactive;
    const char* statusEnabled;
    const char* statusDisabled;
    const char* statusUnknown;
    const char* statusRead;
    const char* statusWrite;

    // Crash analyzer
    const char* crashAnalyzerTitle;
    const char* crashAnalyzerButton;
    const char* pdbFile;
    const char* dropOrSelect;
    const char* crashReport;
    const char* pasteReport;
    const char* analyze;
    const char* analysisResult;
    const char* copyResult;
    const char* pdbLoaded;
    const char* pdbSize;
    const char* noPdbLoaded;
    const char* analysisSuccess;
    const char* analysisNoAddresses;
    const char* selectPdbFile;
    const char* paste;
    const char* clear;

    // Debug panel
    const char* debugPanelTitle;
    const char* sectionPerformance;
    const char* sectionHandTracking;
    const char* sectionVisuals;
    const char* sectionWindow;
    const char* sectionLog;
    const char* fps;
    const char* particles;
    const char* pixelRatio;
    const char* resolution;
    const char* handDetected;
    const char* yes;
    const char* no;
    const char* scale;
    const char* animationScale;
    const char* animationRotX;
    const char* animationRotY;
    const char* showCameraDebug;
    const char* darkMode;
    const char* glassBlur;
    const char* blurStrength;
    const char* backdrop;
    const char* fullscreen;
    const char* transparent;
    const char* clearLog;
    const char* copyAllLog;

    // Advanced section
    const char* sectionAdvanced;
    const char* simdMode;
    const char* simdAuto;
    const char* simdAVX2;
    const char* simdSSE;
    const char* simdScalar;
    const char* simdCurrent;

    // VSync
    const char* vsync;
    const char* vsyncOff;
    const char* vsyncOn;
    const char* vsyncAdaptive;
};

// Chinese strings
inline const Strings& GetChinese() {
    static const Strings zh = {
        // Error dialog - titles
        .errorTitle   = "错误",
        .warningTitle = "警告",
        .crashTitle   = "程序遇到意外问题",

        // Error dialog - common
        .expandDetails   = "展开详情",
        .collapseDetails = "收起详情",
        .copyAll         = "复制全部",
        .close           = "确定",
        .closeProgram    = "关闭程序",

        // Error messages - recoverable
        .cameraInitFailed      = "无法启动摄像头，手势追踪功能已禁用",
        .handTrackerLoadFailed = "手势追踪模块加载失败",
        .shaderCompileFailed   = "图形初始化失败，请尝试更新显卡驱动",
        .outOfVideoMemory      = "显存不足，请关闭其他程序后重试",

        // Error messages - initialization
        .glfwInitFailed           = "窗口系统初始化失败，请检查系统兼容性",
        .windowCreateFailed       = "无法创建窗口，您的显卡可能不支持 OpenGL 4.4",
        .openglLoadFailed         = "OpenGL 扩展加载失败，请更新显卡驱动",
        .openglVersionUnsupported = "您的显卡不支持 OpenGL 4.4，程序需要此版本才能运行",
        .fboCreateFailed          = "帧缓冲创建失败，请尝试更新显卡驱动",
        .embeddedResourceFailed   = "内置资源加载失败，程序文件可能已损坏",

        // Error messages - camera details
        .cameraNotFound          = "找不到摄像头，手势控制将不可用",
        .cameraInUse             = "摄像头被其他程序占用，手势控制将不可用",
        .cameraPermissionDenied  = "摄像头访问被拒绝，请在系统设置中允许访问",

        // Error details (technical messages shown in expandable section)
        .detailWindowCreateFailed = "glfwCreateWindow() 返回 NULL。\n\n"
                                    "这通常意味着您的显卡不支持 OpenGL 4.4 Core Profile。\n"
                                    "请更新显卡驱动或检查硬件兼容性。",
        .detailOpenGLLoadFailed   = "gladLoadGLLoader() 返回 false。\n\n"
                                    "无法加载 OpenGL 函数指针。\n"
                                    "请更新显卡驱动。",
        .detailOpenGLVersionLow   = "检测到的 OpenGL 版本",
        .detailOpenGLRequired     = "需要 OpenGL 4.4 或更高版本",

        // Error messages - hand tracker details
        .palmModelLoadFailed = "手掌检测模型加载失败",
        .handModelLoadFailed = "手部关键点模型加载失败",

        // Error messages - fatal
        .unexpectedError  = "程序遇到意外问题，需要关闭",
        .calculationError = "程序遇到计算错误，需要关闭",
        .stackOverflow    = "程序栈空间耗尽，需要关闭",
        .accessViolation  = "程序访问了无效的内存区域",

        // Crash report sections
        .sectionException  = "异常信息",
        .sectionCallStack  = "调用栈",
        .sectionSystem     = "系统环境",
        .sectionGraphics   = "图形设备",
        .sectionCamera     = "摄像头",
        .sectionAppState   = "应用状态",
        .sectionRecentLogs = "最近日志",

        // Crash report fields
        .fieldType             = "类型",
        .fieldAddress          = "地址",
        .fieldOperation        = "操作",
        .fieldStage            = "崩溃阶段",
        .fieldOS               = "系统",
        .fieldLanguage         = "语言",
        .fieldMemory           = "内存",
        .fieldGPU              = "GPU",
        .fieldDriver           = "驱动版本",
        .fieldOpenGL           = "OpenGL",
        .fieldVRAM             = "显存",
        .fieldDisplay          = "显示器",
        .fieldCameraDevice     = "设备",
        .fieldCameraIndex      = "索引",
        .fieldCameraResolution = "分辨率",
        .fieldCameraStatus     = "状态",
        .fieldVersion          = "版本",
        .fieldUptime           = "运行时间",
        .fieldFrame            = "当前帧",
        .fieldParticles        = "粒子数",
        .fieldLOD              = "LOD",
        .fieldHandTracking     = "手势追踪",

        // Status values
        .statusActive   = "活跃",
        .statusInactive = "未活跃",
        .statusEnabled  = "已启用",
        .statusDisabled = "已禁用",
        .statusUnknown  = "未知",
        .statusRead     = "读取",
        .statusWrite    = "写入",

        // Crash analyzer
        .crashAnalyzerTitle  = "崩溃分析工具",
        .crashAnalyzerButton = "打开崩溃分析工具",
        .pdbFile             = "PDB 文件",
        .dropOrSelect        = "拖入文件或点击选择",
        .crashReport         = "崩溃报告",
        .pasteReport         = "粘贴崩溃报告内容",
        .analyze             = "解析",
        .analysisResult      = "解析结果",
        .copyResult          = "复制结果",
        .pdbLoaded           = "已加载",
        .pdbSize             = "大小",
        .noPdbLoaded         = "未加载 PDB 文件",
        .analysisSuccess     = "解析完成",
        .analysisNoAddresses = "未找到可解析的地址",
        .selectPdbFile       = "选择 PDB 文件",
        .paste               = "粘贴",
        .clear               = "清空",

        // Debug panel
        .debugPanelTitle     = "调试面板",
        .sectionPerformance  = "性能",
        .sectionHandTracking = "手势追踪",
        .sectionVisuals      = "视觉效果",
        .sectionWindow       = "窗口",
        .sectionLog          = "日志",
        .fps                 = "帧率",
        .particles           = "粒子数",
        .pixelRatio          = "像素比例",
        .resolution          = "分辨率",
        .handDetected        = "检测到手势",
        .yes                 = "是",
        .no                  = "否",
        .scale               = "缩放",
        .animationScale      = "动画缩放",
        .animationRotX       = "动画旋转X",
        .animationRotY       = "动画旋转Y",
        .showCameraDebug     = "显示摄像头调试窗口",
        .darkMode            = "深色模式",
        .glassBlur           = "玻璃模糊",
        .blurStrength        = "模糊强度",
        .backdrop            = "背景",
        .fullscreen          = "全屏",
        .transparent         = "透明",
        .clearLog            = "清空",
        .copyAllLog          = "复制全部",

        // Advanced section
        .sectionAdvanced = "高级",
        .simdMode        = "SIMD 模式",
        .simdAuto        = "自动",
        .simdAVX2        = "AVX2",
        .simdSSE         = "SSE",
        .simdScalar      = "标量",
        .simdCurrent     = "当前实现",

        // VSync
        .vsync         = "垂直同步",
        .vsyncOff      = "关闭",
        .vsyncOn       = "开启",
        .vsyncAdaptive = "自适应",
    };
    return zh;
}

// English strings
inline const Strings& GetEnglish() {
    static const Strings en = {
        // Error dialog - titles
        .errorTitle   = "Error",
        .warningTitle = "Warning",
        .crashTitle   = "Unexpected Error",

        // Error dialog - common
        .expandDetails   = "Show Details",
        .collapseDetails = "Hide Details",
        .copyAll         = "Copy All",
        .close           = "OK",
        .closeProgram    = "Close Program",

        // Error messages - recoverable
        .cameraInitFailed      = "Camera unavailable, hand tracking disabled",
        .handTrackerLoadFailed = "Hand tracking module failed to load",
        .shaderCompileFailed   = "Graphics initialization failed, try updating GPU driver",
        .outOfVideoMemory      = "Out of video memory, please close other programs",

        // Error messages - initialization
        .glfwInitFailed           = "Window system initialization failed, check system compatibility",
        .windowCreateFailed       = "Failed to create window, your GPU may not support OpenGL 4.4",
        .openglLoadFailed         = "Failed to load OpenGL extensions, please update GPU driver",
        .openglVersionUnsupported = "Your GPU does not support OpenGL 4.4, which is required",
        .fboCreateFailed          = "Framebuffer creation failed, try updating GPU driver",
        .embeddedResourceFailed   = "Failed to load embedded resources, program file may be corrupted",

        // Error messages - camera details
        .cameraNotFound          = "No camera found, gesture control will be unavailable",
        .cameraInUse             = "Camera is in use by another app, gesture control will be unavailable",
        .cameraPermissionDenied  = "Camera access denied, please allow access in system settings",

        // Error details (technical messages shown in expandable section)
        .detailWindowCreateFailed = "glfwCreateWindow() returned NULL.\n\n"
                                    "This usually means your GPU does not support OpenGL 4.4 Core Profile.\n"
                                    "Please update your graphics driver or check hardware compatibility.",
        .detailOpenGLLoadFailed   = "gladLoadGLLoader() returned false.\n\n"
                                    "Failed to load OpenGL function pointers.\n"
                                    "Please update your graphics driver.",
        .detailOpenGLVersionLow   = "Detected OpenGL version",
        .detailOpenGLRequired     = "Required: OpenGL 4.4 or higher",

        // Error messages - hand tracker details
        .palmModelLoadFailed = "Failed to load palm detection model",
        .handModelLoadFailed = "Failed to load hand landmark model",

        // Error messages - fatal
        .unexpectedError  = "Unexpected error occurred, program needs to close",
        .calculationError = "Calculation error occurred, program needs to close",
        .stackOverflow    = "Stack overflow, program needs to close",
        .accessViolation  = "Program accessed invalid memory",

        // Crash report sections
        .sectionException  = "Exception Info",
        .sectionCallStack  = "Call Stack",
        .sectionSystem     = "System Environment",
        .sectionGraphics   = "Graphics Device",
        .sectionCamera     = "Camera",
        .sectionAppState   = "Application State",
        .sectionRecentLogs = "Recent Logs",

        // Crash report fields
        .fieldType             = "Type",
        .fieldAddress          = "Address",
        .fieldOperation        = "Operation",
        .fieldStage            = "Crash Stage",
        .fieldOS               = "OS",
        .fieldLanguage         = "Language",
        .fieldMemory           = "Memory",
        .fieldGPU              = "GPU",
        .fieldDriver           = "Driver",
        .fieldOpenGL           = "OpenGL",
        .fieldVRAM             = "VRAM",
        .fieldDisplay          = "Display",
        .fieldCameraDevice     = "Device",
        .fieldCameraIndex      = "Index",
        .fieldCameraResolution = "Resolution",
        .fieldCameraStatus     = "Status",
        .fieldVersion          = "Version",
        .fieldUptime           = "Uptime",
        .fieldFrame            = "Frame",
        .fieldParticles        = "Particles",
        .fieldLOD              = "LOD",
        .fieldHandTracking     = "Hand Tracking",

        // Status values
        .statusActive   = "Active",
        .statusInactive = "Inactive",
        .statusEnabled  = "Enabled",
        .statusDisabled = "Disabled",
        .statusUnknown  = "Unknown",
        .statusRead     = "Read",
        .statusWrite    = "Write",

        // Crash analyzer
        .crashAnalyzerTitle  = "Crash Analyzer",
        .crashAnalyzerButton = "Open Crash Analyzer",
        .pdbFile             = "PDB File",
        .dropOrSelect        = "Drop file or click to select",
        .crashReport         = "Crash Report",
        .pasteReport         = "Paste crash report content",
        .analyze             = "Analyze",
        .analysisResult      = "Analysis Result",
        .copyResult          = "Copy Result",
        .pdbLoaded           = "Loaded",
        .pdbSize             = "Size",
        .noPdbLoaded         = "No PDB file loaded",
        .analysisSuccess     = "Analysis complete",
        .analysisNoAddresses = "No addresses found to analyze",
        .selectPdbFile       = "Select PDB File",
        .paste               = "Paste",
        .clear               = "Clear",

        // Debug panel
        .debugPanelTitle     = "Debug Panel",
        .sectionPerformance  = "Performance",
        .sectionHandTracking = "Hand Tracking",
        .sectionVisuals      = "Visuals",
        .sectionWindow       = "Window",
        .sectionLog          = "Log",
        .fps                 = "FPS",
        .particles           = "Particles",
        .pixelRatio          = "Pixel Ratio",
        .resolution          = "Resolution",
        .handDetected        = "Hand Detected",
        .yes                 = "Yes",
        .no                  = "No",
        .scale               = "Scale",
        .animationScale      = "Animation Scale",
        .animationRotX       = "Animation RotX",
        .animationRotY       = "Animation RotY",
        .showCameraDebug     = "Show Camera Debug Window",
        .darkMode            = "Dark Mode",
        .glassBlur           = "Glass Blur",
        .blurStrength        = "Blur Strength",
        .backdrop            = "Backdrop",
        .fullscreen          = "Fullscreen",
        .transparent         = "Transparent",
        .clearLog            = "Clear",
        .copyAllLog          = "Copy All",

        // Advanced section
        .sectionAdvanced = "Advanced",
        .simdMode        = "SIMD Mode",
        .simdAuto        = "Auto",
        .simdAVX2        = "AVX2",
        .simdSSE         = "SSE",
        .simdScalar      = "Scalar",
        .simdCurrent     = "Current Impl",

        // VSync
        .vsync         = "VSync",
        .vsyncOff      = "Off",
        .vsyncOn       = "On",
        .vsyncAdaptive = "Adaptive",
    };
    return en;
}

// Get strings for current system language
inline const Strings& Get() {
    static bool isChinese = IsChineseSystem();
    return isChinese ? GetChinese() : GetEnglish();
}

// Get app version string
inline const char* GetVersion() {
#ifdef APP_VERSION
    return APP_VERSION;
#else
    return "dev";
#endif
}

} // namespace i18n
