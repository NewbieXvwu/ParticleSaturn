#include "app/AppController.h"
#include "app/state/AppStates.h"

#include <cassert>

using namespace ParticleSaturn::App;

int main() {
    // Test 1: Initial state is false
    {
        AppState state;
        assert(!state.render.useObjectShader);
    }

    // Test 2: Command changes state
    {
        AppController controller;
        const auto& state = controller.State();
        assert(!state.render.useObjectShader);

        controller.Dispatch(SetUseObjectShader{true});
        assert(controller.State().render.useObjectShader);
    }

    // Test 3: Command toggles both ways
    {
        AppController controller;

        controller.Dispatch(SetUseObjectShader{true});
        assert(controller.State().render.useObjectShader);

        controller.Dispatch(SetUseObjectShader{false});
        assert(!controller.State().render.useObjectShader);

        controller.Dispatch(SetUseObjectShader{true});
        assert(controller.State().render.useObjectShader);
    }

    // Test 4: Only available on Metal
    {
        AppState state;

        // Object shader 只在 Metal API 下有意义
        state.render.graphicsApi = GraphicsApi::Metal;
        state.render.useObjectShader = true;
        assert(state.render.useObjectShader);

        // 切换到其他 API 时，标志仍然存在但不会被使用
        state.render.graphicsApi = GraphicsApi::OpenGL41;
        // 状态保持，但渲染器会忽略它
        assert(state.render.useObjectShader);
    }

    return 0;
}
