#include "render/RenderGraph.h"
#include "render/ResourceRegistry.h"

#include <cassert>
#include <string>
#include <stdexcept>

using namespace ParticleSaturn;

int main() {
    Render::RenderGraph graph;
    const auto scene = graph.AddResource({"scene", {1920, 1080, 1}});
    const auto bloom = graph.AddResource({"bloom", {320, 180, 1}});
    const auto scenePass = graph.AddPass("scene");
    const auto bloomPass = graph.AddPass("bloom");
    const auto compositePass = graph.AddPass("composite");
    graph.Write(scenePass, scene, Gpu::ResourceUsage::RenderTarget);
    graph.Read(bloomPass, scene, Gpu::ResourceUsage::ShaderRead);
    graph.Write(bloomPass, bloom, Gpu::ResourceUsage::RenderTarget);
    graph.Read(compositePass, scene, Gpu::ResourceUsage::ShaderRead);
    graph.Read(compositePass, bloom, Gpu::ResourceUsage::ShaderRead);
    const auto order = graph.Compile();
    assert(order.size() == 3);
    assert(order[0] == scenePass);
    assert(order[1] == bloomPass);
    assert(order[2] == compositePass);
    std::string execution;
    Render::RenderGraph executable;
    const auto input = executable.AddResource({"input", {1, 1, 1}});
    const auto output = executable.AddResource({"output", {1, 1, 1}});
    const auto writer = executable.AddPass("writer", [&] { execution += 'w'; return true; });
    const auto reader = executable.AddPass("reader", [&] { execution += 'r'; return true; });
    executable.Write(writer, input, Gpu::ResourceUsage::RenderTarget);
    executable.Read(reader, input, Gpu::ResourceUsage::ShaderRead);
    executable.Write(reader, output, Gpu::ResourceUsage::RenderTarget);
    assert(executable.Execute());
    assert(execution == "wr");

    Render::TexturePool pool;
    const Gpu::TextureDesc desc{128, 128, 1};
    const auto first = pool.Acquire(desc, 0);
    pool.Release(first, 3);
    const auto second = pool.Acquire(desc, 2);
    assert(second.index != first.index);
    const auto recycled = pool.Acquire(desc, 3);
    assert(recycled.index == first.index);
    assert(recycled.generation != first.generation);

    bool staleHandleRejected = false;
    try {
        pool.Release(first, 4);
    } catch (const std::logic_error&) {
        staleHandleRejected = true;
    }
    assert(staleHandleRejected);
    return 0;
}
