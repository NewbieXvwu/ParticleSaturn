#pragma once

#include "services/settings/SettingsStore.h"

namespace ParticleSaturn::Services::Settings::MacOS {

class NSUserDefaultsStore final : public SettingsStore {
public:
    App::AppState Load(const App::AppState& defaults) const override;
    void Save(const App::AppState& state) override;
};

} // namespace ParticleSaturn::Services::Settings::MacOS
