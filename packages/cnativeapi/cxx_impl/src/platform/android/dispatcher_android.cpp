#include "../../foundation/dispatcher_common.h"
#include "../../foundation/dispatcher_platform.h"

#include "android_runtime.h"

namespace nativeapi {
namespace dispatcher_platform {

bool PlatformIsMainThread() {
  return dispatcher_internal::IsMainThreadByCapturedId();
}

void PlatformSetMainThread() {
  dispatcher_internal::CaptureCallerAsMainThread();
}

bool PlatformIsMainThreadDispatchSupported() {
  return android::IsInitialized();
}

bool PlatformRunOnMainThread(std::function<void()> fn) {
  return android::Post(std::move(fn));
}

bool PlatformRunMainThreadLoopFor(int timeout_ms) {
  // Android's host owns the main Looper; never nest it here.
  (void)timeout_ms;
  return false;
}

}  // namespace dispatcher_platform
}  // namespace nativeapi
