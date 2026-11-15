#include "menu.h"

namespace nativeapi {
#if !defined(_WIN32) && !defined(__OHOS__)
bool Menu::SetBackend(MenuBackend backend) {
  return backend == MenuBackend::Native;
}

MenuBackend Menu::GetBackend() const {
  return MenuBackend::Native;
}

bool Menu::IsBackendSupported(MenuBackend backend) {
  return backend == MenuBackend::Native;
}
#endif
}  // namespace nativeapi
