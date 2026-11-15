#pragma once

#include <napi/native_api.h>
#include <functional>
#include <cstdint>

namespace nativeapi::ohos {
// Call once from the UIAbility's N-API module initializer, on its ArkTS UI
// thread, before using asynchronous services. The Ability owns the event loop.
// An environment cleanup hook stops dispatch when that environment is destroyed.
bool Initialize(napi_env env);
// Attach the owning UIAbilityContext on the same UI thread (e.g. onCreate).
// This enables metadata/resources and other context-bound public SDK services.
bool AttachContext(napi_env env, napi_value context);
// Release only the matching owning context, from its onDestroy callback.
bool DetachContext(napi_env env, napi_value context);
bool WithEnvironment(const std::function<void(napi_env)>& work);
bool WithContext(const std::function<void(napi_env, napi_value)>& work);
bool IsInitialized();
// Nonzero identity of the currently initialized UI environment; zero after cleanup.
uint64_t EnvironmentGeneration();
// Thread-safe native cleanup registration for services that may first be used
// by a worker. Callbacks run on the owning UI thread after dispatch is stopped.
// Unlike WithEnvironment, registration never waits for the UI thread.
uint64_t AddEnvironmentCleanup(uint64_t expected_generation, std::function<void()> work);
void RemoveEnvironmentCleanup(uint64_t token);
bool Post(std::function<void()> work);
bool RunSync(std::function<void()> work);
}  // namespace nativeapi::ohos
