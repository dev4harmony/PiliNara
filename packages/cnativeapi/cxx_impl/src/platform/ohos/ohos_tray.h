#pragma once
// Internal tray identities. GetNativeObject returns a borrowed model token.
#include <memory>
#include <vector>
#include "../../tray_icon.h"
namespace nativeapi::ohos {
bool TraySupported();
std::shared_ptr<TrayIcon> FindTray(TrayIconId id);
std::vector<std::shared_ptr<TrayIcon>> AllTrays();
void RefreshTrayMenu();
bool DispatchTrayMenuItem(const std::shared_ptr<Menu>& root, MenuItemId item);
}  // namespace nativeapi::ohos
