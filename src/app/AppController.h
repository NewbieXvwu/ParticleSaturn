#pragma once

#include "AppCommand.h"

namespace ParticleSaturn::App {

class AppController {
  public:
    explicit AppController(AppState initialState = {});

    const AppState& State() const noexcept;
    AppState&       MutableState() noexcept;
    CommandEffect   Dispatch(const AppCommand& command);

  private:
    AppState state_;
};

} // namespace ParticleSaturn::App
