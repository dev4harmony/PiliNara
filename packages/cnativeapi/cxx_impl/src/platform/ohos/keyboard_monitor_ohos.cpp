#include "../../keyboard_monitor.h"
#include "ohos_runtime.h"

#include <hilog/log.h>
#include <multimodalinput/oh_input_manager.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace nativeapi {
namespace {
struct Registration {
  std::atomic<bool> active{false};
  uint64_t environment = 0;
  std::function<void(int32_t, int32_t)> deliver;
};
struct Monitors {
  // SDK operations can wait for an input callback. They must never hold members.
  std::mutex operations;
  std::mutex members;
  bool installed = false;
  std::unordered_map<Registration*, std::shared_ptr<Registration>> entries;
};
Monitors& Registry() {
  // SDK callbacks may follow static teardown; no emitter pointer lives here.
  static auto* registry = new Monitors;
  return *registry;
}
void KeyEvent(const Input_KeyEvent* event) {
  if (!event) return;
  const auto action = OH_Input_GetKeyEventAction(event);
  if (action != KEY_ACTION_DOWN && action != KEY_ACTION_UP) return;
  const auto key = OH_Input_GetKeyEventKeyCode(event);
  auto& registry = Registry();
  std::lock_guard<std::mutex> lock(registry.members);
  std::vector<std::shared_ptr<Registration>> snapshot;
  for (const auto& item : registry.entries) snapshot.push_back(item.second);
  // Copy scalars from the SDK's borrowed event, preserving native event order.
  RunOnMainThread([snapshot = std::move(snapshot), action, key] {
    const auto environment = ohos::EnvironmentGeneration();
    for (const auto& entry : snapshot) {
      if (entry->active.load() && entry->environment == environment)
        entry->deliver(action, key);
    }
  });
}
void LogResult(const char* operation, Input_Result result) {
  OH_LOG_Print(LOG_APP, result == INPUT_SUCCESS ? LOG_INFO : LOG_WARN, 1,
               "nativeapi-keyboard", "SDK monitor %{public}s=%{public}d", operation, int(result));
}
}  // namespace
class KeyboardMonitor::Impl {
 public:
  explicit Impl(KeyboardMonitor* monitor) : monitor_(monitor) {}
  void Start() {
    auto& registry = Registry();
    std::lock_guard<std::mutex> operation(registry.operations);
    const auto environment = ohos::EnvironmentGeneration();
    if (!environment || !IsMainThreadDispatchSupported()) return;
    if (entry_ && entry_->active.load() && entry_->environment == environment) return;
    if (entry_) {
      entry_->active.store(false);
      std::lock_guard<std::mutex> lock(registry.members);
      registry.entries.erase(entry_.get());
    }
    entry_.reset();
    if (!registry.installed) {
      const auto result = OH_Input_AddKeyEventMonitor(KeyEvent);
      LogResult("start", result);
      if (result != INPUT_SUCCESS) return;
      registry.installed = true;
    }
    auto entry = std::make_shared<Registration>();
    entry->environment = environment;
    const std::weak_ptr<Registration> weak = entry;
    entry->deliver = monitor_->CreateGuardedCallback<int32_t, int32_t>(
        std::function<void(int32_t, int32_t)>([monitor = monitor_, weak](int32_t action, int32_t key) {
          const auto entry = weak.lock();
          if (!entry || !entry->active.load() ||
              entry->environment != ohos::EnvironmentGeneration()) return;
          // A listener may destroy the monitor. Do not access it after Emit.
          if (action == KEY_ACTION_DOWN) monitor->Emit<KeyPressedEvent>(key);
          else monitor->Emit<KeyReleasedEvent>(key);
        }));
    {
      std::lock_guard<std::mutex> lock(registry.members);
      entry->active.store(true);
      registry.entries.emplace(entry.get(), entry);
    }
    entry_ = std::move(entry);
  }
  void Stop() {
    auto& registry = Registry();
    std::lock_guard<std::mutex> operation(registry.operations);
    bool empty;
    {
      std::lock_guard<std::mutex> lock(registry.members);
      if (entry_) {
        entry_->active.store(false);
        registry.entries.erase(entry_.get());
        entry_.reset();
      }
      empty = registry.entries.empty();
    }
    if (empty && registry.installed) {
      const auto result = OH_Input_RemoveKeyEventMonitor(KeyEvent);
      LogResult("stop", result);
      // Keep cleanup ownership if removal failed; the next Stop retries.
      if (result == INPUT_SUCCESS) registry.installed = false;
    }
  }
  bool IsMonitoring() const {
    std::lock_guard<std::mutex> operation(Registry().operations);
    return entry_ && entry_->active.load() &&
           entry_->environment == ohos::EnvironmentGeneration();
  }
 private:
  KeyboardMonitor* monitor_;
  std::shared_ptr<Registration> entry_;
};
KeyboardMonitor::KeyboardMonitor() : impl_(std::make_unique<Impl>(this)) {}
KeyboardMonitor::~KeyboardMonitor() { ShutdownEmitter(); Stop(); }
void KeyboardMonitor::Start() { impl_->Start(); }
void KeyboardMonitor::Stop() { impl_->Stop(); }
bool KeyboardMonitor::IsMonitoring() const { return impl_->IsMonitoring(); }
EventEmitter<KeyboardEvent>& KeyboardMonitor::GetInternalEventEmitter() { return *this; }
}  // namespace nativeapi
