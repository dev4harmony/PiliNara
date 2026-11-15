#include "../../foundation/dispatcher_common.h"
#include "../../foundation/dispatcher_platform.h"
#include "ohos_runtime.h"

namespace nativeapi {
namespace dispatcher_platform {

bool PlatformIsMainThread() {
  return dispatcher_internal::IsMainThreadByCapturedId();
}

void PlatformSetMainThread() {
  dispatcher_internal::CaptureCallerAsMainThread();
}

bool PlatformIsMainThreadDispatchSupported() {
  return ohos::IsInitialized();
}

bool PlatformRunOnMainThread(std::function<void()> fn) {
  return ohos::Post(std::move(fn));
}

bool PlatformRunMainThreadLoopFor(int timeout_ms) {
  (void)timeout_ms;
  return false;
}

}  // namespace dispatcher_platform
}  // namespace nativeapi
