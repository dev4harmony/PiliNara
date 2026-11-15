#include "../../secure_storage.h"

#include <asset/asset_api.h>
#include <algorithm>
#include <optional>
#include <utility>

namespace nativeapi {
namespace {
Asset_Attr Bytes(Asset_Tag tag, const std::string& text) {
  Asset_Attr attr{};
  attr.tag = tag;
  attr.value.blob = {static_cast<uint32_t>(text.size()),
                     reinterpret_cast<uint8_t*>(const_cast<char*>(text.data()))};
  return attr;
}
Asset_Attr Number(Asset_Tag tag, uint32_t number) {
  Asset_Attr attr{};
  attr.tag = tag;
  attr.value.u32 = number;
  return attr;
}
struct Results {
  Asset_ResultSet value{};
  ~Results() { OH_Asset_FreeResultSet(&value); }
};
std::optional<std::string> Blob(const Asset_Result& result, Asset_Tag tag) {
  auto* attr = OH_Asset_ParseAttr(&result, tag);
  if (!attr || (!attr->value.blob.data && attr->value.blob.size))
    return std::nullopt;
  return attr->value.blob.size
             ? std::string(reinterpret_cast<char*>(attr->value.blob.data), attr->value.blob.size)
             : std::string{};
}
}  // namespace

class SecureStorage::Impl {
 public:
  explicit Impl(std::string scope)
      : scope_(std::move(scope)),
        prefix_("nativeapi.secure.v1:" + std::to_string(scope_.size()) + ":" + scope_ + ":") {}
  bool ValidScope() const { return prefix_.size() < 256 && scope_.find('\0') == scope_.npos; }
  bool ValidKey(const std::string& key) const {
    return ValidScope() && prefix_.size() + key.size() <= 256 && key.find('\0') == key.npos;
  }
  std::vector<Asset_Attr> Query() const {
    // Critical labels are immutable and scope every enumeration/removal to this
    // library's own assets. Scope/key metadata is public; only SECRET is sensitive.
    return {Bytes(ASSET_TAG_DATA_LABEL_CRITICAL_1, prefix_)};
  }
  bool Set(const std::string& key, const std::string& value) {
    // ASSET accepts secrets of 1..1024 bytes. The format byte represents empty
    // strings as well, and makes corrupt/foreign payloads distinguishable.
    if (!ValidKey(key) || value.size() > 1023)
      return false;
    const std::string alias = prefix_ + key;
    const std::string secret = std::string(1, '\1') + value;
    auto attrs = Query();
    attrs.push_back(Bytes(ASSET_TAG_ALIAS, alias));
    attrs.push_back(Bytes(ASSET_TAG_SECRET, secret));
    attrs.push_back(Number(ASSET_TAG_ACCESSIBILITY, ASSET_ACCESSIBILITY_DEVICE_FIRST_UNLOCKED));
    attrs.push_back(Number(ASSET_TAG_CONFLICT_RESOLUTION, ASSET_CONFLICT_OVERWRITE));
    return OH_Asset_Add(attrs.data(), attrs.size()) == ASSET_SUCCESS;
  }
  std::optional<std::string> Read(const std::string& key) const {
    if (!ValidKey(key))
      return std::nullopt;
    const std::string alias = prefix_ + key;
    auto query = Query();
    query.push_back(Bytes(ASSET_TAG_ALIAS, alias));
    query.push_back(Number(ASSET_TAG_RETURN_TYPE, ASSET_RETURN_ALL));
    Results result;
    if (OH_Asset_Query(query.data(), query.size(), &result.value) != ASSET_SUCCESS ||
        result.value.count != 1)
      return std::nullopt;
    const auto secret = Blob(result.value.results[0], ASSET_TAG_SECRET);
    return secret && !secret->empty() && secret->front() == '\1'
               ? std::optional<std::string>(secret->substr(1))
               : std::nullopt;
  }
  std::string Get(const std::string& key, const std::string& fallback) const {
    const auto value = Read(key);
    return value ? *value : fallback;
  }
  bool Remove(const std::string& key) {
    if (!ValidKey(key))
      return false;
    const std::string alias = prefix_ + key;
    auto query = Query();
    query.push_back(Bytes(ASSET_TAG_ALIAS, alias));
    const auto result = OH_Asset_Remove(query.data(), query.size());
    return result == ASSET_SUCCESS;
  }
  bool Clear() {
    if (!ValidScope())
      return false;
    auto query = Query();
    const auto result = OH_Asset_Remove(query.data(), query.size());
    return result == ASSET_SUCCESS || result == ASSET_NOT_FOUND;
  }
  std::vector<std::string> GetKeys() const {
    if (!ValidScope())
      return {};
    auto query = Query();
    query.push_back(Number(ASSET_TAG_RETURN_TYPE, ASSET_RETURN_ATTRIBUTES));
    Results result;
    if (OH_Asset_Query(query.data(), query.size(), &result.value) != ASSET_SUCCESS)
      return {};
    std::vector<std::string> keys;
    for (uint32_t i = 0; i < result.value.count; ++i) {
      auto alias = Blob(result.value.results[i], ASSET_TAG_ALIAS);
      if (alias && alias->compare(0, prefix_.size(), prefix_) == 0)
        keys.push_back(alias->substr(prefix_.size()));
    }
    std::sort(keys.begin(), keys.end());
    return keys;
  }
  bool Contains(const std::string& key) const {
    if (!ValidKey(key))
      return false;
    const std::string alias = prefix_ + key;
    auto query = Query();
    query.push_back(Bytes(ASSET_TAG_ALIAS, alias));
    query.push_back(Number(ASSET_TAG_RETURN_TYPE, ASSET_RETURN_ATTRIBUTES));
    Results result;
    return OH_Asset_Query(query.data(), query.size(), &result.value) == ASSET_SUCCESS &&
           result.value.count == 1;
  }
  std::map<std::string, std::string> GetAll() const {
    std::map<std::string, std::string> values;
    for (const auto& key : GetKeys()) {
      // Query each secret individually: the public ASSET API permits bulk
      // attribute enumeration but only one secret per authenticated query.
      auto value = Read(key);
      if (value)
        values.emplace(key, std::move(*value));
    }
    return values;
  }
  std::string scope_, prefix_;
};
SecureStorage::SecureStorage() : SecureStorage("default") {}
SecureStorage::SecureStorage(const std::string& scope) : pimpl_(std::make_unique<Impl>(scope)) {}
SecureStorage::~SecureStorage() = default;
bool SecureStorage::Set(const std::string& key, const std::string& value) {
  return pimpl_->Set(key, value);
}
std::string SecureStorage::Get(const std::string& key, const std::string& fallback) const {
  return pimpl_->Get(key, fallback);
}
bool SecureStorage::Remove(const std::string& key) {
  return pimpl_->Remove(key);
}
bool SecureStorage::Clear() {
  return pimpl_->Clear();
}
bool SecureStorage::Contains(const std::string& key) const {
  return pimpl_->Contains(key);
}
std::vector<std::string> SecureStorage::GetKeys() const {
  return pimpl_->GetKeys();
}
size_t SecureStorage::GetSize() const {
  return pimpl_->GetKeys().size();
}
std::map<std::string, std::string> SecureStorage::GetAll() const {
  return pimpl_->GetAll();
}
std::string SecureStorage::GetScope() const {
  return pimpl_->scope_;
}
bool SecureStorage::IsAvailable() {
  const std::string scope = "nativeapi.secure.v1:capability-probe";
  Asset_Attr query[] = {Bytes(ASSET_TAG_DATA_LABEL_CRITICAL_1, scope),
                        Number(ASSET_TAG_RETURN_TYPE, ASSET_RETURN_ATTRIBUTES)};
  Results result;
  const auto status = OH_Asset_Query(query, 2, &result.value);
  return status == ASSET_SUCCESS || status == ASSET_NOT_FOUND;
}
}  // namespace nativeapi
