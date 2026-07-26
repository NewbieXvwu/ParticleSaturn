#include "gpu/backends/metal/MetalBackend.h"
#include "app/state/AppStates.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

// 验证 Metal object/mesh shader 路径与传统顶点着色器路径的画面一致性。
// 在同一设备、同一状态下分别渲染两条路径，比较最终输出的像素差异。
// 要求平均通道差异 < 2.0，差异像素比例 < 4%。

using namespace ParticleSaturn;

namespace {

struct Pixel {
    std::uint8_t r, g, b;
};

struct BaselineMetrics {
    double avgChannelDiff;
    double diffPixelRatio;
};

BaselineMetrics CompareSurfaces(const std::vector<Pixel>& a, const std::vector<Pixel>& b,
                                 std::uint32_t width, std::uint32_t height) {
    assert(a.size() == width * height);
    assert(b.size() == width * height);

    double totalDiff = 0.0;
    std::uint32_t diffPixels = 0;

    for (std::uint32_t i = 0; i < width * height; ++i) {
        const int dr = std::abs(static_cast<int>(a[i].r) - static_cast<int>(b[i].r));
        const int dg = std::abs(static_cast<int>(a[i].g) - static_cast<int>(b[i].g));
        const int db = std::abs(static_cast<int>(a[i].b) - static_cast<int>(b[i].b));
        const int maxDiff = std::max({dr, dg, db});

        totalDiff += (dr + dg + db) / 3.0;
        if (maxDiff > 8) ++diffPixels;
    }

    return BaselineMetrics{
        totalDiff / static_cast<double>(width * height),
        static_cast<double>(diffPixels) / static_cast<double>(width * height)
    };
}

std::vector<Pixel> CaptureFrame(Gpu::Metal::MetalDevice& device,
                                 Gpu::Metal::MetalParticleSystem& particles,
                                 Gpu::Metal::MetalStarField& stars,
                                 Gpu::Metal::MetalParticleRenderer& renderer,
                                 const App::AppState& state,
                                 std::uint32_t width, std::uint32_t height) {
    void* nativeDevice = device.NativeDevice();
    id<MTLDevice> mtlDevice = (__bridge id<MTLDevice>)nativeDevice;

    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                     width:width height:height mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [mtlDevice newTextureWithDescriptor:desc];
    assert(texture != nil);

    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)device.NativeCommandQueue();
    id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

    id<MTLRenderCommandEncoder> encoder = [cmdBuf renderCommandEncoderWithDescriptor:pass];
    renderer.Draw((__bridge void*)encoder, particles.RenderBuffer(), stars.Buffer(), width, height, state);
    [encoder endEncoding];

    [cmdBuf commit];
    [cmdBuf waitUntilCompleted];

    std::vector<Pixel> pixels(width * height);
    [texture getBytes:pixels.data() bytesPerRow:width * 4 fromRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0];

    // Convert BGRA to RGB
    for (auto& p : pixels) {
        std::swap(p.r, p.b);
    }

    [texture release];
    return pixels;
}

} // namespace

int main() {
    Gpu::Metal::MetalDevice device;
    assert(device.Initialize());

    void* nativeDevice = device.NativeDevice();
    id<MTLDevice> mtlDevice = (__bridge id<MTLDevice>)nativeDevice;

    // 检查是否支持 object shader
    bool supportsObjectShader = false;
    if (@available(macOS 13.0, *)) {
        supportsObjectShader = [mtlDevice supportsFamily:MTLGPUFamilyMetal3];
    }

    if (!supportsObjectShader) {
        // 不支持 object shader 的设备跳过测试
        return 0;
    }

    Gpu::Metal::MetalParticleSystem particles;
    Gpu::Metal::MetalStarField stars;
    Gpu::Metal::MetalParticleRenderer renderer;

    assert(particles.Initialize(device, nullptr, 0x53415455U));
    assert(stars.Initialize(device, nullptr));
    assert(renderer.Initialize(device, nullptr));

    // 构造固定测试状态
    App::AppState state;
    state.scene.paused = true;
    state.scene.simulationTimeSeconds = 1.0f;
    state.scene.zoom = 1.5f;
    state.scene.rotationX = 0.3f;
    state.scene.rotationY = 0.5f;
    state.render.particleCount = 200000;
    state.render.pixelRatio = 1.0f;
    state.render.densityCompensation = 1.0f;

    constexpr std::uint32_t width = 1280;
    constexpr std::uint32_t height = 720;

    // 渲染传统路径
    state.render.useObjectShader = false;
    auto vertexPixels = CaptureFrame(device, particles, stars, renderer, state, width, height);

    // 渲染 object shader 路径
    state.render.useObjectShader = true;
    auto objectPixels = CaptureFrame(device, particles, stars, renderer, state, width, height);

    // 比较两条路径的输出
    auto metrics = CompareSurfaces(vertexPixels, objectPixels, width, height);

    // 验证阈值
    assert(metrics.avgChannelDiff < 2.0);
    assert(metrics.diffPixelRatio < 0.04);

    return 0;
}
