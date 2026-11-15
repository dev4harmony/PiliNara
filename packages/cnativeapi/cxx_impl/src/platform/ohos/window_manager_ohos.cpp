#include "../../window_close_dispatch.h"
#include "../../window_manager.h"
#include "../../window_registry.h"
#include "ohos_runtime.h"
#include "ohos_window.h"
#include <cmath>

namespace nativeapi {
class WindowManager::Impl {};
WindowManager::WindowManager() : pimpl_(std::make_unique<Impl>()) {}
WindowManager::~WindowManager() {
  ShutdownEmitter();
  StopEventListening();
}
std::shared_ptr<Window> WindowManager::Get(WindowId id) {
  auto window = WindowRegistry::GetInstance().Get(id);
  return window && window->GetNativeObject() ? window : nullptr;
}
std::vector<std::shared_ptr<Window>> WindowManager::GetAll() {
  std::vector<std::shared_ptr<Window>> result;
  for (const auto& state : ohos::WindowStates())
    if (auto window = Get(state->id))
      result.push_back(window);
  return result;
}
std::shared_ptr<Window> WindowManager::GetCurrent() {
  for (auto& window : GetAll())
    if (window->IsFocused())
      return window;
  for (auto& state : ohos::WindowStates())
    if (state->primary)
      return Get(state->id);
  return nullptr;
}
std::shared_ptr<Window> WindowManager::GetWindowAtPoint(Point point, WindowId excluded_window_id) try {
  if (!std::isfinite(point.x) || !std::isfinite(point.y)) return nullptr;
  // The host registers only this application's windows. Never claim to hit-test other apps.
  for (const auto& state : ohos::HitTestWindowStates()) {
    auto window = Get(state->id);
    if (!window) continue;
    if (window->GetId() == excluded_window_id || !window->IsVisible())
      continue;
    const auto bounds = window->GetBounds();
    if (point.x >= bounds.x && point.y >= bounds.y && point.x < bounds.x + bounds.width &&
        point.y < bounds.y + bounds.height)
      return window;
  }
  return nullptr;
} catch (...) {
  return nullptr;
}
void WindowManager::SetWillShowHook(std::optional<WindowWillShowHook>) {}
void WindowManager::SetWillHideHook(std::optional<WindowWillHideHook>) {}
bool WindowManager::HasWillShowHook() const {
  return false;
}
bool WindowManager::HasWillHideHook() const {
  return false;
}
void WindowManager::HandleWillShow(WindowId) {}
void WindowManager::HandleWillHide(WindowId) {}
bool WindowManager::CallOriginalShow(WindowId) {
  return false;
}
bool WindowManager::CallOriginalHide(WindowId) {
  return false;
}
void WindowManager::StartEventListening() {
  ohos::SetWindowEventSink(
      CreateGuardedCallback<const WindowEvent&>(std::function<void(const WindowEvent&)>(
          [this](const WindowEvent& event) { DispatchWindowEvent(event); })));
}
void WindowManager::StopEventListening() {
  ohos::SetWindowEventSink({});
}
void WindowManager::DispatchWindowEvent(const WindowEvent& event) {
  detail::EmitObservedWindowEvent(event);
  Emit(event);
}
}  // namespace nativeapi
