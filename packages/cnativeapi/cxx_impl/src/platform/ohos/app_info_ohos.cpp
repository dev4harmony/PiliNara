#include "../../app_info.h"
#include "ohos_napi.h"
#include "ohos_runtime.h"

namespace nativeapi {
namespace {
std::string BundleField(const char* name, bool numeric = false) {
  std::string result;
  ohos::WithEnvironment([&](napi_env env) {
    napi_value module, bundle, value;
    // GET_BUNDLE_INFO_WITH_APPLICATION, for the current application only.
    if (!ohos::js::Module(env, "@ohos.bundle.bundleManager", module) ||
        !ohos::js::Call(env, module, "getBundleInfoForSelfSync", {ohos::js::Number(env, 1)},
                        bundle) ||
        !ohos::js::Property(env, bundle, name, value))
      return;
    if (numeric) {
      uint32_t number = 0;
      if (napi_get_value_uint32(env, value, &number) == napi_ok)
        result = std::to_string(number);
    } else {
      result = ohos::js::String(env, value);
    }
  });
  return result;
}
class OhosAppInfoImpl final : public AppInfo::Impl {
 public:
  std::string GetName() const override {
    std::string name;
    ohos::WithContext([&](napi_env env, napi_value context) {
      napi_value info, label, manager, value;
      if (!ohos::js::Property(env, context, "applicationInfo", info))
        return;
      if (ohos::js::Property(env, info, "labelId", label) &&
          ohos::js::Property(env, context, "resourceManager", manager) &&
          ohos::js::Call(env, manager, "getStringSync", {label}, value))
        name = ohos::js::String(env, value);
      if (name.empty())
        name = ohos::js::StringProperty(env, info, "label");
      if (name.empty())
        name = ohos::js::StringProperty(env, info, "name");
    });
    return name;
  }
  std::string GetIdentifier() const override { return BundleField("name"); }
  std::string GetVersion() const override { return BundleField("versionName"); }
  std::string GetBuildNumber() const override { return BundleField("versionCode", true); }
};
}  // namespace
AppInfo::AppInfo() : pimpl_(std::make_unique<OhosAppInfoImpl>()) {}
AppInfo::~AppInfo() = default;
}  // namespace nativeapi
