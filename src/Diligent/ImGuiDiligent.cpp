// ImGuiDiligent.cpp - ImGui Diligent Engine 渲染器
// 基于 Diligent 抽象层实现，参考 imgui_impl_dx12.cpp 的渲染逻辑

#include "ImGuiDiligent.h"

#if defined(__APPLE__)
#import <Cocoa/Cocoa.h>
#include "backends/imgui_impl_osx.h"
#include <unistd.h>
#else
#include <windows.h>
#include "backends/imgui_impl_win32.h"
#endif

#include <cstring>

#include "DeviceContext.h"
#include "RenderDevice.h"
#include "SwapChain.h"
#include "imgui.h"

#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#endif

#if !defined(__APPLE__)
// imgui_impl_win32.h 将该声明放在 #if 0 中（避免强制包含 <windows.h> 的依赖），这里显式前置声明。
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

using namespace Diligent;

namespace ParticleSaturn::UI {

namespace {

// Draw 验证开关：Release 系列全部关闭（见同项目 DiligentBackend.cpp 的说明）。
#if defined(NDEBUG)
static constexpr DRAW_FLAGS kDrawVerifyFlags = DRAW_FLAG_NONE;
#else
static constexpr DRAW_FLAGS kDrawVerifyFlags = DRAW_FLAG_VERIFY_ALL;
#endif

// ImGui 顶点常量缓冲结构
struct ImGuiConstants {
    float ProjectionMatrix[4][4];
};

// HLSL 着色器（D3D11/D3D12）
static constexpr char kHlslVS[] = R"(
cbuffer Constants : register(b0)
{
    float4x4 ProjectionMatrix;
};

struct VS_INPUT
{
    float2 pos : ATTRIB0;
    float2 uv  : ATTRIB1;
    float4 col : ATTRIB2;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};

PS_INPUT main(VS_INPUT input)
{
    PS_INPUT output;
    output.pos = mul(ProjectionMatrix, float4(input.pos.xy, 0.0, 1.0));
    output.col = input.col;
    output.uv  = input.uv;
    return output;
}
)";

static constexpr char kHlslPS[] = R"(
struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};

Texture2D    g_Texture;
SamplerState g_Texture_sampler;

float4 main(PS_INPUT input) : SV_Target
{
    return input.col * g_Texture.Sample(g_Texture_sampler, input.uv);
}
)";

// GLSL 着色器（Vulkan/OpenGL）
static constexpr char kGlslVS[] = R"(
layout(std140, binding = 0) uniform Constants
{
    mat4 ProjectionMatrix;
};

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outUV;

void main()
{
    gl_Position = ProjectionMatrix * vec4(inPos.xy, 0.0, 1.0);
    outColor = inColor;
    outUV = inUV;
}
)";

static constexpr char kGlslPS[] = R"(
layout(location = 0) in vec4 inColor;
layout(location = 1) in vec2 inUV;

layout(binding = 1) uniform sampler2D g_Texture;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = inColor * texture(g_Texture, inUV);
}
)";

RefCntAutoPtr<IShader> CreateShaderFromSource(IRenderDevice* device, const char* name, SHADER_TYPE type,
                                              const char* source, SHADER_SOURCE_LANGUAGE lang) {
    ShaderCreateInfo sci;
    sci.SourceLanguage  = lang;
    sci.Desc.ShaderType = type;
    sci.Desc.Name       = name;
    sci.Source          = source;
    sci.EntryPoint      = "main";

    RefCntAutoPtr<IShader> shader;
    device->CreateShader(sci, &shader);
    return shader;
}

} // namespace

// 全局 ImGuiDiligent 实例指针（供 MD3 回调使用）
static ImGuiDiligent* g_imguiDiligentInstance = nullptr;

ImGuiDiligent* GetImGuiDiligentInstance() {
    return g_imguiDiligentInstance;
}

void SetImGuiDiligentInstance(ImGuiDiligent* instance) {
    g_imguiDiligentInstance = instance;
}

ImGuiDiligent::~ImGuiDiligent() {
    Shutdown();
}

bool ImGuiDiligent::Init(HWND hwnd, Render::Backend backend, IRenderDevice* device, ISwapChain* swapChain) {
    if (swapChain == nullptr) {
        return false;
    }
    const auto& scDesc = swapChain->GetDesc();
    return Init(hwnd, backend, device, scDesc.ColorBufferFormat, scDesc.Width, scDesc.Height);
}

bool ImGuiDiligent::Init(HWND hwnd, Render::Backend backend, IRenderDevice* device, TEXTURE_FORMAT rtvFormat,
                         uint32_t width, uint32_t height) {
    if (initialized_) {
        return true;
    }
    if (device == nullptr || hwnd == nullptr || width == 0 || height == 0) {
        return false;
    }

    hwnd_    = hwnd;
    backend_ = backend;
    device_  = device;

    // 创建 ImGui 上下文
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // 禁用 ini 文件

#if defined(__APPLE__)
    if (!ImGui_ImplOSX_Init(static_cast<NSView*>(hwnd))) {
#else
    if (!ImGui_ImplWin32_Init(hwnd)) {
#endif
        ImGui::DestroyContext();
        return false;
    }

    // 创建 Diligent 渲染资源
    if (!CreatePipelineStates(device, rtvFormat)) {
#if defined(__APPLE__)
        ImGui_ImplOSX_Shutdown();
#else
        ImGui_ImplWin32_Shutdown();
#endif
        ImGui::DestroyContext();
        return false;
    }

    if (!CreateFontTexture(device)) {
#if defined(__APPLE__)
        ImGui_ImplOSX_Shutdown();
#else
        ImGui_ImplWin32_Shutdown();
#endif
        ImGui::DestroyContext();
        return false;
    }

    if (!CreateDepthStencilBuffer(device, width, height)) {
#if defined(__APPLE__)
        ImGui_ImplOSX_Shutdown();
#else
        ImGui_ImplWin32_Shutdown();
#endif
        ImGui::DestroyContext();
        return false;
    }

    // 初始化顶点/索引缓冲
    if (!CreateBuffers(device, 5000, 10000)) {
#if defined(__APPLE__)
        ImGui_ImplOSX_Shutdown();
#else
        ImGui_ImplWin32_Shutdown();
#endif
        ImGui::DestroyContext();
        return false;
    }

    // 设置后端名称
    io.BackendRendererName = "imgui_impl_diligent";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

    // 设置全局实例（供 MD3 回调使用）
    g_imguiDiligentInstance = this;

    initialized_ = true;
    return true;
}

void ImGuiDiligent::Shutdown() {
    if (!initialized_) {
        return;
    }

    // 清除全局实例
    if (g_imguiDiligentInstance == this) {
        g_imguiDiligentInstance = nullptr;
    }

    pso_.Release();
    psoStencilWrite_.Release();
    psoStencilTest_.Release();
    srb_.Release();
    srbStencilWrite_.Release();
    srbStencilTest_.Release();
    texVar_             = nullptr;
    texVarStencilWrite_ = nullptr;
    texVarStencilTest_  = nullptr;
    vertexBuffer_.Release();
    indexBuffer_.Release();
    constantBuffer_.Release();
    fontTexture_.Release();
    fontSRV_.Release();
    depthStencilTexture_.Release();
    dsv_.Release();
    device_.Release();

#if defined(__APPLE__)
    ImGui_ImplOSX_Shutdown();
#else
    ImGui_ImplWin32_Shutdown();
#endif
    ImGui::DestroyContext();

    initialized_ = false;
}

void ImGuiDiligent::NewFrame() {
    if (!initialized_) {
        return;
    }
#if defined(__APPLE__)
    ImGui_ImplOSX_NewFrame(static_cast<NSView*>(hwnd_));
#else
    ImGui_ImplWin32_NewFrame();
#endif
    ImGui::NewFrame();
}

bool ImGuiDiligent::HandleWin32Message(HWND hwnd, unsigned int msg, unsigned long long wParam, long long lParam) {
#if defined(__APPLE__)
    return false;
#else
    // 注意：ImGui Win32 后端的处理函数在全局命名空间，避免在 ParticleSaturn::UI 命名空间下产生未定义符号
    return ::ImGui_ImplWin32_WndProcHandler(hwnd, msg, static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam)) != 0;
#endif
}

void ImGuiDiligent::Render(IDeviceContext* context, ITextureView* rtv) {
    if (!initialized_ || context == nullptr || rtv == nullptr) {
        return;
    }

    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData == nullptr || drawData->TotalVtxCount == 0) {
        return;
    }

    // 检查并增长缓冲区
    if (drawData->TotalVtxCount > vertexBufferSize_ || drawData->TotalIdxCount > indexBufferSize_) {
        CreateBuffers(device_, drawData->TotalVtxCount + 5000, drawData->TotalIdxCount + 10000);
    }

    // 上传顶点和索引数据
    {
        PVoid vtxMapped = nullptr;
        PVoid idxMapped = nullptr;
        context->MapBuffer(vertexBuffer_, MAP_WRITE, MAP_FLAG_DISCARD, vtxMapped);
        context->MapBuffer(indexBuffer_, MAP_WRITE, MAP_FLAG_DISCARD, idxMapped);

        if (vtxMapped != nullptr && idxMapped != nullptr) {
            ImDrawVert* vtxDst = static_cast<ImDrawVert*>(vtxMapped);
            ImDrawIdx*  idxDst = static_cast<ImDrawIdx*>(idxMapped);

            for (int n = 0; n < drawData->CmdListsCount; n++) {
                const ImDrawList* cmdList = drawData->CmdLists[n];
                memcpy(vtxDst, cmdList->VtxBuffer.Data, cmdList->VtxBuffer.Size * sizeof(ImDrawVert));
                memcpy(idxDst, cmdList->IdxBuffer.Data, cmdList->IdxBuffer.Size * sizeof(ImDrawIdx));
                vtxDst += cmdList->VtxBuffer.Size;
                idxDst += cmdList->IdxBuffer.Size;
            }
        }

        context->UnmapBuffer(vertexBuffer_, MAP_WRITE);
        context->UnmapBuffer(indexBuffer_, MAP_WRITE);
    }

    // 设置正交投影矩阵
    {
        float L = drawData->DisplayPos.x;
        float R = drawData->DisplayPos.x + drawData->DisplaySize.x;
        float T = drawData->DisplayPos.y;
        float B = drawData->DisplayPos.y + drawData->DisplaySize.y;

        ImGuiConstants constants{};
        // 列主序矩阵（Diligent/HLSL/GLSL 兼容）
        constants.ProjectionMatrix[0][0] = 2.0f / (R - L);
        constants.ProjectionMatrix[1][1] = 2.0f / (T - B);
        constants.ProjectionMatrix[2][2] = 0.5f;
        constants.ProjectionMatrix[3][0] = (R + L) / (L - R);
        constants.ProjectionMatrix[3][1] = (T + B) / (B - T);
        constants.ProjectionMatrix[3][2] = 0.5f;
        constants.ProjectionMatrix[3][3] = 1.0f;

        PVoid mapped = nullptr;
        context->MapBuffer(constantBuffer_, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
        if (mapped != nullptr) {
            memcpy(mapped, &constants, sizeof(constants));
            context->UnmapBuffer(constantBuffer_, MAP_WRITE);
        }
    }

    // 设置渲染目标（包含 DepthStencil）
    // 检查 DSV 尺寸是否匹配，如果不匹配则重新创建
    auto displayWidth  = static_cast<unsigned int>(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    auto displayHeight = static_cast<unsigned int>(drawData->DisplaySize.y * drawData->FramebufferScale.y);
    if (dsv_ != nullptr && (cachedWidth_ != displayWidth || cachedHeight_ != displayHeight)) {
        // 需要重新创建 DSV
        TextureDesc dsDesc{};
        dsDesc.Name      = "ImGui DepthStencil";
        dsDesc.Type      = RESOURCE_DIM_TEX_2D;
        dsDesc.Width     = displayWidth;
        dsDesc.Height    = displayHeight;
        dsDesc.MipLevels = 1;
        dsDesc.Format    = TEX_FORMAT_D24_UNORM_S8_UINT;
        dsDesc.BindFlags = BIND_DEPTH_STENCIL;
        dsDesc.Usage     = USAGE_DEFAULT;

        depthStencilTexture_.Release();
        dsv_.Release();

        device_->CreateTexture(dsDesc, nullptr, &depthStencilTexture_);
        if (depthStencilTexture_ != nullptr) {
            dsv_          = depthStencilTexture_->GetDefaultView(TEXTURE_VIEW_DEPTH_STENCIL);
            cachedWidth_  = displayWidth;
            cachedHeight_ = displayHeight;
        }
    }

    ITextureView* dsvPtr = dsv_.RawPtr();
    context->SetRenderTargets(1, &rtv, dsvPtr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // 清除 Stencil 缓冲
    if (dsvPtr != nullptr) {
        context->ClearDepthStencil(dsvPtr, CLEAR_STENCIL_FLAG, 1.0f, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }

    // 保存当前上下文供回调使用
    currentContext_ = context;
    currentRTV_     = rtv;

    // 设置视口
    Viewport vp{};
    vp.Width    = drawData->DisplaySize.x * drawData->FramebufferScale.x;
    vp.Height   = drawData->DisplaySize.y * drawData->FramebufferScale.y;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context->SetViewports(1, &vp, static_cast<Uint32>(vp.Width), static_cast<Uint32>(vp.Height));

    // 绑定管线和资源
    context->SetPipelineState(pso_);
    context->CommitShaderResources(srb_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // 绑定顶点和索引缓冲
    IBuffer* vbs[]     = {vertexBuffer_};
    Uint64   offsets[] = {0};
    context->SetVertexBuffers(0, 1, vbs, offsets, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                              SET_VERTEX_BUFFERS_FLAG_RESET);
    context->SetIndexBuffer(indexBuffer_, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // 渲染命令列表
    int    globalVtxOffset = 0;
    int    globalIdxOffset = 0;
    ImVec2 clipOff         = drawData->DisplayPos;
    ImVec2 clipScale       = drawData->FramebufferScale;

    // 跟踪当前绑定的纹理，避免重复绑定
    ITextureView* currentTexture = nullptr;

    for (int n = 0; n < drawData->CmdListsCount; n++) {
        const ImDrawList* cmdList = drawData->CmdLists[n];
        for (int cmdIdx = 0; cmdIdx < cmdList->CmdBuffer.Size; cmdIdx++) {
            const ImDrawCmd* pcmd = &cmdList->CmdBuffer[cmdIdx];

            if (pcmd->UserCallback != nullptr) {
                if (pcmd->UserCallback == ImDrawCallback_ResetRenderState) {
                    // 重置渲染状态
                    stencilMode_ = StencilMode::Disabled;
                    context->SetPipelineState(pso_);
                    context->CommitShaderResources(srb_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

                    // ResetRenderState 的语义应恢复 ImGui 渲染器的关键 GPU 状态。
                    // 这里至少需要恢复 viewport + VB/IB，否则用户回调（例如自定义 PSO 绘制）改变绑定后，后续 ImGui draw
                    // 会异常。
                    context->SetViewports(1, &vp, static_cast<Uint32>(vp.Width), static_cast<Uint32>(vp.Height));

                    IBuffer* resetVBs[]     = {vertexBuffer_};
                    Uint64   resetOffsets[] = {0};
                    context->SetVertexBuffers(0, 1, resetVBs, resetOffsets, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                              SET_VERTEX_BUFFERS_FLAG_RESET);
                    context->SetIndexBuffer(indexBuffer_, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

                    currentTexture = nullptr; // 强制下次重新绑定纹理
                } else {
                    // 执行用户回调
                    pcmd->UserCallback(cmdList, pcmd);
                    // 回调可能改变了 Stencil 模式，应用当前模式
                    ApplyStencilMode(context);
                    currentTexture = nullptr; // 强制重新绑定
                }
            } else {
                // 计算裁剪矩形
                ImVec2 clipMin((pcmd->ClipRect.x - clipOff.x) * clipScale.x,
                               (pcmd->ClipRect.y - clipOff.y) * clipScale.y);
                ImVec2 clipMax((pcmd->ClipRect.z - clipOff.x) * clipScale.x,
                               (pcmd->ClipRect.w - clipOff.y) * clipScale.y);

                if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y) {
                    continue;
                }

                // 获取当前绘制命令的纹理
                ITextureView* texView = reinterpret_cast<ITextureView*>(pcmd->GetTexID());
                if (texView == nullptr) {
                    texView = fontSRV_.RawPtr(); // 默认使用字体纹理
                }

                // 只有纹理变化时才重新绑定
                if (texView != currentTexture) {
                    currentTexture = texView;
                    // 根据当前 Stencil 模式选择正确的 SRB
                    IShaderResourceBinding*  currentSrb = srb_.RawPtr();
                    IShaderResourceVariable* texVar     = texVar_;
                    if (stencilMode_ == StencilMode::WriteIncr || stencilMode_ == StencilMode::WriteDecr) {
                        currentSrb = srbStencilWrite_.RawPtr();
                        texVar     = texVarStencilWrite_;
                    } else if (stencilMode_ == StencilMode::TestEqual) {
                        currentSrb = srbStencilTest_.RawPtr();
                        texVar     = texVarStencilTest_;
                    }
                    if (texVar == nullptr && currentSrb != nullptr) {
                        // 兜底：理论上 CreatePipelineStates() 会缓存变量指针；若因重建/异常未缓存，这里退化为一次查找。
                        texVar = currentSrb->GetVariableByName(SHADER_TYPE_PIXEL, "g_Texture");
                    }
                    if (texVar != nullptr) {
                        texVar->Set(texView);
                    }
                    context->CommitShaderResources(currentSrb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                }

                // 设置裁剪矩形
                Diligent::Rect scissor{};
                scissor.left   = static_cast<Int32>(clipMin.x);
                scissor.top    = static_cast<Int32>(clipMin.y);
                scissor.right  = static_cast<Int32>(clipMax.x);
                scissor.bottom = static_cast<Int32>(clipMax.y);
                context->SetScissorRects(1, &scissor, static_cast<Uint32>(vp.Width), static_cast<Uint32>(vp.Height));

                // 绘制
                DrawIndexedAttribs drawAttribs{};
                drawAttribs.NumIndices         = pcmd->ElemCount;
                drawAttribs.IndexType          = sizeof(ImDrawIdx) == 2 ? VT_UINT16 : VT_UINT32;
                drawAttribs.FirstIndexLocation = pcmd->IdxOffset + globalIdxOffset;
                drawAttribs.BaseVertex         = pcmd->VtxOffset + globalVtxOffset;
                drawAttribs.Flags              = kDrawVerifyFlags;
                context->DrawIndexed(drawAttribs);
            }
        }
        globalIdxOffset += cmdList->IdxBuffer.Size;
        globalVtxOffset += cmdList->VtxBuffer.Size;
    }

    // 清理当前上下文指针和重置 Stencil 模式
    currentContext_ = nullptr;
    currentRTV_     = nullptr;
    stencilMode_    = StencilMode::Disabled;
}

bool ImGuiDiligent::CreatePipelineStates(IRenderDevice* device, TEXTURE_FORMAT rtvFormat) {
    const bool isVulkan = (backend_ == Render::Backend::Vulkan);
    const auto lang     = isVulkan ? SHADER_SOURCE_LANGUAGE_GLSL : SHADER_SOURCE_LANGUAGE_HLSL;

    auto vs = CreateShaderFromSource(device, "ImGui VS", SHADER_TYPE_VERTEX, isVulkan ? kGlslVS : kHlslVS, lang);
    auto ps = CreateShaderFromSource(device, "ImGui PS", SHADER_TYPE_PIXEL, isVulkan ? kGlslPS : kHlslPS, lang);
    if (vs == nullptr || ps == nullptr) {
        return false;
    }

    // 创建常量缓冲
    {
        BufferDesc cbDesc{};
        cbDesc.Name           = "ImGui Constants";
        cbDesc.Size           = sizeof(ImGuiConstants);
        cbDesc.Usage          = USAGE_DYNAMIC;
        cbDesc.BindFlags      = BIND_UNIFORM_BUFFER;
        cbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;

        device->CreateBuffer(cbDesc, nullptr, &constantBuffer_);
        if (constantBuffer_ == nullptr) {
            return false;
        }
    }

    // 顶点输入布局（ImDrawVert: pos, uv, col）
    LayoutElement layoutElems[] = {
        {0, 0, 2, VT_FLOAT32, False, offsetof(ImDrawVert, pos)},
        {1, 0, 2, VT_FLOAT32, False, offsetof(ImDrawVert, uv)},
        {2, 0, 4, VT_UINT8, True, offsetof(ImDrawVert, col)},
    };

    // 资源变量
    ShaderResourceVariableDesc vars[] = {
        {SHADER_TYPE_PIXEL, "g_Texture", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
    };

    // 采样器
    SamplerDesc sampDesc{};
    sampDesc.MinFilter = FILTER_TYPE_LINEAR;
    sampDesc.MagFilter = FILTER_TYPE_LINEAR;
    sampDesc.MipFilter = FILTER_TYPE_LINEAR;
    sampDesc.AddressU  = TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV  = TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW  = TEXTURE_ADDRESS_CLAMP;

    // 采样器 - Vulkan/GLSL 使用组合采样器 (sampler2D)，采样器名称直接用纹理名
    // D3D12/HLSL 使用分离采样器，采样器名称需要 "_sampler" 后缀
    const char*          samplerName     = (backend_ == Render::Backend::Vulkan) ? "g_Texture" : "g_Texture_sampler";
    ImmutableSamplerDesc imtblSamplers[] = {
        {SHADER_TYPE_PIXEL, samplerName, sampDesc},
    };

    // ========== PSO 1: 普通渲染（无 Stencil）==========
    {
        GraphicsPipelineStateCreateInfo psoCI{};
        psoCI.PSODesc.Name         = "ImGui PSO";
        psoCI.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

        psoCI.GraphicsPipeline.NumRenderTargets  = 1;
        psoCI.GraphicsPipeline.RTVFormats[0]     = rtvFormat;
        psoCI.GraphicsPipeline.DSVFormat         = backend_ == Render::Backend::Vulkan
                                                        ? TEX_FORMAT_UNKNOWN
                                                        : TEX_FORMAT_D24_UNORM_S8_UINT;
        psoCI.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        psoCI.GraphicsPipeline.RasterizerDesc.CullMode      = CULL_MODE_NONE;
        psoCI.GraphicsPipeline.RasterizerDesc.ScissorEnable = True;

        psoCI.GraphicsPipeline.DepthStencilDesc.DepthEnable   = False;
        psoCI.GraphicsPipeline.DepthStencilDesc.StencilEnable = False;

        auto& rt0                 = psoCI.GraphicsPipeline.BlendDesc.RenderTargets[0];
        rt0.BlendEnable           = True;
        rt0.SrcBlend              = BLEND_FACTOR_SRC_ALPHA;
        rt0.DestBlend             = BLEND_FACTOR_INV_SRC_ALPHA;
        rt0.BlendOp               = BLEND_OPERATION_ADD;
        rt0.SrcBlendAlpha         = BLEND_FACTOR_ONE;
        rt0.DestBlendAlpha        = BLEND_FACTOR_INV_SRC_ALPHA;
        rt0.BlendOpAlpha          = BLEND_OPERATION_ADD;
        rt0.RenderTargetWriteMask = COLOR_MASK_ALL;

        psoCI.GraphicsPipeline.InputLayout.NumElements    = _countof(layoutElems);
        psoCI.GraphicsPipeline.InputLayout.LayoutElements = layoutElems;

        psoCI.PSODesc.ResourceLayout.NumVariables         = _countof(vars);
        psoCI.PSODesc.ResourceLayout.Variables            = vars;
        psoCI.PSODesc.ResourceLayout.NumImmutableSamplers = _countof(imtblSamplers);
        psoCI.PSODesc.ResourceLayout.ImmutableSamplers    = imtblSamplers;

        psoCI.pVS = vs;
        psoCI.pPS = ps;

        device->CreateGraphicsPipelineState(psoCI, &pso_);
        if (pso_ == nullptr) {
            return false;
        }

        if (auto* var = pso_->GetStaticVariableByName(SHADER_TYPE_VERTEX, "Constants"); var != nullptr) {
            var->Set(constantBuffer_);
        }
        pso_->CreateShaderResourceBinding(&srb_, true);
        texVar_ = (srb_ != nullptr) ? srb_->GetVariableByName(SHADER_TYPE_PIXEL, "g_Texture") : nullptr;
    }

    // ========== PSO 2: Stencil 写入（禁止颜色写入）==========
    {
        GraphicsPipelineStateCreateInfo psoCI{};
        psoCI.PSODesc.Name         = "ImGui PSO StencilWrite";
        psoCI.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

        psoCI.GraphicsPipeline.NumRenderTargets  = 1;
        psoCI.GraphicsPipeline.RTVFormats[0]     = rtvFormat;
        psoCI.GraphicsPipeline.DSVFormat         = backend_ == Render::Backend::Vulkan
                                                        ? TEX_FORMAT_UNKNOWN
                                                        : TEX_FORMAT_D24_UNORM_S8_UINT;
        psoCI.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        psoCI.GraphicsPipeline.RasterizerDesc.CullMode      = CULL_MODE_NONE;
        psoCI.GraphicsPipeline.RasterizerDesc.ScissorEnable = True;

        psoCI.GraphicsPipeline.DepthStencilDesc.DepthEnable      = False;
        psoCI.GraphicsPipeline.DepthStencilDesc.StencilEnable    = True;
        psoCI.GraphicsPipeline.DepthStencilDesc.StencilReadMask  = 0xFF;
        psoCI.GraphicsPipeline.DepthStencilDesc.StencilWriteMask = 0xFF;
        // Front face: 总是通过，替换 Stencil 值
        psoCI.GraphicsPipeline.DepthStencilDesc.FrontFace.StencilFunc        = COMPARISON_FUNC_ALWAYS;
        psoCI.GraphicsPipeline.DepthStencilDesc.FrontFace.StencilPassOp      = STENCIL_OP_REPLACE;
        psoCI.GraphicsPipeline.DepthStencilDesc.FrontFace.StencilFailOp      = STENCIL_OP_KEEP;
        psoCI.GraphicsPipeline.DepthStencilDesc.FrontFace.StencilDepthFailOp = STENCIL_OP_KEEP;
        // Back face 同样设置
        psoCI.GraphicsPipeline.DepthStencilDesc.BackFace = psoCI.GraphicsPipeline.DepthStencilDesc.FrontFace;

        // 禁止颜色写入
        auto& rt0                 = psoCI.GraphicsPipeline.BlendDesc.RenderTargets[0];
        rt0.BlendEnable           = False;
        rt0.RenderTargetWriteMask = COLOR_MASK_NONE;

        psoCI.GraphicsPipeline.InputLayout.NumElements    = _countof(layoutElems);
        psoCI.GraphicsPipeline.InputLayout.LayoutElements = layoutElems;

        psoCI.PSODesc.ResourceLayout.NumVariables         = _countof(vars);
        psoCI.PSODesc.ResourceLayout.Variables            = vars;
        psoCI.PSODesc.ResourceLayout.NumImmutableSamplers = _countof(imtblSamplers);
        psoCI.PSODesc.ResourceLayout.ImmutableSamplers    = imtblSamplers;

        psoCI.pVS = vs;
        psoCI.pPS = ps;

        device->CreateGraphicsPipelineState(psoCI, &psoStencilWrite_);
        if (psoStencilWrite_ == nullptr) {
            return false;
        }

        if (auto* var = psoStencilWrite_->GetStaticVariableByName(SHADER_TYPE_VERTEX, "Constants"); var != nullptr) {
            var->Set(constantBuffer_);
        }
        psoStencilWrite_->CreateShaderResourceBinding(&srbStencilWrite_, true);
        texVarStencilWrite_ = (srbStencilWrite_ != nullptr)
                                ? srbStencilWrite_->GetVariableByName(SHADER_TYPE_PIXEL, "g_Texture")
                                : nullptr;
    }

    // ========== PSO 3: Stencil 测试（相等时通过）==========
    {
        GraphicsPipelineStateCreateInfo psoCI{};
        psoCI.PSODesc.Name         = "ImGui PSO StencilTest";
        psoCI.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

        psoCI.GraphicsPipeline.NumRenderTargets  = 1;
        psoCI.GraphicsPipeline.RTVFormats[0]     = rtvFormat;
        psoCI.GraphicsPipeline.DSVFormat         = backend_ == Render::Backend::Vulkan
                                                        ? TEX_FORMAT_UNKNOWN
                                                        : TEX_FORMAT_D24_UNORM_S8_UINT;
        psoCI.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        psoCI.GraphicsPipeline.RasterizerDesc.CullMode      = CULL_MODE_NONE;
        psoCI.GraphicsPipeline.RasterizerDesc.ScissorEnable = True;

        psoCI.GraphicsPipeline.DepthStencilDesc.DepthEnable      = False;
        psoCI.GraphicsPipeline.DepthStencilDesc.StencilEnable    = True;
        psoCI.GraphicsPipeline.DepthStencilDesc.StencilReadMask  = 0xFF;
        psoCI.GraphicsPipeline.DepthStencilDesc.StencilWriteMask = 0x00; // 不写入 Stencil
        // Front face: 相等时通过
        psoCI.GraphicsPipeline.DepthStencilDesc.FrontFace.StencilFunc        = COMPARISON_FUNC_EQUAL;
        psoCI.GraphicsPipeline.DepthStencilDesc.FrontFace.StencilPassOp      = STENCIL_OP_KEEP;
        psoCI.GraphicsPipeline.DepthStencilDesc.FrontFace.StencilFailOp      = STENCIL_OP_KEEP;
        psoCI.GraphicsPipeline.DepthStencilDesc.FrontFace.StencilDepthFailOp = STENCIL_OP_KEEP;
        psoCI.GraphicsPipeline.DepthStencilDesc.BackFace = psoCI.GraphicsPipeline.DepthStencilDesc.FrontFace;

        // 正常混合
        auto& rt0                 = psoCI.GraphicsPipeline.BlendDesc.RenderTargets[0];
        rt0.BlendEnable           = True;
        rt0.SrcBlend              = BLEND_FACTOR_SRC_ALPHA;
        rt0.DestBlend             = BLEND_FACTOR_INV_SRC_ALPHA;
        rt0.BlendOp               = BLEND_OPERATION_ADD;
        rt0.SrcBlendAlpha         = BLEND_FACTOR_ONE;
        rt0.DestBlendAlpha        = BLEND_FACTOR_INV_SRC_ALPHA;
        rt0.BlendOpAlpha          = BLEND_OPERATION_ADD;
        rt0.RenderTargetWriteMask = COLOR_MASK_ALL;

        psoCI.GraphicsPipeline.InputLayout.NumElements    = _countof(layoutElems);
        psoCI.GraphicsPipeline.InputLayout.LayoutElements = layoutElems;

        psoCI.PSODesc.ResourceLayout.NumVariables         = _countof(vars);
        psoCI.PSODesc.ResourceLayout.Variables            = vars;
        psoCI.PSODesc.ResourceLayout.NumImmutableSamplers = _countof(imtblSamplers);
        psoCI.PSODesc.ResourceLayout.ImmutableSamplers    = imtblSamplers;

        psoCI.pVS = vs;
        psoCI.pPS = ps;

        device->CreateGraphicsPipelineState(psoCI, &psoStencilTest_);
        if (psoStencilTest_ == nullptr) {
            return false;
        }

        if (auto* var = psoStencilTest_->GetStaticVariableByName(SHADER_TYPE_VERTEX, "Constants"); var != nullptr) {
            var->Set(constantBuffer_);
        }
        psoStencilTest_->CreateShaderResourceBinding(&srbStencilTest_, true);
        texVarStencilTest_ =
            (srbStencilTest_ != nullptr) ? srbStencilTest_->GetVariableByName(SHADER_TYPE_PIXEL, "g_Texture") : nullptr;
    }

    return srb_ != nullptr && srbStencilWrite_ != nullptr && srbStencilTest_ != nullptr;
}

bool ImGuiDiligent::CreateFontTexture(IRenderDevice* device) {
    ImGuiIO& io = ImGui::GetIO();

    // 获取 DPI 缩放（从窗口句柄获取）
    float dpiScale = 1.0f;
#if !defined(__APPLE__)
    if (hwnd_) {
        HDC hdc = GetDC(hwnd_);
        if (hdc) {
            int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
            dpiScale = static_cast<float>(dpiX) / 96.0f;
            ReleaseDC(hwnd_, hdc);
        }
    }
#endif

    // 加载自定义字体
    float fontSize = 16.0f * dpiScale;

    ImFontConfig fontConfig;
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 2;

#if defined(__APPLE__)
    const char* englishFonts[] = {"/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc"};
#else
    // 英文字体优先级：Cascadia Code → Cascadia Mono → Consolas → Arial
    const char* englishFonts[] = {"C:\\Windows\\Fonts\\CascadiaCode.ttf", "C:\\Windows\\Fonts\\CascadiaMono.ttf",
                                  "C:\\Windows\\Fonts\\consola.ttf", "C:\\Windows\\Fonts\\arial.ttf"};
#endif

    ImFont* mainFont = nullptr;
    for (const char* fontPath : englishFonts) {
#if defined(__APPLE__)
        if (access(fontPath, F_OK) == 0) {
#else
        if (GetFileAttributesA(fontPath) != INVALID_FILE_ATTRIBUTES) {
#endif
            mainFont = io.Fonts->AddFontFromFileTTF(fontPath, fontSize, &fontConfig);
            if (mainFont) {
                break;
            }
        }
    }

    // 如果没有找到任何字体，使用默认字体
    if (!mainFont) {
        mainFont = io.Fonts->AddFontDefault();
    }

#if !defined(__APPLE__)
    // 中文字体（MergeMode）：Deng.ttf → msyhl.ttc → msyh.ttc → simhei.ttf
    // 使用简体常用字形集合以缩小字体贴图（减少启动时间/显存占用）。如需全量汉字可改回 ChineseFull。
    const char* chineseFonts[] = {"C:\\Windows\\Fonts\\Deng.ttf", "C:\\Windows\\Fonts\\msyhl.ttc",
                                  "C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\simhei.ttf"};

    ImFontConfig chineseConfig;
    chineseConfig.MergeMode   = true;
    chineseConfig.OversampleH = 2;
    chineseConfig.OversampleV = 2;

    for (const char* fontPath : chineseFonts) {
        if (GetFileAttributesA(fontPath) != INVALID_FILE_ATTRIBUTES) {
            io.Fonts->AddFontFromFileTTF(fontPath, fontSize, &chineseConfig, io.Fonts->GetGlyphRangesChineseFull());
            break;
        }
    }
#endif

    unsigned char* pixels = nullptr;
    int            width  = 0;
    int            height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    if (pixels == nullptr || width <= 0 || height <= 0) {
        return false;
    }

    // 创建纹理
    TextureDesc texDesc{};
    texDesc.Name      = "ImGui Font Texture";
    texDesc.Type      = RESOURCE_DIM_TEX_2D;
    texDesc.Width     = static_cast<Uint32>(width);
    texDesc.Height    = static_cast<Uint32>(height);
    texDesc.MipLevels = 1;
    texDesc.Format    = TEX_FORMAT_RGBA8_UNORM;
    texDesc.BindFlags = BIND_SHADER_RESOURCE;
    texDesc.Usage     = USAGE_IMMUTABLE;

    TextureSubResData subResData{};
    subResData.pData  = pixels;
    subResData.Stride = static_cast<Uint32>(width * 4);

    TextureData texData{};
    texData.NumSubresources = 1;
    texData.pSubResources   = &subResData;

    device->CreateTexture(texDesc, &texData, &fontTexture_);
    if (fontTexture_ == nullptr) {
        return false;
    }

    fontSRV_ = fontTexture_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    if (fontSRV_ == nullptr) {
        return false;
    }

    // 绑定字体纹理到 SRB
    if (texVar_ != nullptr) {
        texVar_->Set(fontSRV_);
    }

    // 存储纹理 ID（ImGui 使用）
    io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(fontSRV_.RawPtr()));

    return true;
}

bool ImGuiDiligent::CreateBuffers(IRenderDevice* device, int vertexCount, int indexCount) {
    // 顶点缓冲
    {
        BufferDesc vbDesc{};
        vbDesc.Name           = "ImGui Vertex Buffer";
        vbDesc.Size           = static_cast<Uint32>(vertexCount * sizeof(ImDrawVert));
        vbDesc.Usage          = USAGE_DYNAMIC;
        vbDesc.BindFlags      = BIND_VERTEX_BUFFER;
        vbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;

        vertexBuffer_.Release();
        device->CreateBuffer(vbDesc, nullptr, &vertexBuffer_);
        if (vertexBuffer_ == nullptr) {
            return false;
        }
        vertexBufferSize_ = vertexCount;
    }

    // 索引缓冲
    {
        BufferDesc ibDesc{};
        ibDesc.Name           = "ImGui Index Buffer";
        ibDesc.Size           = static_cast<Uint32>(indexCount * sizeof(ImDrawIdx));
        ibDesc.Usage          = USAGE_DYNAMIC;
        ibDesc.BindFlags      = BIND_INDEX_BUFFER;
        ibDesc.CPUAccessFlags = CPU_ACCESS_WRITE;

        indexBuffer_.Release();
        device->CreateBuffer(ibDesc, nullptr, &indexBuffer_);
        if (indexBuffer_ == nullptr) {
            return false;
        }
        indexBufferSize_ = indexCount;
    }

    return true;
}

bool ImGuiDiligent::CreateDepthStencilBuffer(IRenderDevice* device, uint32_t width, uint32_t height) {
    cachedWidth_  = width;
    cachedHeight_ = height;

    // The Vulkan ImGui pipeline does not use depth, so keep the optional DSV
    // attachment disabled while stencil support is not part of this path.
    if (backend_ == Render::Backend::Vulkan) {
        depthStencilTexture_.Release();
        dsv_.Release();
        return true;
    }

    TextureDesc dsDesc{};
    dsDesc.Name      = "ImGui DepthStencil";
    dsDesc.Type      = RESOURCE_DIM_TEX_2D;
    dsDesc.Width     = width;
    dsDesc.Height    = height;
    dsDesc.MipLevels = 1;
    dsDesc.Format    = TEX_FORMAT_D24_UNORM_S8_UINT;
    dsDesc.BindFlags = BIND_DEPTH_STENCIL;
    dsDesc.Usage     = USAGE_DEFAULT;

    depthStencilTexture_.Release();
    dsv_.Release();

    device->CreateTexture(dsDesc, nullptr, &depthStencilTexture_);
    if (depthStencilTexture_ == nullptr) {
        return false;
    }

    dsv_ = depthStencilTexture_->GetDefaultView(TEXTURE_VIEW_DEPTH_STENCIL);
    return dsv_ != nullptr;
}

void ImGuiDiligent::SetStencilMode(StencilMode mode, int stencilRef) {
    stencilMode_ = mode;
    stencilRef_  = stencilRef;
}

void ImGuiDiligent::ApplyStencilMode(IDeviceContext* context) {
    IShaderResourceBinding* currentSrb = nullptr;
    IPipelineState*         currentPso = nullptr;

    switch (stencilMode_) {
    case StencilMode::WriteIncr:
    case StencilMode::WriteDecr:
        currentPso = psoStencilWrite_;
        currentSrb = srbStencilWrite_;
        break;
    case StencilMode::TestEqual:
        currentPso = psoStencilTest_;
        currentSrb = srbStencilTest_;
        break;
    case StencilMode::Disabled:
    default:
        currentPso = pso_;
        currentSrb = srb_;
        break;
    }

    context->SetPipelineState(currentPso);
    context->SetStencilRef(static_cast<Uint32>(stencilRef_));
}

} // namespace ParticleSaturn::UI
