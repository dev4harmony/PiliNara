#pragma once
#include <napi/native_api.h>
namespace nativeapi::ohos {
// UI-thread bridge for the original Want from UIAbility.onCreate/onNewWant.
// Cold-launch activation is held until Initialize, after event subscription.
bool HandleNotificationWant(napi_env env, napi_value want);
}  // namespace nativeapi::ohos
