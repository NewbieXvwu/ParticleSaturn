#include "services/vulkan/VulkanDriverRuntime.h"

#include <cassert>
#include <cstdlib>
#include <string>

int main(int argc, char* argv[]) {
    assert(argc == 2);
    std::string error;
    assert(ParticleSaturn::Services::Vulkan::ConfigureDriver(
        ParticleSaturn::App::VulkanDriver::MoltenVK, argv[1], error));
    const char* driverFiles = std::getenv("VK_DRIVER_FILES");
    assert(driverFiles != nullptr);
    assert(std::string{driverFiles}.find("MoltenVK_icd.json") != std::string::npos);
    return 0;
}
