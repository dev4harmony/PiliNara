#include "../../preferences.h"

#include <database/preferences/oh_preferences.h>
#include <database/preferences/oh_preferences_err_code.h>
#include <mutex>

#if !defined(OH_CURRENT_API_VERSION) || OH_CURRENT_API_VERSION < 23
#error "nativeapi OHOS preferences require an OpenHarmony/HarmonyOS NDK at API 23 or later"
#endif

namespace nativeapi {
namespace {

// OH_Preferences_Close removes the SDK's per-file cache. Multiple nativeapi
// objects must therefore share one handle, closed only after the final owner.
struct PreferenceStore {
  OH_Preferences* preferences = nullptr;
  std::mutex mutex;

  explicit PreferenceStore(const std::string& scope) {
    auto* options = OH_PreferencesOption_Create();
    if (!options) return;
    // Byte encoding preserves exact scope identity and cannot escape the sandbox.
    static constexpr char hex[] = "0123456789abcdef";
    std::string filename = "nativeapi_";
    for (unsigned char c : scope) {
      filename += hex[c >> 4];
      filename += hex[c & 15];
    }
    int error = PREFERENCES_ERROR_INVALID_PARAM;
    if (OH_PreferencesOption_SetFileName(options, filename.c_str()) == PREFERENCES_OK)
      preferences = OH_Preferences_Open(options, &error);
    OH_PreferencesOption_Destroy(options);
  }

  ~PreferenceStore() {
    if (preferences) OH_Preferences_Close(preferences);
  }
};

struct PreferenceStores {
  std::mutex mutex;
  std::map<std::string, std::shared_ptr<PreferenceStore>> stores;
};

PreferenceStores& Stores() {
  static PreferenceStores stores;
  return stores;
}

std::shared_ptr<PreferenceStore> OpenStore(const std::string& scope) {
  auto& registry = Stores();
  std::lock_guard<std::mutex> lock(registry.mutex);
  auto& store = registry.stores[scope];
  if (!store) store = std::make_shared<PreferenceStore>(scope);
  return store;
}

void CloseStore(const std::string& scope, std::shared_ptr<PreferenceStore>& store) {
  auto& registry = Stores();
  std::lock_guard<std::mutex> lock(registry.mutex);
  // The registry owns one reference; each Impl owns exactly one more. Serialize
  // the last SDK close with new opens, so an old close cannot evict a new store.
  const auto it = registry.stores.find(scope);
  if (it != registry.stores.end() && it->second == store && store.use_count() == 2)
    registry.stores.erase(it);
  store.reset();
}

}  // namespace

class Preferences::Impl {
 public:
  explicit Impl(const std::string& scope) : scope_(scope), store_(OpenStore(scope)) {}
  ~Impl() { CloseStore(scope_, store_); }

  bool Set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(store_->mutex);
    return store_->preferences && Valid(key) && Valid(value) &&
           OH_Preferences_SetString(store_->preferences, key.c_str(), value.c_str()) == PREFERENCES_OK &&
           OH_Preferences_Flush(store_->preferences) == PREFERENCES_OK;
  }

  std::string Get(const std::string& key, const std::string& default_value) const {
    std::lock_guard<std::mutex> lock(store_->mutex);
    char* value = nullptr;
    uint32_t length = 0;
    if (!store_->preferences || !Valid(key) ||
        OH_Preferences_GetString(store_->preferences, key.c_str(), &value, &length) != PREFERENCES_OK)
      return default_value;
    std::unique_ptr<char, decltype(&OH_Preferences_FreeString)> owned(value,
                                                                      OH_Preferences_FreeString);
    return value ? std::string(value) : default_value;
  }

  bool Remove(const std::string& key) {
    std::lock_guard<std::mutex> lock(store_->mutex);
    // The NDK treats deleting a missing key as success; Storage::Remove does not.
    return store_->preferences && Valid(key) && OH_Preferences_HasKey(store_->preferences, key.c_str()) &&
           OH_Preferences_Delete(store_->preferences, key.c_str()) == PREFERENCES_OK &&
           OH_Preferences_Flush(store_->preferences) == PREFERENCES_OK;
  }

  bool Clear() {
    std::lock_guard<std::mutex> lock(store_->mutex);
    return store_->preferences && OH_Preferences_ClearCache(store_->preferences) == PREFERENCES_OK &&
           OH_Preferences_Flush(store_->preferences) == PREFERENCES_OK;
  }

  bool Contains(const std::string& key) const {
    std::lock_guard<std::mutex> lock(store_->mutex);
    return store_->preferences && Valid(key) && OH_Preferences_HasKey(store_->preferences, key.c_str());
  }

  std::vector<std::string> GetKeys() const {
    std::vector<std::string> keys;
    for (const auto& entry : GetAll())
      keys.push_back(entry.first);
    return keys;
  }

  size_t GetSize() const { return GetAll().size(); }

  std::map<std::string, std::string> GetAll() const {
    std::lock_guard<std::mutex> lock(store_->mutex);
    OH_PreferencesPair* pairs = nullptr;
    uint32_t count = 0;
    if (!store_->preferences || OH_Preferences_GetAll(store_->preferences, &pairs, &count) != PREFERENCES_OK)
      return {};
    // Native pairs and strings are independent allocations.
    auto cleanup = [count](OH_PreferencesPair* p) { OH_PreferencesPair_Destroy(p, count); };
    std::unique_ptr<OH_PreferencesPair, decltype(cleanup)> owned(pairs, cleanup);
    std::map<std::string, std::string> result;
    for (uint32_t i = 0; i < count; ++i) {
      const char* key = OH_PreferencesPair_GetKey(pairs, i);
      const auto* native_value = OH_PreferencesPair_GetPreferencesValue(pairs, i);
      char* value = nullptr;
      uint32_t length = 0;
      if (key && native_value &&
          OH_PreferencesValue_GetString(native_value, &value, &length) == PREFERENCES_OK) {
        std::unique_ptr<char, decltype(&OH_Preferences_FreeString)> string(
            value, OH_Preferences_FreeString);
        result.emplace(key, value ? value : "");
      }
    }
    return result;
  }

  const std::string& GetScope() const { return scope_; }

 private:
  static bool Valid(const std::string& value) { return value.find('\0') == std::string::npos; }
  std::string scope_;
  std::shared_ptr<PreferenceStore> store_;
};

// Constructor implementations
Preferences::Preferences() : Preferences("default") {}

Preferences::Preferences(const std::string& scope) : pimpl_(std::make_unique<Impl>(scope)) {}

Preferences::~Preferences() = default;

// Interface implementation
bool Preferences::Set(const std::string& key, const std::string& value) {
  return pimpl_->Set(key, value);
}

std::string Preferences::Get(const std::string& key, const std::string& default_value) const {
  return pimpl_->Get(key, default_value);
}

bool Preferences::Remove(const std::string& key) {
  return pimpl_->Remove(key);
}

bool Preferences::Clear() {
  return pimpl_->Clear();
}

bool Preferences::Contains(const std::string& key) const {
  return pimpl_->Contains(key);
}

std::vector<std::string> Preferences::GetKeys() const {
  return pimpl_->GetKeys();
}

size_t Preferences::GetSize() const {
  return pimpl_->GetSize();
}

std::map<std::string, std::string> Preferences::GetAll() const {
  return pimpl_->GetAll();
}

std::string Preferences::GetScope() const {
  return pimpl_->GetScope();
}

}  // namespace nativeapi
