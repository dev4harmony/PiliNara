#include "display_ohos.h"
#include <dlfcn.h>
#include "../../display.h"

namespace nativeapi {

class Display::Impl {
 public:
  explicit Impl(void* token) : token_(token) {}
  const DisplayId id_ = IdAllocator::Allocate<Display>();
  void* token_;
};

Display::Display(void* display) : pimpl_(std::make_unique<Impl>(display)) {}
Display::~Display() = default;

void* Display::GetNativeObjectInternal() const {
  return ohos::ReadDisplay(pimpl_->token_) ? pimpl_->token_ : nullptr;
}
DisplayId Display::GetId() const {
  return pimpl_->id_;
}
std::string Display::GetName() const {
  auto info = ohos::ReadDisplay(pimpl_->token_);
  return info ? info->name : "";
}
Point Display::GetPosition() const {
  auto info = ohos::ReadDisplay(pimpl_->token_);
  if (!info)
    return {};
  int32_t x = 0, y = 0;
  // Added in API 26. Resolve optionally so API 23-25 consumers can still load
  // the library; not every SDK's availability runtime is shipped in emulators.
  using PositionQuery = NativeDisplayManager_ErrorCode (*)(uint64_t, int32_t*, int32_t*);
  static const auto query = reinterpret_cast<PositionQuery>(
      dlsym(RTLD_DEFAULT, "OH_NativeDisplayManager_GetDisplayPosition"));
  if (query && query(info->id, &x, &y) != DISPLAY_MANAGER_OK)
    x = y = 0;
  const auto scale = ohos::DisplayScale(*info);
  return {x / scale, y / scale};
}
Size Display::GetSize() const {
  auto info = ohos::ReadDisplay(pimpl_->token_);
  if (!info)
    return {};
  const auto scale = ohos::DisplayScale(*info);
  return {info->width / scale, info->height / scale};
}
Rectangle Display::GetWorkArea() const {
  auto info = ohos::ReadDisplay(pimpl_->token_);
  if (!info)
    return {};
  const auto scale = ohos::DisplayScale(*info);
  const auto position = GetPosition();
  NativeDisplayManager_Rect* area = nullptr;
  OH_NativeDisplayManager_CreateAvailableArea(info->id, &area);
  if (area) {
    Rectangle result{position.x + area->left / scale, position.y + area->top / scale,
                     area->width / scale, area->height / scale};
    OH_NativeDisplayManager_DestroyAvailableArea(area);
    return result;
  }
  // Phones on older releases do not expose the available area.
  return {position.x, position.y, info->width / scale, info->height / scale};
}
double Display::GetScaleFactor() const {
  auto info = ohos::ReadDisplay(pimpl_->token_);
  return info ? ohos::DisplayScale(*info) : 0.0;
}
bool Display::IsPrimary() const {
  if (!ohos::ReadDisplay(pimpl_->token_))
    return false;
  NativeDisplayManager_DisplayInfo* primary = nullptr;
  OH_NativeDisplayManager_CreatePrimaryDisplay(&primary);
  ohos::DisplayInfo info(primary, OH_NativeDisplayManager_DestroyDisplay);
  return info && info->id == ohos::DisplayNativeId(pimpl_->token_);
}
DisplayOrientation Display::GetOrientation() const {
  auto info = ohos::ReadDisplay(pimpl_->token_);
  if (!info)
    return DisplayOrientation::kPortrait;
  switch (info->orientation) {
    case DISPLAY_MANAGER_LANDSCAPE:
      return DisplayOrientation::kLandscape;
    case DISPLAY_MANAGER_PORTRAIT_INVERTED:
      return DisplayOrientation::kPortraitFlipped;
    case DISPLAY_MANAGER_LANDSCAPE_INVERTED:
      return DisplayOrientation::kLandscapeFlipped;
    default:
      return DisplayOrientation::kPortrait;
  }
}
int Display::GetRefreshRate() const {
  auto info = ohos::ReadDisplay(pimpl_->token_);
  return info ? static_cast<int>(info->refreshRate) : 0;
}
int Display::GetBitDepth() const {
  // The public NDK does not expose framebuffer bit depth.
  return 0;
}

}  // namespace nativeapi
