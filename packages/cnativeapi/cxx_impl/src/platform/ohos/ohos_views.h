#pragma once
// Internal host seam. Call on the owning ArkTS UI thread.
#include "ohos_window.h"
namespace nativeapi::ohos {
// Pass the NodeContent shown by this window's ContentSlot. Reattachment keeps
// the existing View tree. Unmount before the page's slot disappears.
// Optional companion NativeTooltipHost from ets/NativeTooltipHost.ets enables
// delayed nonmodal hover tips. Ownership lasts until this content is unmounted.
bool AttachViewContent(napi_env env, napi_value window, napi_value content,
                       napi_value tooltip_host = nullptr);
void DetachViewContent(const std::shared_ptr<WindowState>& state);
}  // namespace nativeapi::ohos
