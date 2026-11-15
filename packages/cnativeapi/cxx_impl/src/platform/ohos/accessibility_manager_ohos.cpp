#include "../../accessibility_manager.h"
#include "ohos_napi.h"
#include "ohos_runtime.h"

namespace nativeapi {

void AccessibilityManager::Enable() {
  // The public accessibility module only queries the system state. Enabling an
  // extension requires the system-only config.enableAbility API and
  // WRITE_ACCESSIBILITY_CONFIG. Ordinary applications cannot grant themselves
  // this authority. Do not pretend a local flag enables a system service.
}

bool AccessibilityManager::IsEnabled() {
  bool enabled = false;
  ohos::WithEnvironment([&](napi_env env) {
    napi_value module = nullptr, result = nullptr;
    if (ohos::js::Module(env, "@ohos.accessibility", module) &&
        ohos::js::Call(env, module, "isOpenAccessibilitySync", {}, result)) {
      napi_get_value_bool(env, result, &enabled);
    }
  });
  return enabled;
}

}  // namespace nativeapi
