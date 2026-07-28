#pragma once

#include "app/state/AppStates.h"

namespace ParticleSaturn::Services::Settings::MacOS {

class NSUserDefaultsStore final {
  public:
    explicit NSUserDefaultsStore(void* nativeDefaults = nullptr) : nativeDefaults_{nativeDefaults} {}

    App::AppState Load(const App::AppState& defaults) const;
    void          Save(const App::AppState& state);

  private:
    void* nativeDefaults_ = nullptr;
};

} // namespace ParticleSaturn::Services::Settings::MacOS
