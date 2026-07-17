#pragma once

#include "services/settings/SettingsStore.h"

namespace ParticleSaturn::Services::Settings::MacOS {

class NSUserDefaultsStore final : public SettingsStore {
public:
    explicit NSUserDefaultsStore(void* nativeDefaults = nullptr) : nativeDefaults_{nativeDefaults} {}

    App::AppState Load(const App::AppState& defaults) const override;
    void Save(const App::AppState& state) override;

private:
    void* nativeDefaults_ = nullptr;
};

} // namespace ParticleSaturn::Services::Settings::MacOS
