#include "VulkanDriverRuntime.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <spawn.h>
#include <vector>

extern char** environ;

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
    std::clog << "[Vulkan] selected ICD " << name << " at " << path << '\n';
    error.clear();
    return true;
}

bool RestartWithDriver(App::VulkanDriver driver, const std::string& bundleResources, const std::string& executable,
                       const std::vector<std::string>& arguments, int& processId, std::string& error) {
    if (!ConfigureDriver(driver, bundleResources, error)) return false;
    if (executable.empty()) {
        error = "restart executable is empty";
        return false;
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    pid_t child = 0;
    const int result = posix_spawn(&child, executable.c_str(), nullptr, nullptr, argv.data(), environ);
    if (result != 0) {
        error = "unable to restart selected Vulkan driver process";
        return false;
    }
    processId = static_cast<int>(child);
    error.clear();
    return true;
}

} // namespace ParticleSaturn::Services::Vulkan
