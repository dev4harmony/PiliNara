#pragma once

#include <functional>
#include <memory>
#include <string>

#include "foundation/event_emitter.h"
#include "foundation/keyboard.h"

namespace nativeapi {

/**
 * @brief Observes native keyboard press and release events.
 *
 * Key codes use the platform's native keyboard code space. Register listeners
 * before Start(), and call Stop() when observation is no longer needed.
 *
 * @code
 * KeyboardMonitor monitor;
 * monitor.AddListener<KeyPressedEvent>([](const KeyPressedEvent& event) {
 *   const auto native_key = event.GetKeycode();
 *   (void)native_key;
 * });
 * monitor.Start();
 * @endcode
 */
class KeyboardMonitor : public EventEmitter<KeyboardEvent> {
 public:
  KeyboardMonitor();
  virtual ~KeyboardMonitor();

  /**
   * @brief Starts observing keyboard events; repeated calls are idempotent.
   *
   * Check IsMonitoring() after this call. Permission or service failures leave
   * the monitor inactive. Adding a listener alone does not call Start().
   *
   * @note Platform availability:
   * - macOS: Requires permission for the native session event tap.
   * - Windows: Uses a low-level native keyboard hook.
   * - Linux: Requires an accessible X11 display with XInput2.
   * - Android: Not implemented; remains inactive.
   * - iOS: Not implemented; remains inactive.
   * - OHOS: Requires the initialized UI host and INPUT_MONITORING permission
   *   (restricted system grant, eligible for ACL provisioning). Multiple library
   *   monitors share the process's one public NDK key-monitor slot; unrelated
   *   callers must not register that slot independently. Press/release events,
   *   including modifier key codes, arrive on UI. On the tested HarmonyOS 7
   *   emulator, a system-app debug profile is additionally needed; a normal
   *   app profile with the ACL alone remains denied. ModifierKeysChangedEvent is
   *   not emitted because the NDK event provides no modifier-state snapshot.
   */
  void Start();

  /**
   * @brief Stops keyboard observation for this monitor.
   *
   * On OHOS other library monitors remain active, and queued deliveries from
   * this monitor's previous Start() are invalidated.
   * @see Start() for platform availability.
   */
  void Stop();

  /**
   * @brief Checks whether this monitor has an active native registration.
   * @return False before a successful Start(), after Stop(), or after OHOS host
   * environment cleanup. True does not imply that a listener is installed.
   * @see Start() for platform availability.
   */
  bool IsMonitoring() const;

  // Get access to the event emitter for internal use
  EventEmitter<KeyboardEvent>& GetInternalEventEmitter();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;

  friend class Impl;
};

}  // namespace nativeapi
