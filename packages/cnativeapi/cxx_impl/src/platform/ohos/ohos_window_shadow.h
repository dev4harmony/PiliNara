#pragma once
// Private optional Stage companion. Called on the owning ArkTS UI thread.
#include "ohos_window.h"
namespace nativeapi {
class WindowShadow;
}
namespace nativeapi::ohos {
bool AttachWindowShadowHost(napi_env env, napi_value host);
bool SetWindowCustomShadow(const std::shared_ptr<WindowState>& state,
                           std::shared_ptr<WindowShadow> shadow);
std::shared_ptr<WindowShadow> WindowCustomShadow(const std::shared_ptr<WindowState>& state);
// Returns true when an existing custom configuration handles the visibility switch.
bool ToggleWindowCustomShadow(const std::shared_ptr<WindowState>& state, bool enabled);
void ObserveWindowCustomShadowSize(const std::shared_ptr<WindowState>& state, napi_value size);
void ObserveWindowCustomShadowFrame(const std::shared_ptr<WindowState>& state, napi_value event);
void RefreshWindowCustomShadow(const std::shared_ptr<WindowState>& state);
void ForgetWindowCustomShadow(const std::shared_ptr<WindowState>& state, bool notify);
}  // namespace nativeapi::ohos
