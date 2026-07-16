#pragma once

#include "app/state/AppStates.h"

#include <string>
#include <vector>

namespace ParticleSaturn::Services::Vulkan {

bool ConfigureDriver(App::VulkanDriver driver, const std::string& bundleResources, std::string& error);
bool RestartWithDriver(App::VulkanDriver driver, const std::string& bundleResources, const std::string& executable,
                       const std::vector<std::string>& arguments, int& processId, std::string& error);

} // namespace ParticleSaturn::Services::Vulkan
