#include "VulkanDriverRuntime.h"

#include <cstdlib>
#include <filesystem>

namespace ParticleSaturn::Services::Vulkan {

bool ConfigureDriver(App::VulkanDriver driver, const std::string& bundleResources, std::string& error) {
    const char* name = driver == App::VulkanDriver::MoltenVK ? "MoltenVK_icd.json" : "KosmicKrisp_icd.json";
    const auto path = std::filesystem::path{bundleResources} / "Vulkan" / "etc" / "vulkan" / "icd.d" / name;
    if (!std::filesystem::is_regular_file(path)) {
        error = "selected Vulkan driver ICD is missing: " + path.string();
        return false;
    }
    if (setenv("VK_DRIVER_FILES", path.c_str(), 1) != 0) {
        error = "unable to set VK_DRIVER_FILES";
        return false;
    }
    error.clear();
    return true;
}

} // namespace ParticleSaturn::Services::Vulkan
