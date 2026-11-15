#pragma once

#include <memory>
#include "storage.h"

namespace nativeapi {

/**
 * @brief Secure storage for sensitive data like passwords and tokens.
 *
 * Similar to Web Storage's encrypted storage, this provides secure persistent
 * storage for sensitive application data. Data is encrypted at rest.
 *
 * @note Platform availability:
 * - macOS: ❌ Not implemented
 * - Windows: ❌ Not implemented
 * - Linux: ❌ Not implemented
 * - Android: ❌ Not implemented
 * - iOS: ❌ Not implemented
 * - OpenHarmony: ✅ Public ASSET encrypted storage, isolated by application and scope.
 *   Available after the device's first unlock. Assets are removed on app uninstall.
 *   Values are at most 1023 bytes; an internal format byte encodes empty strings.
 *   The namespaced scope/key alias must fit the native 256-byte limit. Scope/key
 *   embedded NUL is rejected. Scope/key metadata is not encrypted; values are.
 *   Invalid inputs fail before replacing existing assets. SDK failures return
 *   false, defaults or empty collections; plaintext storage is never used.
 */
class SecureStorage : public Storage {
 public:
  /**
   * @brief Create secure storage with default scope.
   *
   * Uses application name as scope identifier if available.
   */
  SecureStorage();

  /**
   * @brief Create secure storage with custom scope.
   *
   * @param scope Scope/application identifier for keychain/credential manager
   */
  explicit SecureStorage(const std::string& scope);

  virtual ~SecureStorage();

  // Storage interface implementation
  bool Set(const std::string& key, const std::string& value) override;
  std::string Get(const std::string& key, const std::string& default_value = "") const override;
  bool Remove(const std::string& key) override;
  bool Clear() override;
  bool Contains(const std::string& key) const override;
  std::vector<std::string> GetKeys() const override;
  size_t GetSize() const override;
  std::map<std::string, std::string> GetAll() const override;

  /**
   * @brief Get the scope.
   *
   * @return The scope identifier used for this secure storage instance
   */
  std::string GetScope() const;

  /**
   * @brief Check if secure storage is available on this platform.
   *
   * @return true if platform supports secure storage, false otherwise
   */
  static bool IsAvailable();

  // Prevent copying and moving
  SecureStorage(const SecureStorage&) = delete;
  SecureStorage& operator=(const SecureStorage&) = delete;
  SecureStorage(SecureStorage&&) = delete;
  SecureStorage& operator=(SecureStorage&&) = delete;

 private:
  class Impl;
  std::unique_ptr<Impl> pimpl_;
};

}  // namespace nativeapi
