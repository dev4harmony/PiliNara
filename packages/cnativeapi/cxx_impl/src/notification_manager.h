#pragma once
#include <memory>
#include <string>
#include "foundation/event_emitter.h"

namespace nativeapi {
class NotificationEvent : public Event {
 public:
  std::string GetTypeName() const override { return "NotificationEvent"; }
};
class NotificationActivatedEvent : public NotificationEvent {
 public:
  explicit NotificationActivatedEvent(const std::string& argument) : argument_(argument) {}
  const std::string& GetArgument() const { return argument_; }
  std::string GetTypeName() const override { return "NotificationActivatedEvent"; }
 private:
  std::string argument_;
};

/** System notifications. Subscribe before Initialize(), including cold launches.
 * Activation events are delivered on the main dispatcher. Capability does not
 * imply that notification settings allow delivery or that banners will appear.
 *
 * @note Platform availability:
 * - macOS: ❌ Not implemented.
 * - Windows: ✅ Windows App SDK with NATIVEAPI_ENABLE_WINUI3; main STA calls.
 * - Linux: ❌ Not implemented.
 * - Android: ❌ Not implemented in this native layer.
 * - iOS: ❌ Not implemented.
 * - OpenHarmony: ✅ Public notificationManager and WantAgent APIs after host
 *   initialization/context attachment. Forward onCreate/onNewWant through the
 *   private OHOS bridge. Initialize/Show/Remove require a worker and the running
 *   ArkTS UI loop. Initialize requests user authorization when necessary.
 */
class NotificationManager : public EventEmitter<NotificationEvent> {
 public:
  static NotificationManager& GetInstance();
  ~NotificationManager();
  NotificationManager(const NotificationManager&) = delete;
  NotificationManager& operator=(const NotificationManager&) = delete;
  NotificationManager(NotificationManager&&) = delete;
  NotificationManager& operator=(NotificationManager&&) = delete;
  static bool IsSupported();
  /** Registers activation delivery. Call at application startup, after subscribing.
   * OpenHarmony waits for actual authorization completion; denial returns false.
   */
  bool Initialize();
  /** Release registration before stopping the UI loop. Delivered notifications remain. */
  void Shutdown();
  /** Uses tag to replace an existing notification. Activation argument is
   * tag=<encoded tag>&action=open or action=button. The optional button uses the
   * latter. OpenHarmony tags contain 1-192 UTF-8 bytes without NUL; title/message
   * must be nonempty. SDK display-length limits apply. True means accepted by
   * the OS; notification settings may suppress the banner.
   */
  bool Show(const std::string& title, const std::string& message,
            const std::string& tag, const std::string& button_label);
  bool Remove(const std::string& tag);
  std::string GetLastError() const;
 private:
  NotificationManager();
  class Impl;
  std::unique_ptr<Impl> pimpl_;
};
}
