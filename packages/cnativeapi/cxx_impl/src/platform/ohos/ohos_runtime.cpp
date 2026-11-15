#include "ohos_runtime.h"

#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "../../foundation/dispatcher_common.h"

namespace nativeapi::ohos {
namespace {
std::mutex mutex;
napi_env environment = nullptr;
uint64_t generation = 0;
napi_ref ability_context = nullptr;
napi_threadsafe_function dispatcher = nullptr;
uint64_t next_cleanup = 0;
std::unordered_map<uint64_t, std::function<void()>> cleanups;
bool cleaning = false;

void Invoke(napi_env env, napi_value, void*, void* data) {
  std::unique_ptr<std::function<void()>> work(static_cast<std::function<void()>*>(data));
  // N-API invokes this with a null environment to dispose queued work on abort.
  if (env && work)
    (*work)();
}

void Cleanup(void*) {
  decltype(cleanups) pending;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (ability_context)
      napi_delete_reference(environment, ability_context);
    ability_context = nullptr;
    environment = nullptr;
    auto function = dispatcher;
    dispatcher = nullptr;
    cleaning = true;
    pending.swap(cleanups);
    if (function)
      napi_release_threadsafe_function(function, napi_tsfn_abort);
  }
  // Services may unsubscribe and re-enter runtime helpers. Never invoke their
  // continuations underneath the dispatcher mutex or in a replacement host.
  for (auto& entry : pending) {
    try { entry.second(); } catch (...) { }
  }
  std::lock_guard<std::mutex> lock(mutex);
  cleaning = false;
}
}  // namespace

bool Initialize(napi_env env) {
  if (!env)
    return false;
  std::lock_guard<std::mutex> lock(mutex);
  if (cleaning)
    return false;
  if (dispatcher)
    return environment == env && dispatcher_internal::IsMainThreadByCapturedId();
  napi_value name;
  if (napi_create_string_utf8(env, "nativeapi.dispatch", NAPI_AUTO_LENGTH, &name) != napi_ok ||
      napi_create_threadsafe_function(env, nullptr, nullptr, name, 0, 1, nullptr, nullptr, nullptr,
                                      Invoke, &dispatcher) != napi_ok)
    return false;
  if (napi_add_env_cleanup_hook(env, Cleanup, nullptr) != napi_ok) {
    napi_release_threadsafe_function(dispatcher, napi_tsfn_abort);
    dispatcher = nullptr;
    return false;
  }
  napi_unref_threadsafe_function(env, dispatcher);
  environment = env;
  ++generation;
  dispatcher_internal::CaptureCallerAsMainThread();
  return true;
}

bool AttachContext(napi_env env, napi_value context) {
  if (!env || !context || !dispatcher_internal::IsMainThreadByCapturedId())
    return false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (environment != env)
      return false;
  }
  napi_valuetype type;
  napi_ref reference = nullptr;
  if (napi_typeof(env, context, &type) != napi_ok || type != napi_object ||
      napi_create_reference(env, context, 1, &reference) != napi_ok)
    return false;
  if (ability_context)
    napi_delete_reference(env, ability_context);
  ability_context = reference;
  return true;
}
bool WithEnvironment(const std::function<void(napi_env)>& work) {
  bool invoked = false;
  try {
    RunSync([&] {
      napi_env env;
      {
        std::lock_guard<std::mutex> lock(mutex);
        env = environment;
      }
      if (!env)
        return;
      napi_handle_scope scope = nullptr;
      if (napi_open_handle_scope(env, &scope) != napi_ok)
        return;
      try {
        work(env);
        invoked = true;
      } catch (...) {
        napi_close_handle_scope(env, scope);
        throw;
      }
      napi_close_handle_scope(env, scope);
    });
  } catch (...) {
  }
  return invoked;
}
bool DetachContext(napi_env env, napi_value context) {
  if (!env || !context || !dispatcher_internal::IsMainThreadByCapturedId())
    return false;
  bool detached = false;
  WithContext([&](napi_env current_env, napi_value current) {
    bool same = false;
    if (env == current_env && napi_strict_equals(env, current, context, &same) == napi_ok && same) {
      napi_delete_reference(env, ability_context);
      ability_context = nullptr;
      detached = true;
    }
  });
  return detached;
}
bool WithContext(const std::function<void(napi_env, napi_value)>& work) {
  bool invoked = false;
  WithEnvironment([&](napi_env env) {
    napi_value context = nullptr;
    if (ability_context && napi_get_reference_value(env, ability_context, &context) == napi_ok &&
        context) {
      work(env, context);
      invoked = true;
    }
  });
  return invoked;
}

uint64_t EnvironmentGeneration() {
  std::lock_guard<std::mutex> lock(mutex);
  return environment ? generation : 0;
}

uint64_t AddEnvironmentCleanup(uint64_t expected_generation, std::function<void()> work) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!dispatcher || !work || generation != expected_generation)
    return 0;
  const auto token = ++next_cleanup;
  cleanups.emplace(token, std::move(work));
  return token;
}

void RemoveEnvironmentCleanup(uint64_t token) {
  std::function<void()> removed;
  {
    std::lock_guard<std::mutex> lock(mutex);
    auto found = cleanups.find(token);
    if (found == cleanups.end()) return;
    removed = std::move(found->second);
    cleanups.erase(found);
  }
}

bool IsInitialized() {
  std::lock_guard<std::mutex> lock(mutex);
  return dispatcher != nullptr;
}

bool Post(std::function<void()> work) {
  if (!work)
    return true;
  auto data = std::make_unique<std::function<void()>>(std::move(work));
  std::lock_guard<std::mutex> lock(mutex);
  if (!dispatcher ||
      napi_call_threadsafe_function(dispatcher, data.get(), napi_tsfn_nonblocking) != napi_ok)
    return false;
  data.release();
  return true;
}

bool RunSync(std::function<void()> work) {
  if (!IsInitialized())
    return false;
  if (dispatcher_internal::IsMainThreadByCapturedId()) {
    work();
    return true;
  }
  auto completion = std::make_shared<std::promise<void>>();
  auto result = completion->get_future();
  if (!Post([completion, work = std::move(work)] {
        try {
          work();
          completion->set_value();
        } catch (...) {
          completion->set_exception(std::current_exception());
        }
      }))
    return false;
  completion.reset();
  try {
    result.get();
    return true;
  } catch (...) {
    return false;
  }
}
}  // namespace nativeapi::ohos
