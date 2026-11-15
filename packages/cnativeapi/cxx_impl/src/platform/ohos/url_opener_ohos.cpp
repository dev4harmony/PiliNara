#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <mutex>
#include "../../foundation/dispatcher.h"
#include "../../url_opener.h"
#include "ohos_napi.h"
#include "ohos_runtime.h"

namespace nativeapi {
namespace {
UrlOpenResult Failure(const std::string& message) {
  return {false, UrlOpenErrorCode::kInvocationFailed, message};
}

std::string Link(const std::string& raw) {
  auto first =
      std::find_if_not(raw.begin(), raw.end(), [](unsigned char c) { return std::isspace(c); });
  auto last = std::find_if_not(raw.rbegin(), raw.rend(), [](unsigned char c) {
                return std::isspace(c);
              }).base();
  if (first >= last)
    return {};
  std::string link(first, last);
  auto colon = link.find(':');
  if (colon == link.npos)
    return link;
  std::transform(link.begin(), link.begin() + colon, link.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return link;
}

napi_value Text(napi_env env, const std::string& text) {
  napi_value value = nullptr;
  napi_create_string_utf8(env, text.data(), text.size(), &value);
  return value;
}

std::string Error(napi_env env, napi_value value) {
  napi_value code_value = nullptr;
  int32_t code = 0;
  napi_valuetype type = napi_undefined;
  napi_typeof(env, value, &type);
  if (type == napi_number)
    napi_get_value_int32(env, value, &code);
  else if (ohos::js::Property(env, value, "code", code_value))
    napi_get_value_int32(env, code_value, &code);
  return "System URL dispatch failed (" + std::to_string(code) +
         "): " + ohos::js::StringProperty(env, value, "message");
}

bool ValidLink(napi_env env, const std::string& link) {
  if (link.find('\0') != link.npos ||
      std::any_of(link.begin(), link.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
    return false;
  napi_value module = nullptr, constructor = nullptr, parsed = nullptr;
  auto input = Text(env, link);
  if (!ohos::js::Module(env, "@ohos.url", module) ||
      !ohos::js::Property(env, module, "URL", constructor) ||
      napi_new_instance(env, constructor, 1, &input, &parsed) != napi_ok) {
    ohos::js::ClearException(env);
    return false;
  }
  return !ohos::js::StringProperty(env, parsed, "hostname").empty();
}

struct Dispatch : std::enable_shared_from_this<Dispatch> {
  std::mutex mutex;
  std::condition_variable changed;
  bool done = false, cleanup_hook = false;
  napi_env env = nullptr;
  UrlOpenResult result = Failure("System URL dispatch did not complete");

  void Finish(UrlOpenResult value) {
    // SDK callbacks and environment cleanup run on the owning UI thread.
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (done)
        return;
    }
    if (cleanup_hook) {
      cleanup_hook = false;
      napi_remove_env_cleanup_hook(env, Cleanup, this);
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      result = std::move(value);
      done = true;
    }
    changed.notify_all();
  }
  static void Cleanup(void* data) {
    auto self = static_cast<Dispatch*>(data)->shared_from_this();
    self->cleanup_hook = false;
    self->Finish(Failure("URL dispatch UI environment was destroyed"));
  }
  static napi_value Completed(napi_env env, napi_callback_info info) {
    size_t count = 1;
    napi_value error = nullptr, ignored = nullptr;
    void* data = nullptr;
    napi_get_cb_info(env, info, &count, &error, nullptr, &data);
    auto self = *static_cast<std::shared_ptr<Dispatch>*>(data);
    try {
      napi_valuetype type = napi_undefined;
      napi_value null_value = nullptr, code_value = nullptr;
      bool is_null = false;
      napi_get_null(env, &null_value);
      if (count && error) {
        napi_typeof(env, error, &type);
        napi_strict_equals(env, error, null_value, &is_null);
      }
      int32_t code = -1;
      bool zero =
          type == napi_number && napi_get_value_int32(env, error, &code) == napi_ok && code == 0;
      if (type == napi_object && !is_null && ohos::js::Property(env, error, "code", code_value))
        zero = napi_get_value_int32(env, code_value, &code) == napi_ok && code == 0;
      self->Finish(type == napi_undefined || is_null || zero
                       ? UrlOpenResult{true, UrlOpenErrorCode::kNone, {}}
                       : Failure(Error(env, error)));
    } catch (...) {
      self->Finish(Failure("Could not process the URL dispatch result"));
    }
    napi_get_undefined(env, &ignored);
    return ignored;
  }
  bool Start(napi_env environment, napi_value context, const std::string& link) {
    env = environment;
    napi_value want = nullptr, callback = nullptr, method = nullptr, ignored = nullptr;
    if (!ValidLink(env, link) || napi_create_object(env, &want) != napi_ok ||
        napi_set_named_property(env, want, "uri", Text(env, link)) != napi_ok ||
        napi_set_named_property(env, want, "action", Text(env, "ohos.want.action.viewData")) !=
            napi_ok ||
        !ohos::js::Property(env, context, "startAbility", method) ||
        napi_add_env_cleanup_hook(env, Cleanup, this) != napi_ok)
      return false;
    cleanup_hook = true;
    auto* holder = new std::shared_ptr<Dispatch>(shared_from_this());
    if (napi_create_function(env, "nativeapi.urlDispatch", NAPI_AUTO_LENGTH, Completed, holder,
                             &callback) != napi_ok) {
      delete holder;
      return false;
    }
    if (napi_add_finalizer(
            env, callback, holder,
            [](napi_env, void* data, void*) {
              delete static_cast<std::shared_ptr<Dispatch>*>(data);
            },
            nullptr, nullptr) != napi_ok) {
      delete holder;
      return false;
    }
    napi_value arguments[] = {want, callback};
    if (napi_call_function(env, context, method, 2, arguments, &ignored) != napi_ok) {
      bool pending = false;
      napi_is_exception_pending(env, &pending);
      napi_value error = nullptr;
      if (pending && napi_get_and_clear_last_exception(env, &error) == napi_ok)
        Finish(Failure(Error(env, error)));
      return false;
    }
    return true;
  }
};

class OhosUrlOpenerImpl final : public UrlOpener::Impl {
 public:
  bool IsSupported() const override {
    bool supported = false;
    ohos::WithContext([&](napi_env env, napi_value context) {
      napi_value method = nullptr;
      napi_valuetype type;
      supported = ohos::js::Property(env, context, "startAbility", method) &&
                  napi_typeof(env, method, &type) == napi_ok && type == napi_function;
    });
    return supported;
  }

  bool CanOpen(const std::string& raw) const override {
    bool supported = false;
    const auto link = Link(raw);
    ohos::WithContext([&](napi_env env, napi_value) {
      napi_value module = nullptr, value = nullptr;
      if (ValidLink(env, link) && ohos::js::Module(env, "@ohos.bundle.bundleManager", module) &&
          ohos::js::Call(env, module, "canOpenLink", {Text(env, link)}, value))
        napi_get_value_bool(env, value, &supported);
    });
    return supported;
  }

  UrlOpenResult Open(const std::string& raw) const override {
    try {
      if (IsMainThread())
        return Failure("Open URLs from a worker; the ArkTS UI thread must remain responsive");
      auto session = std::make_shared<Dispatch>();
      const auto link = Link(raw);
      if (!ohos::RunSync([&] {
            const bool attached = ohos::WithContext([&](napi_env env, napi_value context) {
              if (!session->Start(env, context, link))
                session->Finish(
                    Failure("Cannot dispatch URL: invalid link or SDK invocation failure"));
            });
            if (!attached)
              session->Finish(Failure("The URL dispatch UIAbilityContext is not attached"));
          }))
        return Failure("The URL dispatch UI host is not initialized or is shutting down");
      std::unique_lock<std::mutex> lock(session->mutex);
      session->changed.wait(lock, [&] { return session->done; });
      return session->result;
    } catch (...) {
      return Failure("Unexpected URL dispatch failure");
    }
  }
};

}  // namespace

UrlOpener::UrlOpener() : pimpl_(std::make_unique<OhosUrlOpenerImpl>()) {}

UrlOpener::~UrlOpener() = default;

}  // namespace nativeapi
