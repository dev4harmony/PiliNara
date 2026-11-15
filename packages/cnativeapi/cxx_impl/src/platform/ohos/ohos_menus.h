#pragma once
// Internal host bridge for real ArkUI Menu / MenuItem components. UI thread only.
#include <napi/native_api.h>
namespace nativeapi::ohos {
// Pass the companion NativeMenuHost from ets/NativeMenuHost.ets, after attaching
// this host window. Its anchor must be mounted in the same window's page.
bool AttachMenuHost(napi_env env, napi_value window, napi_value host);
}  // namespace nativeapi::ohos
