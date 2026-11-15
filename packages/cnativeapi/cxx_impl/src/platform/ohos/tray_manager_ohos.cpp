#include "../../tray_manager.h"
#include "ohos_tray.h"

namespace nativeapi {
class TrayManager::Impl {};
TrayManager::TrayManager() : pimpl_(std::make_unique<Impl>()), next_tray_id_(1) {}
TrayManager::~TrayManager() = default;
bool TrayManager::IsSupported() {
  return ohos::TraySupported();
}
std::shared_ptr<TrayIcon> TrayManager::Get(TrayIconId id) {
  return ohos::FindTray(id);
}
std::vector<std::shared_ptr<TrayIcon>> TrayManager::GetAll() {
  return ohos::AllTrays();
}
}  // namespace nativeapi
