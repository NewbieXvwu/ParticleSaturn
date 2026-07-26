#include "render/ResourceRegistry.h"

#include <cassert>
#include <stdexcept>

using namespace ParticleSaturn;

int main() {
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
