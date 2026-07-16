#pragma once

#include "app/state/AppStates.h"

#include <string>

namespace ParticleSaturn::Services::Vulkan {

bool ConfigureDriver(App::VulkanDriver driver, const std::string& bundleResources, std::string& error);

} // namespace ParticleSaturn::Services::Vulkan
