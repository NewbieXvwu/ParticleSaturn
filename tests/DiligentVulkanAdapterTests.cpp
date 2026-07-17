#include "gpu/backends/diligent/DiligentVulkanAdapter.h"
#include "services/vulkan/VulkanDriverRuntime.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/wait.h>

namespace {

ParticleSaturn::App::VulkanDriver ParseDriver(const char* value) {
    return std::strcmp(value, "molten") == 0 ? ParticleSaturn::App::VulkanDriver::MoltenVK
                                               : ParticleSaturn::App::VulkanDriver::KosmicKrisp;
}

int RunChild(const char* executable, const char* resources, const char* driverName) {
    std::string error;
    int processId = 0;
    assert(ParticleSaturn::Services::Vulkan::RestartWithDriver(
        ParseDriver(driverName), resources, executable, {resources, driverName}, processId, error));
    int status = 0;
    assert(waitpid(processId, &status, 0) == processId);
    assert(WIFEXITED(status));
    return WEXITSTATUS(status);
}

} // namespace

int main(int argc, char* argv[]) {
    assert(argc == 2 || argc == 3);
    if (argc == 2) {
        assert(RunChild(argv[0], argv[1], "molten") == 0);
        assert(RunChild(argv[0], argv[1], "kosmic") == 0);
        return 0;
    }

    ParticleSaturn::Gpu::Diligent::DiligentVulkanAdapter adapter;
    std::string error;
    if (!adapter.Initialize(ParseDriver(argv[2]), argv[1], error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    assert(!adapter.AdapterName().empty());
    assert(adapter.Capabilities().supportsCompute);
    assert(adapter.Capabilities().supportsStorageBuffer);
    assert(adapter.Capabilities().supportsIndirectDraw);
    assert(!adapter.CreateSwapChain(nullptr, 0, 0, error));
    assert(!error.empty());
    return 0;
}
