#pragma once
// Private UI-host seam. Call only on the owning UI thread. The host forwards
// normal component onKeyPreIme events, never API 26 pre-dispatch raw wrappers:
// only normal KeyEvent objects have valid event-time modifier snapshots.
#include "ohos_window.h"
namespace nativeapi::ohos {
uint64_t AttachLocalKeyHost(napi_env env, napi_value window);
void DetachLocalKeyHost(uint64_t host);
bool DispatchLocalKey(uint64_t host, int32_t key, int32_t type,
                      uint32_t modifiers, int32_t device);
void ResetLocalKeys(const std::shared_ptr<WindowState>& state, bool forget = false);
}  // namespace nativeapi::ohos
