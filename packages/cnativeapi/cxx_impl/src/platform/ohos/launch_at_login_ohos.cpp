#include "../../launch_at_login.h"
#include "../../app_info.h"
#include "ohos_runtime.h"
#include "ohos_sdk_request.h"

namespace nativeapi {
class LaunchAtLogin::Impl {
 public:
  // The public SDK can read self status, but cannot register/enable/disable it.
  static bool IsSupported() { return false; }
  Impl() : Impl("", "") { default_id_ = true; default_name_ = true; }
  explicit Impl(const std::string& id) : Impl(id, "") { default_name_ = true; }
  Impl(const std::string& id, const std::string& name) : id_(id), display_name_(name) {}
  std::string GetId() const {
    return default_id_ ? AppInfo::GetInstance().GetIdentifier() : id_;
  }
  std::string GetDisplayName() const {
    return default_name_ ? AppInfo::GetInstance().GetName() : display_name_;
  }
  bool SetDisplayName(const std::string& name) {
    display_name_ = name;
    default_name_ = false;
    return true;  // Metadata only; the system always displays the bundle label.
  }
  bool SetProgram(const std::string&, const std::vector<std::string>&) { return false; }
  std::string GetExecutablePath() const { return {}; }
  std::vector<std::string> GetArguments() const { return {}; }
  bool Enable() { return false; }
  bool Disable() { return false; }
  bool IsEnabled() const {
    if (!ohos::IsInitialized()) return false;
    const auto id = GetId();
    if (id.empty() || id != AppInfo::GetInstance().GetIdentifier()) return false;
    std::shared_ptr<ohos::SdkRequest> request;
    std::shared_ptr<bool> result;
    bool submitted = false;
    const bool used = ohos::WithEnvironment([&](napi_env env) {
      const auto epoch = ohos::EnvironmentGeneration();
      {
        std::lock_guard<std::mutex> lock(status_->mutex);
        if (status_->generation != epoch) {
          status_->generation = epoch;
          status_->enabled = false;
          status_->request.reset();
        }
        if (status_->request) {
          std::lock_guard<std::mutex> waiting(status_->request->mutex);
          if (!status_->request->done) {
            request = status_->request;
            result = status_->result;
            submitted = true;
            return;
          }
        }
      }
      request = std::make_shared<ohos::SdkRequest>();
      result = std::make_shared<bool>(false);
      request->read_result = [result](napi_env env, napi_value value) {
        return napi_get_value_bool(env, value, result.get()) == napi_ok;
      };
      request->completed = [weak = std::weak_ptr<Status>(status_), epoch, result](bool ok) {
        if (auto status = weak.lock()) {
          std::lock_guard<std::mutex> lock(status->mutex);
          if (status->generation == epoch) status->enabled = ok && *result;
        }
      };
      {
        std::lock_guard<std::mutex> lock(status_->mutex);
        status_->request = request;
        status_->result = result;
      }
      napi_value module = nullptr;
      if (ohos::js::Module(env, "@ohos.app.ability.autoStartupManager", module))
        submitted = request->Start(env, "getAutoStartupStatusForSelf", module);
      else {
        // No hook or JS roots exist if module loading failed.
        request->env = env;
        request->Finish(false);
      }
    });
    if (!used || !request || !submitted) return false;
    if (!IsMainThread()) return request->Result(true) && *result;
    // Never block the UI event loop on its own SDK Promise. UI queries refresh
    // and expose the last completed value in this environment, initially false.
    std::lock_guard<std::mutex> lock(status_->mutex);
    return status_->enabled;
  }
 private:
  struct Status {
    std::mutex mutex;
    uint64_t generation = 0;
    bool enabled = false;
    std::shared_ptr<ohos::SdkRequest> request;
    std::shared_ptr<bool> result;
  };
  std::string id_, display_name_;
  bool default_id_ = false, default_name_ = false;
  std::shared_ptr<Status> status_ = std::make_shared<Status>();
};
LaunchAtLogin::LaunchAtLogin() : pimpl_(std::make_unique<Impl>()) {}
LaunchAtLogin::LaunchAtLogin(const std::string& id) : pimpl_(std::make_unique<Impl>(id)) {}
LaunchAtLogin::LaunchAtLogin(const std::string& id, const std::string& display_name)
    : pimpl_(std::make_unique<Impl>(id, display_name)) {}
LaunchAtLogin::~LaunchAtLogin() = default;
bool LaunchAtLogin::IsSupported() { return Impl::IsSupported(); }
std::string LaunchAtLogin::GetId() const { return pimpl_->GetId(); }
std::string LaunchAtLogin::GetDisplayName() const { return pimpl_->GetDisplayName(); }
bool LaunchAtLogin::SetDisplayName(const std::string& name) { return pimpl_->SetDisplayName(name); }
bool LaunchAtLogin::SetProgram(const std::string& path, const std::vector<std::string>& args) {
  return pimpl_->SetProgram(path, args);
}
std::string LaunchAtLogin::GetExecutablePath() const { return pimpl_->GetExecutablePath(); }
std::vector<std::string> LaunchAtLogin::GetArguments() const { return pimpl_->GetArguments(); }
bool LaunchAtLogin::Enable() { return pimpl_->Enable(); }
bool LaunchAtLogin::Disable() { return pimpl_->Disable(); }
bool LaunchAtLogin::IsEnabled() const { return pimpl_->IsEnabled(); }
}  // namespace nativeapi
