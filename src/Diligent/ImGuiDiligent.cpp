// ImGuiDiligent.cpp - ImGui Diligent Engine 渲染器
// 基于 Diligent 抽象层实现，参考 imgui_impl_dx12.cpp 的渲染逻辑

#include "ImGuiDiligent.h"

#include "imgui.h"
#include "backends/imgui_impl_win32.h"

#include <windows.h>

#include "RenderDevice.h"
#include "DeviceContext.h"
#include "SwapChain.h"

#include <cstring>

using namespace Diligent;

namespace ParticleSaturn::UI {

namespace {

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
    sci.SourceLanguage = lang;
    sci.Desc.ShaderType = type;
    sci.Desc.Name = name;
    sci.Source = source;
    sci.EntryPoint = "main";

    RefCntAutoPtr<IShader> shader;
    device->CreateShader(sci, &shader);
    return shader;
}

} // namespace

ImGuiDiligent::~ImGuiDiligent() {
    Shutdown();
}

bool ImGuiDiligent::Init(HWND hwnd, Render::Backend backend, IRenderDevice* device, ISwapChain* swapChain) {
    if (initialized_) {
        return true;
    }
    if (device == nullptr || swapChain == nullptr || hwnd == nullptr) {
        return false;
    }

    hwnd_ = hwnd;
    backend_ = backend;
    device_ = device;

    // 创建 ImGui 上下文
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // 禁用 ini 文件

    // 初始化 Win32 平台后端
    if (!ImGui_ImplWin32_Init(hwnd)) {
        ImGui::DestroyContext();
        return false;
    }

    // 创建 Diligent 渲染资源
    if (!CreatePipelineState(device, swapChain)) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    if (!CreateFontTexture(device)) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    // 初始化顶点/索引缓冲
    if (!CreateBuffers(device, 5000, 10000)) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    // 设置后端名称
    io.BackendRendererName = "imgui_impl_diligent";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

    initialized_ = true;
    return true;
}

void ImGuiDiligent::Shutdown() {
    if (!initialized_) {
        return;
    }

    pso_.Release();
    srb_.Release();
    vertexBuffer_.Release();
    indexBuffer_.Release();
    constantBuffer_.Release();
    fontTexture_.Release();
    fontSRV_.Release();
    device_.Release();

    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    initialized_ = false;
}

void ImGuiDiligent::NewFrame() {
    if (!initialized_) {
        return;
    }
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

bool ImGuiDiligent::HandleWin32Message(HWND hwnd, unsigned int msg, unsigned long long wParam, long long lParam) {
    // 声明外部函数（定义在 imgui_impl_win32.cpp 中）
    extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    return ImGui_ImplWin32_WndProcHandler(hwnd, msg, static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam)) != 0;
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
            ImDrawIdx* idxDst = static_cast<ImDrawIdx*>(idxMapped);

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

    // 设置渲染目标
    context->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // 设置视口
    Viewport vp{};
    vp.Width = drawData->DisplaySize.x * drawData->FramebufferScale.x;
    vp.Height = drawData->DisplaySize.y * drawData->FramebufferScale.y;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context->SetViewports(1, &vp, static_cast<Uint32>(vp.Width), static_cast<Uint32>(vp.Height));

    // 绑定管线和资源
    context->SetPipelineState(pso_);
    context->CommitShaderResources(srb_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // 绑定顶点和索引缓冲
    IBuffer* vbs[] = {vertexBuffer_};
    Uint64 offsets[] = {0};
    context->SetVertexBuffers(0, 1, vbs, offsets, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, SET_VERTEX_BUFFERS_FLAG_RESET);
    context->SetIndexBuffer(indexBuffer_, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // 渲染命令列表
    int globalVtxOffset = 0;
    int globalIdxOffset = 0;
    ImVec2 clipOff = drawData->DisplayPos;
    ImVec2 clipScale = drawData->FramebufferScale;

    // 跟踪当前绑定的纹理，避免重复绑定
    ITextureView* currentTexture = nullptr;

    for (int n = 0; n < drawData->CmdListsCount; n++) {
        const ImDrawList* cmdList = drawData->CmdLists[n];
        for (int cmdIdx = 0; cmdIdx < cmdList->CmdBuffer.Size; cmdIdx++) {
            const ImDrawCmd* pcmd = &cmdList->CmdBuffer[cmdIdx];

            if (pcmd->UserCallback != nullptr) {
                if (pcmd->UserCallback == ImDrawCallback_ResetRenderState) {
                    // 重置渲染状态
                    context->SetPipelineState(pso_);
                    currentTexture = nullptr; // 强制下次重新绑定纹理
                } else {
                    pcmd->UserCallback(cmdList, pcmd);
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
                    if (auto* var = srb_->GetVariableByName(SHADER_TYPE_PIXEL, "g_Texture"); var != nullptr) {
                        var->Set(texView);
                    }
                    context->CommitShaderResources(srb_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                }

                // 设置裁剪矩形
                Rect scissor{};
                scissor.left = static_cast<Int32>(clipMin.x);
                scissor.top = static_cast<Int32>(clipMin.y);
                scissor.right = static_cast<Int32>(clipMax.x);
                scissor.bottom = static_cast<Int32>(clipMax.y);
                context->SetScissorRects(1, &scissor, static_cast<Uint32>(vp.Width), static_cast<Uint32>(vp.Height));

                // 绘制
                DrawIndexedAttribs drawAttribs{};
                drawAttribs.NumIndices = pcmd->ElemCount;
                drawAttribs.IndexType = sizeof(ImDrawIdx) == 2 ? VT_UINT16 : VT_UINT32;
                drawAttribs.FirstIndexLocation = pcmd->IdxOffset + globalIdxOffset;
                drawAttribs.BaseVertex = pcmd->VtxOffset + globalVtxOffset;
                drawAttribs.Flags = DRAW_FLAG_VERIFY_ALL;
                context->DrawIndexed(drawAttribs);
            }
        }
        globalIdxOffset += cmdList->IdxBuffer.Size;
        globalVtxOffset += cmdList->VtxBuffer.Size;
    }
}

bool ImGuiDiligent::CreatePipelineState(IRenderDevice* device, ISwapChain* swapChain) {
    const bool isVulkan = (backend_ == Render::Backend::Vulkan);
    const auto lang = isVulkan ? SHADER_SOURCE_LANGUAGE_GLSL : SHADER_SOURCE_LANGUAGE_HLSL;

    auto vs = CreateShaderFromSource(device, "ImGui VS", SHADER_TYPE_VERTEX,
                                      isVulkan ? kGlslVS : kHlslVS, lang);
    auto ps = CreateShaderFromSource(device, "ImGui PS", SHADER_TYPE_PIXEL,
                                      isVulkan ? kGlslPS : kHlslPS, lang);
    if (vs == nullptr || ps == nullptr) {
        return false;
    }

    // 创建常量缓冲
    {
        BufferDesc cbDesc{};
        cbDesc.Name = "ImGui Constants";
        cbDesc.Size = sizeof(ImGuiConstants);
        cbDesc.Usage = USAGE_DYNAMIC;
        cbDesc.BindFlags = BIND_UNIFORM_BUFFER;
        cbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;

        device->CreateBuffer(cbDesc, nullptr, &constantBuffer_);
        if (constantBuffer_ == nullptr) {
            return false;
        }
    }

    GraphicsPipelineStateCreateInfo psoCI{};
    psoCI.PSODesc.Name = "ImGui PSO";
    psoCI.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

    const auto& scDesc = swapChain->GetDesc();
    psoCI.GraphicsPipeline.NumRenderTargets = 1;
    psoCI.GraphicsPipeline.RTVFormats[0] = scDesc.ColorBufferFormat;
    psoCI.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;
    psoCI.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // 光栅化设置
    psoCI.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;
    psoCI.GraphicsPipeline.RasterizerDesc.ScissorEnable = True;

    // 深度设置
    psoCI.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;

    // Alpha 混合设置（与 imgui_impl_dx12 一致）
    auto& rt0 = psoCI.GraphicsPipeline.BlendDesc.RenderTargets[0];
    rt0.BlendEnable = True;
    rt0.SrcBlend = BLEND_FACTOR_SRC_ALPHA;
    rt0.DestBlend = BLEND_FACTOR_INV_SRC_ALPHA;
    rt0.BlendOp = BLEND_OPERATION_ADD;
    rt0.SrcBlendAlpha = BLEND_FACTOR_ONE;
    rt0.DestBlendAlpha = BLEND_FACTOR_INV_SRC_ALPHA;
    rt0.BlendOpAlpha = BLEND_OPERATION_ADD;
    rt0.RenderTargetWriteMask = COLOR_MASK_ALL;

    // 顶点输入布局（ImDrawVert: pos, uv, col）
    LayoutElement layoutElems[] = {
        {0, 0, 2, VT_FLOAT32, False, offsetof(ImDrawVert, pos)},
        {1, 0, 2, VT_FLOAT32, False, offsetof(ImDrawVert, uv)},
        {2, 0, 4, VT_UINT8,   True,  offsetof(ImDrawVert, col)},
    };
    psoCI.GraphicsPipeline.InputLayout.NumElements = _countof(layoutElems);
    psoCI.GraphicsPipeline.InputLayout.LayoutElements = layoutElems;

    // 资源变量
    ShaderResourceVariableDesc vars[] = {
        {SHADER_TYPE_PIXEL, "g_Texture", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
    };
    psoCI.PSODesc.ResourceLayout.NumVariables = _countof(vars);
    psoCI.PSODesc.ResourceLayout.Variables = vars;

    // 采样器
    SamplerDesc sampDesc{};
    sampDesc.MinFilter = FILTER_TYPE_LINEAR;
    sampDesc.MagFilter = FILTER_TYPE_LINEAR;
    sampDesc.MipFilter = FILTER_TYPE_LINEAR;
    sampDesc.AddressU = TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = TEXTURE_ADDRESS_CLAMP;

    ImmutableSamplerDesc imtblSamplers[] = {
        {SHADER_TYPE_PIXEL, "g_Texture", sampDesc},
    };
    psoCI.PSODesc.ResourceLayout.NumImmutableSamplers = _countof(imtblSamplers);
    psoCI.PSODesc.ResourceLayout.ImmutableSamplers = imtblSamplers;

    psoCI.pVS = vs;
    psoCI.pPS = ps;

    device->CreateGraphicsPipelineState(psoCI, &pso_);
    if (pso_ == nullptr) {
        return false;
    }

    // 绑定常量缓冲
    if (auto* var = pso_->GetStaticVariableByName(SHADER_TYPE_VERTEX, "Constants"); var != nullptr) {
        var->Set(constantBuffer_);
    }

    pso_->CreateShaderResourceBinding(&srb_, true);
    return srb_ != nullptr;
}

bool ImGuiDiligent::CreateFontTexture(IRenderDevice* device) {
    ImGuiIO& io = ImGui::GetIO();

    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    if (pixels == nullptr || width <= 0 || height <= 0) {
        return false;
    }

    // 创建纹理
    TextureDesc texDesc{};
    texDesc.Name = "ImGui Font Texture";
    texDesc.Type = RESOURCE_DIM_TEX_2D;
    texDesc.Width = static_cast<Uint32>(width);
    texDesc.Height = static_cast<Uint32>(height);
    texDesc.MipLevels = 1;
    texDesc.Format = TEX_FORMAT_RGBA8_UNORM;
    texDesc.BindFlags = BIND_SHADER_RESOURCE;
    texDesc.Usage = USAGE_IMMUTABLE;

    TextureSubResData subResData{};
    subResData.pData = pixels;
    subResData.Stride = static_cast<Uint32>(width * 4);

    TextureData texData{};
    texData.NumSubresources = 1;
    texData.pSubResources = &subResData;

    device->CreateTexture(texDesc, &texData, &fontTexture_);
    if (fontTexture_ == nullptr) {
        return false;
    }

    fontSRV_ = fontTexture_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    if (fontSRV_ == nullptr) {
        return false;
    }

    // 绑定字体纹理到 SRB
    if (auto* var = srb_->GetVariableByName(SHADER_TYPE_PIXEL, "g_Texture"); var != nullptr) {
        var->Set(fontSRV_);
    }

    // 存储纹理 ID（ImGui 使用）
    io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(fontSRV_.RawPtr()));

    return true;
}

bool ImGuiDiligent::CreateBuffers(IRenderDevice* device, int vertexCount, int indexCount) {
    // 顶点缓冲
    {
        BufferDesc vbDesc{};
        vbDesc.Name = "ImGui Vertex Buffer";
        vbDesc.Size = static_cast<Uint32>(vertexCount * sizeof(ImDrawVert));
        vbDesc.Usage = USAGE_DYNAMIC;
        vbDesc.BindFlags = BIND_VERTEX_BUFFER;
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
        ibDesc.Name = "ImGui Index Buffer";
        ibDesc.Size = static_cast<Uint32>(indexCount * sizeof(ImDrawIdx));
        ibDesc.Usage = USAGE_DYNAMIC;
        ibDesc.BindFlags = BIND_INDEX_BUFFER;
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

} // namespace ParticleSaturn::UI
