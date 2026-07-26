#include "common/ImageMetrics.h"

#include "gpu/backends/metal/MetalBackend.h"
#include "app/state/AppStates.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// 验证 Metal object/mesh shader 路径与传统顶点着色器路径的画面一致性。
// 在同一设备、同一状态下分别渲染两条路径到与生产一致的 RGBA16Float
// 场景目标，比较最终输出的像素差异。
// 要求平均通道差异 < 2.0（8-bit 尺度），差异像素比例 < 4%。

using namespace ParticleSaturn;

namespace {

struct Pixel {
    std::uint8_t r, g, b;
};

struct BaselineMetrics {
    double avgChannelDiff;
    double diffPixelRatio;
};

std::uint8_t HalfToByte(std::uint16_t bits) {
    _Float16 half;
    static_assert(sizeof(half) == sizeof(bits));
    std::memcpy(&half, &bits, sizeof(bits));
    const float value = std::clamp(static_cast<float>(half), 0.0f, 1.0f);
    return static_cast<std::uint8_t>(value * 255.0f + 0.5f);
}

BaselineMetrics CompareSurfaces(const std::vector<Pixel>& a, const std::vector<Pixel>& b,
                                 std::uint32_t width, std::uint32_t height) {
    assert(a.size() == width * height);
    assert(b.size() == width * height);

    ParticleSaturn::TestCommon::ImageDifferenceAccumulator accumulator;
    for (std::uint32_t i = 0; i < width * height; ++i) {
        accumulator.AddPixel(a[i].r, a[i].g, a[i].b, b[i].r, b[i].g, b[i].b);
    }
    return BaselineMetrics{accumulator.MeanChannelDifference(), accumulator.MismatchFraction()};
}

std::vector<Pixel> CaptureFrame(Gpu::Metal::MetalDevice& device,
                                 Gpu::Metal::MetalParticleSystem& particles,
                                 Gpu::Metal::MetalStarField& stars,
                                 Gpu::Metal::MetalParticleRenderer& renderer,
                                 const App::AppState& state,
                                 std::uint32_t width, std::uint32_t height) {
    id<MTLDevice> mtlDevice = (id<MTLDevice>)device.NativeDevice();

    // 与生产 scene-hdr 通道一致的目标格式。
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                     width:width height:height mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [mtlDevice newTextureWithDescriptor:desc];
    assert(texture != nil);

    id<MTLCommandQueue> queue = (id<MTLCommandQueue>)device.NativeCommandQueue();
    id<MTLCommandBuffer> commands = [queue commandBuffer];

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.005, 0.008, 0.016, 1.0);

    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    renderer.Draw((void*)encoder, particles.RenderBuffer(), stars.Buffer(), width, height, state);
    [encoder endEncoding];

    [commands commit];
    [commands waitUntilCompleted];
    assert([commands status] == MTLCommandBufferStatusCompleted);

    std::vector<std::uint16_t> halfPixels(static_cast<std::size_t>(width) * height * 4U);
    [texture getBytes:halfPixels.data() bytesPerRow:width * 8 fromRegion:MTLRegionMake2D(0, 0, width, height)
          mipmapLevel:0];
    [texture release];

    std::vector<Pixel> pixels(static_cast<std::size_t>(width) * height);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = {HalfToByte(halfPixels[i * 4U + 0U]), HalfToByte(halfPixels[i * 4U + 1U]),
                     HalfToByte(halfPixels[i * 4U + 2U])};
    }
    return pixels;
}

} // namespace

int main(int argc, char* argv[]) {
    assert(argc == 2);
    Gpu::Metal::MetalDevice device;
    assert(device.Initialize());

    // 不支持 Metal 3 网格着色器的设备跳过测试。
    if (!device.Capabilities().supportsMeshShader) {
        std::printf("mesh shaders unsupported, skipping baseline comparison\n");
        return 0;
    }

    Gpu::Metal::MetalParticleSystem particles;
    Gpu::Metal::MetalStarField stars;
    Gpu::Metal::MetalParticleRenderer renderer;

    assert(particles.Initialize(device, argv[1], 0x53415455U));
    assert(stars.Initialize(device, argv[1], 0x53544152U));
    assert(renderer.Initialize(device, argv[1]));

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
    const auto vertexPixels = CaptureFrame(device, particles, stars, renderer, state, width, height);

    // 渲染 object shader 路径
    state.render.useObjectShader = true;
    const auto objectPixels = CaptureFrame(device, particles, stars, renderer, state, width, height);

    // 比较两条路径的输出
    const auto metrics = CompareSurfaces(vertexPixels, objectPixels, width, height);
    std::printf("avg channel diff %.4f, diff pixel ratio %.4f\n", metrics.avgChannelDiff, metrics.diffPixelRatio);

    assert(metrics.avgChannelDiff < 2.0);
    assert(metrics.diffPixelRatio < 0.04);

    return 0;
}
