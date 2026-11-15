#include <condition_variable>
#include <deque>
#include <mutex>
#include "../../foundation/dispatcher.h"
#include "../../notification_manager.h"
#include "ohos_napi.h"
#include "ohos_notifications.h"
#include "ohos_runtime.h"

namespace nativeapi {
namespace {
constexpr int notification_id = 0x4e41;
constexpr const char* argument_key = "nativeapi.notification.argument";
struct Registration {
  std::mutex mutex;
  bool registered = false;
  uint64_t generation = 0;
  std::string error;
};
struct ActivationHub {
  napi_env env = nullptr;
  std::shared_ptr<Registration>* cleanup_data = nullptr;
  std::function<void(const std::string&)> deliver;
  std::deque<std::string> pending;
  bool initialized_once = false;
};
// UI-thread only; outlives the manager's static destruction.
ActivationHub& Hub() {
  static auto* hub = new ActivationHub;
  return *hub;
}
void CleanupRegistration(void* data) {
  auto* holder = static_cast<std::shared_ptr<Registration>*>(data);
  auto state = *holder;
  delete holder;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->registered = false;
    ++state->generation;
  }
  Hub().cleanup_data = nullptr;
  Hub().env = nullptr;
  Hub().deliver = {};
  Hub().pending.clear();
  Hub().initialized_once = false;
}
void DetachRegistration() {
  auto& hub = Hub();
  if (hub.cleanup_data) {
    napi_remove_env_cleanup_hook(hub.env, CleanupRegistration, hub.cleanup_data);
    delete hub.cleanup_data;
    hub.cleanup_data = nullptr;
    hub.env = nullptr;
  }
  hub.deliver = {};
  hub.pending.clear();
  hub.initialized_once = true;
}
napi_value Text(napi_env env, const std::string& value) {
  napi_value result = nullptr;
  napi_create_string_utf8(env, value.data(), value.size(), &result);
  return result;
}
napi_value Object(napi_env env) {
  napi_value value = nullptr;
  napi_create_object(env, &value);
  return value;
}
bool Set(napi_env env, napi_value object, const char* name, napi_value value) {
  return value && napi_set_named_property(env, object, name, value) == napi_ok;
}
std::string Encode(const std::string& value) {
  const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.')
      out += c;
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}
bool TagValid(const std::string& tag) {
  return !tag.empty() && tag.size() <= 192 && tag.find('\0') == tag.npos;
}
std::string SdkError(napi_env env, napi_value error) {
  napi_valuetype type = napi_undefined;
  napi_typeof(env, error, &type);
  if (type == napi_undefined || type == napi_null)
    return {};
  int32_t code = -1;
  napi_value value = nullptr;
  if (type == napi_number)
    napi_get_value_int32(env, error, &code);
  else if (ohos::js::Property(env, error, "code", value))
    napi_get_value_int32(env, value, &code);
  if (code == 0)
    return {};
  return "Notification SDK error (" + std::to_string(code) +
         "): " + ohos::js::StringProperty(env, error, "message");
}
struct Operation : std::enable_shared_from_this<Operation> {
  enum class Kind { Initialize, Show, Remove } kind;
  std::mutex mutex;
  std::condition_variable changed;
  bool done = false, hook = false;
  std::string error = "Notification SDK did not complete";
  std::string title, message, tag, button, bundle, ability, module_name;
  napi_env env = nullptr;
  napi_ref callback_ref = nullptr, request_ref = nullptr;
  int step = 0;
  explicit Operation(Kind value) : kind(value) {}
  static void Cleanup(void* data) {
    auto self = static_cast<Operation*>(data)->shared_from_this();
    self->hook = false;
    self->Finish("Notification UI environment was destroyed");
  }
  void Finish(std::string failure) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (done)
        return;
    }
    if (hook) {
      hook = false;
      napi_remove_env_cleanup_hook(env, Cleanup, this);
    }
    if (callback_ref) {
      napi_delete_reference(env, callback_ref);
      callback_ref = nullptr;
    }
    if (request_ref) {
      napi_delete_reference(env, request_ref);
      request_ref = nullptr;
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      error = std::move(failure);
      done = true;
    }
    changed.notify_all();
  }
  bool Call(const char* module, const char* method, std::vector<napi_value> args) {
    napi_value object = nullptr, function = nullptr, returned = nullptr, callback = nullptr;
    if (!ohos::js::Module(env, module, object) ||
        !ohos::js::Property(env, object, method, function) ||
        napi_get_reference_value(env, callback_ref, &callback) != napi_ok) {
      Finish("Notification SDK method is unavailable");
      return false;
    }
    args.push_back(callback);
    if (napi_call_function(env, object, function, args.size(), args.data(), &returned) != napi_ok) {
      bool pending = false;
      napi_value exception = nullptr;
      napi_is_exception_pending(env, &pending);
      auto failure = std::string("Notification SDK invocation failed");
      if (pending && napi_get_and_clear_last_exception(env, &exception) == napi_ok) {
        auto detail = SdkError(env, exception);
        if (!detail.empty())
          failure = detail;
      }
      Finish(failure);
      return false;
    }
    return true;
  }
  void Agent(bool action_button) {
    const auto argument = "tag=" + Encode(tag) + "&action=" + (action_button ? "button" : "open");
    auto parameters = Object(env), want = Object(env), info = Object(env);
    napi_value wants = nullptr, flags = nullptr;
    napi_create_array_with_length(env, 1, &wants);
    napi_create_array_with_length(env, 1, &flags);
    bool valid =
        Set(env, parameters, argument_key, Text(env, argument)) &&
        Set(env, want, "bundleName", Text(env, bundle)) &&
        Set(env, want, "abilityName", Text(env, ability)) &&
        Set(env, want, "moduleName", Text(env, module_name)) &&
        Set(env, want, "uri", Text(env, "nativeapi-notification://activate?" + argument)) &&
        Set(env, want, "parameters", parameters) &&
        napi_set_element(env, wants, 0, want) == napi_ok &&
        napi_set_element(env, flags, 0, ohos::js::Number(env, 4)) == napi_ok &&
        Set(env, info, "wants", wants) && Set(env, info, "actionType", ohos::js::Number(env, 1)) &&
        Set(env, info, "requestCode", ohos::js::Number(env, notification_id)) &&
        Set(env, info, "actionFlags", flags);
    // CONSTANT_FLAG, START_ABILITY. Distinct immutable intents are keyed by URI.
    if (!valid) {
      Finish("Could not construct notification intent");
      return;
    }
    step = action_button ? 2 : 1;
    Call("@ohos.app.ability.wantAgent", "getWantAgent", {info});
  }
  static napi_value Callback(napi_env env, napi_callback_info info) {
    size_t count = 2;
    napi_value args[2]{}, ignored = nullptr;
    void* data = nullptr;
    napi_get_cb_info(env, info, &count, args, nullptr, &data);
    auto self = *static_cast<std::shared_ptr<Operation>*>(data);
    try {
      bool done;
      {
        std::lock_guard<std::mutex> lock(self->mutex);
        done = self->done;
      }
      if (!done) {
        auto failure = count ? SdkError(env, args[0]) : std::string();
        if (!failure.empty())
          self->Finish(failure);
        else if (self->step == 1 || self->step == 2) {
          napi_value request = nullptr;
          napi_get_reference_value(env, self->request_ref, &request);
          bool valid = count == 2 && args[1];
          if (self->step == 1)
            valid = valid && Set(env, request, "wantAgent", args[1]);
          else {
            auto button = Object(env);
            napi_value buttons = nullptr;
            napi_create_array_with_length(env, 1, &buttons);
            valid = valid && Set(env, button, "title", Text(env, self->button)) &&
                    Set(env, button, "wantAgent", args[1]) &&
                    napi_set_element(env, buttons, 0, button) == napi_ok &&
                    Set(env, request, "actionButtons", buttons);
          }
          if (!valid)
            self->Finish("Invalid notification WantAgent result");
          else if (self->step == 1 && !self->button.empty())
            self->Agent(true);
          else {
            self->step = 3;
            self->Call("@ohos.notificationManager", "publish", {request});
          }
        } else
          self->Finish({});
      }
    } catch (...) {
      self->Finish("Could not process notification SDK result");
    }
    napi_get_undefined(env, &ignored);
    return ignored;
  }
  void Start(napi_env environment, napi_value context) {
    env = environment;
    if (napi_add_env_cleanup_hook(env, Cleanup, this) != napi_ok) {
      Finish("Cannot attach notification environment cleanup");
      return;
    }
    hook = true;
    auto* held = new std::shared_ptr<Operation>(shared_from_this());
    napi_value callback = nullptr;
    if (napi_create_function(env, "nativeapi.notification", NAPI_AUTO_LENGTH, Callback, held,
                             &callback) != napi_ok) {
      delete held;
      Finish("Cannot create notification callback");
      return;
    }
    if (napi_add_finalizer(
            env, callback, held,
            [](napi_env, void* data, void*) {
              delete static_cast<std::shared_ptr<Operation>*>(data);
            },
            nullptr, nullptr) != napi_ok) {
      delete held;
      Finish("Cannot retain notification callback");
      return;
    }
    if (napi_create_reference(env, callback, 1, &callback_ref) != napi_ok) {
      Finish("Cannot retain notification callback");
      return;
    }
    if (kind == Kind::Initialize) {
      napi_value module = nullptr, enabled = nullptr;
      bool allowed = false;
      if (ohos::js::Module(env, "@ohos.notificationManager", module) &&
          ohos::js::Call(env, module, "isNotificationEnabledSync", {}, enabled))
        napi_get_value_bool(env, enabled, &allowed);
      if (allowed)
        Finish({});
      else
        Call("@ohos.notificationManager", "requestEnableNotification", {context});
    } else if (kind == Kind::Remove) {
      Call("@ohos.notificationManager", "cancel",
           {ohos::js::Number(env, notification_id), Text(env, "nativeapi:" + tag)});
    } else {
      napi_value ability_info = nullptr;
      if (ohos::js::Property(env, context, "abilityInfo", ability_info)) {
        bundle = ohos::js::StringProperty(env, ability_info, "bundleName");
        ability = ohos::js::StringProperty(env, ability_info, "name");
        module_name = ohos::js::StringProperty(env, ability_info, "moduleName");
      }
      if (bundle.empty() || ability.empty()) {
        Finish("Notification Ability metadata is unavailable");
        return;
      }
      auto request = Object(env), content = Object(env), normal = Object(env);
      const bool valid = Set(env, normal, "title", Text(env, title)) &&
                         Set(env, normal, "text", Text(env, message)) &&
                         Set(env, content, "notificationContentType", ohos::js::Number(env, 0)) &&
                         Set(env, content, "normal", normal) &&
                         Set(env, request, "content", content) &&
                         Set(env, request, "id", ohos::js::Number(env, notification_id)) &&
                         Set(env, request, "label", Text(env, "nativeapi:" + tag)) &&
                         napi_create_reference(env, request, 1, &request_ref) == napi_ok;
      if (!valid) {
        Finish("Cannot construct notification content");
        return;
      }
      Agent(false);
    }
  }
  bool Run() {
    if (IsMainThread()) {
      error = "Run notification operations from a worker; the UI thread must remain responsive";
      return false;
    }
    if (!ohos::WithContext([&](napi_env env, napi_value context) { Start(env, context); })) {
      error = "Notification UIAbilityContext is not attached";
      return false;
    }
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait(lock, [&] { return done; });
    return error.empty();
  }
};
}  // namespace
namespace ohos {
bool HandleNotificationWant(napi_env env, napi_value want) {
  if (!IsMainThread())
    return false;
  napi_value parameters = nullptr;
  if (!js::Property(env, want, "parameters", parameters))
    return false;
  auto argument = js::StringProperty(env, parameters, argument_key);
  if (argument.empty())
    return false;
  auto& hub = Hub();
  if (hub.deliver)
    hub.deliver(argument);
  else if (!hub.initialized_once) {
    if (hub.pending.size() == 64)
      hub.pending.pop_front();
    hub.pending.push_back(argument);
  }
  return true;
}
}  // namespace ohos
class NotificationManager::Impl {
 public:
  std::shared_ptr<Registration> state = std::make_shared<Registration>();
  bool Result(const std::shared_ptr<Operation>& operation, bool ok, uint64_t expected) {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->generation != expected) {
      state->error = "Notification registration changed during operation";
      return false;
    }
    state->error = operation->error;
    return ok;
  }
};
NotificationManager::NotificationManager() : pimpl_(std::make_unique<Impl>()) {}
NotificationManager& NotificationManager::GetInstance() {
  static NotificationManager instance;
  return instance;
}
NotificationManager::~NotificationManager() {
  ShutdownEmitter();
  Shutdown();
}
bool NotificationManager::IsSupported() {
  bool supported = false;
  ohos::WithEnvironment([&](napi_env env) {
    napi_value module = nullptr, method = nullptr;
    napi_valuetype type;
    supported = ohos::js::Module(env, "@ohos.notificationManager", module) &&
                ohos::js::Property(env, module, "publish", method) &&
                napi_typeof(env, method, &type) == napi_ok && type == napi_function;
  });
  return supported;
}
bool NotificationManager::Initialize() {
  uint64_t generation;
  {
    std::lock_guard<std::mutex> lock(pimpl_->state->mutex);
    if (pimpl_->state->registered) {
      pimpl_->state->error.clear();
      return true;
    }
    generation = pimpl_->state->generation;
  }
  auto operation = std::make_shared<Operation>(Operation::Kind::Initialize);
  const bool ok = operation->Run();
  if (!pimpl_->Result(operation, ok, generation) || !ok)
    return false;
  bool attached = false;
  ohos::WithEnvironment([&](napi_env env) {
    std::lock_guard<std::mutex> lock(pimpl_->state->mutex);
    if (pimpl_->state->generation != generation)
      return;
    if (pimpl_->state->registered) {
      attached = true;
      return;
    }
    auto* cleanup_data = new std::shared_ptr<Registration>(pimpl_->state);
    if (napi_add_env_cleanup_hook(env, CleanupRegistration, cleanup_data) != napi_ok) {
      delete cleanup_data;
      pimpl_->state->error = "Cannot attach notification registration cleanup";
      return;
    }
    Hub().env = env;
    Hub().cleanup_data = cleanup_data;
    pimpl_->state->registered = true;
    Hub().initialized_once = true;
    Hub().deliver = CreateGuardedCallback(std::function<void(const std::string&)>(
        [this](const std::string& argument) { EmitAsync<NotificationActivatedEvent>(argument); }));
    auto pending = std::move(Hub().pending);
    Hub().pending.clear();
    for (const auto& argument : pending)
      Hub().deliver(argument);
    attached = true;
  });
  if (!attached) {
    std::lock_guard<std::mutex> lock(pimpl_->state->mutex);
    if (pimpl_->state->error.empty())
      pimpl_->state->error = "Notification host or registration changed during initialization";
  }
  return attached;
}
void NotificationManager::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(pimpl_->state->mutex);
    pimpl_->state->registered = false;
    ++pimpl_->state->generation;
  }
  ohos::RunSync(DetachRegistration);
}
bool NotificationManager::Show(const std::string& title,
                               const std::string& message,
                               const std::string& tag,
                               const std::string& button) {
  uint64_t generation;
  {
    std::lock_guard<std::mutex> lock(pimpl_->state->mutex);
    if (!pimpl_->state->registered) {
      pimpl_->state->error = "Initialize notifications first";
      return false;
    }
    if (!TagValid(tag) || title.empty() || message.empty() || title.find('\0') != title.npos ||
        message.find('\0') != message.npos || button.find('\0') != button.npos) {
      pimpl_->state->error =
          "Notifications require nonempty title/message and a 1-192 byte tag without NUL";
      return false;
    }
    generation = pimpl_->state->generation;
  }
  auto operation = std::make_shared<Operation>(Operation::Kind::Show);
  operation->title = title;
  operation->message = message;
  operation->tag = tag;
  operation->button = button;
  const bool ok = operation->Run();
  return pimpl_->Result(operation, ok, generation);
}
bool NotificationManager::Remove(const std::string& tag) {
  uint64_t generation;
  {
    std::lock_guard<std::mutex> lock(pimpl_->state->mutex);
    if (!pimpl_->state->registered || !TagValid(tag)) {
      pimpl_->state->error = "Initialize notifications and provide a valid tag first";
      return false;
    }
    generation = pimpl_->state->generation;
  }
  auto operation = std::make_shared<Operation>(Operation::Kind::Remove);
  operation->tag = tag;
  const bool ok = operation->Run();
  return pimpl_->Result(operation, ok, generation);
}
std::string NotificationManager::GetLastError() const {
  std::lock_guard<std::mutex> lock(pimpl_->state->mutex);
  return pimpl_->state->error;
}
}  // namespace nativeapi
