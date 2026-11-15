#pragma once

#include <memory>
#include <string>

namespace nativeapi {

enum class UrlOpenErrorCode {
  kNone = 0,
  kInvalidUrlEmpty,
  kInvalidUrlMissingScheme,
  kInvalidUrlUnsupportedScheme,
  kUnsupportedPlatform,
  kInvocationFailed,
};

struct UrlOpenResult {
  bool success = false;
  UrlOpenErrorCode error_code = UrlOpenErrorCode::kNone;
  std::string error_message;
};

/** Opens HTTP/HTTPS links with the system's registered handler.
 *
 * @note Platform availability:
 * - macOS: ✅ NSWorkspace URL dispatch.
 * - Windows: ✅ ShellExecute URL dispatch.
 * - Linux: ✅ GIO desktop URI dispatch.
 * - Android: ❌ Not implemented in this native layer.
 * - iOS: ✅ UIApplication URL dispatch.
 * - OpenHarmony: ✅ Public UIAbilityContext dispatch after host/context attachment.
 */
class UrlOpener {
 public:
  static UrlOpener& GetInstance();

  bool IsSupported() const;
  /** Checks the link's scheme and platform handler. OpenHarmony queries the
   * public bundle manager; declare http/https in the host module's querySchemes.
   * A missing declaration, invalid link or unavailable handler returns false.
   */
  bool CanOpen(const std::string& url) const;
  /** Requests system URL dispatch. OpenHarmony requires a worker while the
   * ArkTS UI thread processes the actual SDK completion callback. UI-thread calls
   * fail explicitly. SDK foreground/permission rules apply; launch failure is
   * returned as kInvocationFailed with its SDK diagnostic. Success acknowledges
   * system dispatch, not network loading of the destination page.
   */
  UrlOpenResult Open(const std::string& url) const;

  UrlOpener(const UrlOpener&) = delete;
  UrlOpener& operator=(const UrlOpener&) = delete;
  UrlOpener(UrlOpener&&) = delete;
  UrlOpener& operator=(UrlOpener&&) = delete;

  class Impl {
   public:
    virtual ~Impl() = default;

    virtual bool IsSupported() const = 0;
    virtual bool CanOpen(const std::string& url) const;
    virtual UrlOpenResult Open(const std::string& url) const = 0;
  };

 private:
  UrlOpener();
  ~UrlOpener();

  std::unique_ptr<Impl> pimpl_;
};

}  // namespace nativeapi
