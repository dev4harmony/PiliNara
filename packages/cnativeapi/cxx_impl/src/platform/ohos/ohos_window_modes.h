#pragma once

#include "ohos_window.h"

namespace nativeapi::ohos {
// After applying known SDK/StartOptions mode overrides, the host can publish
// that exact baseline. This does not call the SDK. Enum values are the public
// bundleManager.SupportWindowMode values; unknown/invalid masks are rejected.
// Pending nativeapi mode requests must finish before a baseline can be replaced.
bool SetWindowModeBaseline(const std::shared_ptr<Window>& window,
                           const std::vector<int32_t>& modes);
void SetWindowFullScreenable(const std::shared_ptr<WindowState>& state, bool enabled);
bool WindowFullScreenable(const std::shared_ptr<WindowState>& state);
}  // namespace nativeapi::ohos
