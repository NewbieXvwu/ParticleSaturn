#pragma once

#include "app/state/AppStates.h"

namespace ParticleSaturn::Services::Settings {

class SettingsStore {
public:
    virtual ~SettingsStore() = default;
    virtual App::AppState Load(const App::AppState& defaults) const = 0;
    virtual void Save(const App::AppState& state) = 0;
};

} // namespace ParticleSaturn::Services::Settings
