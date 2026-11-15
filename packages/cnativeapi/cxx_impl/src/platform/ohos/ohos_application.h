#pragma once
#include <napi/native_api.h>
#include <memory>

namespace nativeapi::ohos {
// Forward the owning UIAbility's actual callbacks, on its initialized UI thread.
// Create follows AttachContext; Destroy precedes releasing the Ability context.
// One owning UIAbility is supported per linked core/UI environment. Duplicate
// callbacks are idempotent; foreign contexts and invalid transitions are rejected.
enum class AbilityLifecycle { Create, Foreground, Background, Destroy };
struct WindowState;
// The owning Ability is system-hidden; per-window showing state alone misses this.
bool ApplicationHidesWindow(const std::shared_ptr<WindowState>& window);
// A primary native window observation, distinct from an Ability lifecycle callback.
void ObserveApplicationWindowVisibility(napi_env env, bool visible);
bool HandleApplicationLifecycle(napi_env env, napi_value context, AbilityLifecycle event);
}  // namespace nativeapi::ohos
