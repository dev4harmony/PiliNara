#pragma once

#include <window_manager/oh_display_manager.h>
#include <cstdint>
#include <memory>

namespace nativeapi::ohos {
// Internal lifecycle diagnostic; queued notifications for the current subscription.
uint32_t PendingDisplayNotifications();
// DisplayManager's narrow seam carries an opaque token. Add one because the
// system's default display ID is normally zero; nullptr remains an invalid token.
inline void* DisplayToken(uint32_t id) {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(id) + 1);
}
inline uint32_t DisplayNativeId(void* token) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(token) - 1);
}
using DisplayInfo = std::unique_ptr<NativeDisplayManager_DisplayInfo,
                                    decltype(&OH_NativeDisplayManager_DestroyDisplay)>;
inline DisplayInfo ReadDisplay(void* token) {
  NativeDisplayManager_DisplayInfo* info = nullptr;
  if (token) {
    OH_NativeDisplayManager_CreateDisplayById(DisplayNativeId(token), &info);
  }
  return DisplayInfo(info, OH_NativeDisplayManager_DestroyDisplay);
}
inline double DisplayScale(const NativeDisplayManager_DisplayInfo& info) {
  return info.densityPixels > 0 ? info.densityPixels : 1.0;
}
}  // namespace nativeapi::ohos
