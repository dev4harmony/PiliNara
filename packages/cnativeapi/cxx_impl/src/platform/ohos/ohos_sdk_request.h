#pragma once

// Private promise request shared by UI-thread submissions and waiting workers.
#include <hilog/log.h>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "../../foundation/dispatcher.h"
#include "ohos_napi.h"

namespace nativeapi::ohos {
// A UI caller submits the request without blocking the Ability's event loop.
// A worker waits for the real SDK promise; environment cleanup releases it.
struct SdkRequest : std::enable_shared_from_this<SdkRequest> {
  std::mutex mutex;
  std::condition_variable changed;
  bool done = false, success = false, hook = false;
  napi_env env = nullptr;
  std::string name;
  std::vector<napi_ref> roots;
  // Optional checked decoder for SDK getters. Runs on UI while the fulfilled
  // value is borrowed; invalid result types make the request fail.
  std::function<bool(napi_env, napi_value)> read_result;
  std::function<void(bool)> completed;
  static void Cleanup(void* data) {
    auto self = static_cast<SdkRequest*>(data)->shared_from_this();
    self->hook = false;
    self->Finish(false, false);
  }
  void Finish(bool ok, bool notify = true) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (done)
        return;
    }
    if (hook) {
      napi_remove_env_cleanup_hook(env, Cleanup, this);
      hook = false;
    }
    for (auto ref : roots)
      napi_delete_reference(env, ref);
    roots.clear();
    if (notify && completed) {
      try {
        completed(ok);
      } catch (...) {
      }
    }
    completed = {};
    read_result = {};
    {
      std::lock_guard<std::mutex> lock(mutex);
      done = true;
      success = ok;
    }
    changed.notify_all();
  }
  bool Keep(napi_value value) {
    napi_ref ref = nullptr;
    if (napi_create_reference(env, value, 1, &ref) != napi_ok)
      return false;
    try {
      roots.push_back(ref);
      return true;
    } catch (...) {
      napi_delete_reference(env, ref);
      return false;
    }
  }
  struct Reaction {
    std::shared_ptr<SdkRequest> request;
    bool success;
  };
  static napi_value React(napi_env env, napi_callback_info info) {
    size_t count = 1;
    napi_value arg = nullptr;
    void* data = nullptr;
    napi_get_cb_info(env, info, &count, &arg, nullptr, &data);
    auto reaction = *static_cast<Reaction*>(data);
    auto self = reaction.request;
    if (env != self->env)
      return nullptr;
    if (!reaction.success) {
      int32_t code = -1;
      napi_value value = nullptr;
      if (ohos::js::Property(env, arg, "code", value))
        napi_get_value_int32(env, value, &code);
      OH_LOG_Print(LOG_APP, LOG_WARN, 1, "NativeApi", "%{public}s SDK rejected %{public}d",
                   self->name.c_str(), code);
    }
    bool success = reaction.success;
    if (success && self->read_result) {
      try { success = self->read_result(env, arg); } catch (...) { success = false; }
    }
    self->Finish(success);
    return nullptr;
  }
  bool Start(napi_env environment,
             const std::string& method,
             napi_value object,
             const std::vector<napi_value>& args = {}) try {
    env = environment;
    name = method;
    // A raster request may protect cleanup before its CPU work is queued.
    if (!hook) {
      if (napi_add_env_cleanup_hook(env, Cleanup, this) != napi_ok) {
        Finish(false);
        return false;
      }
      hook = true;
    }
    std::vector<napi_value> callbacks;
    for (bool success : {true, false}) {
      auto held = std::make_unique<Reaction>(Reaction{shared_from_this(), success});
      napi_value callback = nullptr;
      if (napi_create_function(env, "nativeapi.sdk", NAPI_AUTO_LENGTH, React, held.get(),
                               &callback) != napi_ok ||
          napi_add_finalizer(
              env, callback, held.get(),
              [](napi_env, void* p, void*) { delete static_cast<Reaction*>(p); }, nullptr,
              nullptr) != napi_ok) {
        Finish(false);
        return false;
      }
      held.release();
      if (!Keep(callback)) {
        Finish(false);
        return false;
      }
      callbacks.push_back(callback);
    }
    napi_value promise = nullptr, ignored = nullptr;
    bool is_promise = false;
    if (!ohos::js::Call(env, object, method.c_str(), args, promise) ||
        napi_is_promise(env, promise, &is_promise) != napi_ok || !is_promise ||
        !ohos::js::Call(env, promise, "then", callbacks, ignored)) {
      Finish(false);
      return false;
    }
    return true;
  } catch (...) {
    Finish(false);
    return false;
  }
  bool Result(bool submitted) {
    if (!submitted)
      return false;
    std::unique_lock<std::mutex> lock(mutex);
    if (IsMainThread())
      return !done || success;
    changed.wait(lock, [&] { return done; });
    return success;
  }
};
}  // namespace nativeapi::ohos
