#include "../../window_close_dispatch.h"
#include "ohos_runtime.h"
#include "ohos_window.h"

namespace nativeapi::detail {
std::shared_ptr<WindowCloseState> GetNativeWindowCloseState(void* token, WindowId id) {
  auto state = ohos::FindWindowState(token);
  return state && state->id == id ? state->close : nullptr;
}
bool RequestNativeWindowClose(void* token, WindowId id) {
  auto state = ohos::FindWindowState(token);
  if (!state || state->id != id || state->primary)
    return false;
  if (state->destroying)
    return true;
  return state->close->Request([weak = std::weak_ptr<ohos::WindowState>(state)] {
    auto state = weak.lock();
    if (!state || !state->alive)
      return;
    ohos::DestroyWindow(state);
  });
}
bool IsNativeWindowCloseSupported() {
  return ohos::IsInitialized();
}
}  // namespace nativeapi::detail

namespace nativeapi {
bool Window::DispatchWindowTask(std::function<void()> task) {
  return ohos::RunSync(std::move(task));
}
}  // namespace nativeapi
