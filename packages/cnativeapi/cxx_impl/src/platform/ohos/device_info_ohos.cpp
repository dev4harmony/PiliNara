#include "../../device_info.h"
#include "ohos_napi.h"
#include "ohos_runtime.h"

#include <deviceinfo.h>
#include <sys/utsname.h>

namespace nativeapi {
namespace {

std::string String(const char* value) {
  return value ? value : "";
}

class OhosDeviceInfoImpl final : public DeviceInfo::Impl {
 public:
  std::string GetName() const override {
    std::string name;
    // Settings owns the user-assigned name. It is a public synchronous read,
    // performed only on the attached context's UI environment. Before host
    // attachment or on SDK refusal, retain the documented NDK market-name fallback.
    ohos::WithContext([&](napi_env env, napi_value context) {
      napi_value settings = nullptr, general = nullptr, key = nullptr, value = nullptr;
      napi_value empty = nullptr;
      if (napi_create_string_utf8(env, "", 0, &empty) != napi_ok ||
          !ohos::js::Module(env, "@ohos.settings", settings) ||
          !ohos::js::Property(env, settings, "general", general) ||
          !ohos::js::Property(env, general, "DEVICE_NAME", key) ||
          !ohos::js::Call(env, settings, "getValueSync", {context, key, empty}, value)) return;
      name = ohos::js::String(env, value);
    });
    return name.empty() ? String(OH_GetMarketName()) : name;
  }
  std::string GetModel() const override { return String(OH_GetProductModel()); }
  std::string GetManufacturer() const override { return String(OH_GetManufacture()); }

  std::string GetOsName() const override {
    // An unavailable distribution must not be reported as another OS.
    return String(OH_GetDistributionOSName());
  }

  std::string GetOsVersion() const override {
    auto version = String(OH_GetDistributionOSVersion());
    return version.empty() ? String(OH_GetOSFullName()) : version;
  }

  std::string GetKernelVersion() const override {
    utsname info{};
    return uname(&info) == 0 ? info.release : "";
  }

  std::string GetArchitecture() const override {
    // uname.machine describes the kernel, which can differ from this process
    // for a 32-bit ABI or emulated execution. Preserve the native code spelling.
#if defined(__aarch64__)
    return "aarch64";
#elif defined(__x86_64__)
    return "x86_64";
#elif defined(__arm__)
    return "arm";
#elif defined(__i386__)
    return "i686";
#else
    return "";
#endif
  }
};

}  // namespace

DeviceInfo::DeviceInfo() : pimpl_(std::make_unique<OhosDeviceInfoImpl>()) {}
DeviceInfo::~DeviceInfo() = default;

}  // namespace nativeapi
