#include "services/vulkan/VulkanDriverRuntime.h"

#include <cassert>
#include <cstdlib>
#include <string>
#include <sys/wait.h>

int main(int argc, char* argv[]) {
    assert(argc == 2);
    std::string error;
    assert(ParticleSaturn::Services::Vulkan::ConfigureDriver(
        ParticleSaturn::App::VulkanDriver::MoltenVK, argv[1], error));
    const char* driverFiles = std::getenv("VK_DRIVER_FILES");
    assert(driverFiles != nullptr);
    assert(std::string{driverFiles}.find("MoltenVK_icd.json") != std::string::npos);
    int processId = 0;
    assert(ParticleSaturn::Services::Vulkan::RestartWithDriver(
        ParticleSaturn::App::VulkanDriver::MoltenVK, argv[1], "/usr/bin/true", {}, processId, error));
    int status = 0;
    assert(waitpid(processId, &status, 0) == processId);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return 0;
}
