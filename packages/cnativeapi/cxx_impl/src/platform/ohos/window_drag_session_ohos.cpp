#include "../../window_drag_session.h"

namespace nativeapi {

// Ordinary apps cannot establish global primary-button tracking: the public
// mouse monitor requires INPUT_MONITORING and a screen-recording scenario.
// A local component gesture is not a replacement for a session that continues
// across windows. Native Window::StartDragging() is a separate SDK operation.
class WindowDragSession::Impl {};

WindowDragSession::WindowDragSession() : pimpl_(std::make_unique<Impl>()) {}

WindowDragSession::~WindowDragSession() = default;

void WindowDragSession::StartTicking() {}

void WindowDragSession::StopTicking() {}

bool WindowDragSession::QueryPointer(Point& /*position*/, bool& /*primary_button_down*/) const {
  return false;
}

void WindowDragSession::MoveWindow(Window& window, Point cursor_position) const {
  window.SetPosition({cursor_position.x - anchor_.x, cursor_position.y - anchor_.y});
}

}  // namespace nativeapi
