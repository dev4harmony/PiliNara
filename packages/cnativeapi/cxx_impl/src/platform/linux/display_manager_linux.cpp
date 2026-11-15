#include <gtk/gtk.h>
#include <string>
#include <vector>

#include "../../display_manager.h"

namespace nativeapi {

DisplayManager::DisplayManager() {
  gtk_init(nullptr, nullptr);
  // Prime the instance cache so the first change notification diffs against
  // the displays present at startup.
  GetAll();
  // TODO: Connect to GdkDisplay's "monitor-added" / "monitor-removed" signals
  // and call HandleDisplaysChanged() from the handlers.
}

DisplayManager::~DisplayManager() {
  // Destructor implementation
}

bool DisplayManager::EnumerateNativeDisplays(std::vector<NativeDisplayInfo>& natives) {
  natives.clear();
  GdkDisplay* display = gdk_display_get_default();
  if (!display) {
    return false;
  }

  GdkMonitor* primary = gdk_display_get_primary_monitor(display);
  int monitor_count = gdk_display_get_n_monitors(display);
  for (int i = 0; i < monitor_count; ++i) {
    GdkMonitor* monitor = gdk_display_get_monitor(display, i);
    if (!monitor) {
      continue;
    }
    // A GdkMonitor object is stable for as long as the monitor stays
    // connected, so its address serves as the identity key.
    bool is_primary = (primary != nullptr) ? (monitor == primary) : (i == 0);
    natives.push_back(
        {std::to_string(reinterpret_cast<uintptr_t>(monitor)), monitor, is_primary});
  }
  return true;
}

Point DisplayManager::GetCursorPosition() {
  GdkDisplay* display = gdk_display_get_default();
  GdkSeat* seat = gdk_display_get_default_seat(display);
  GdkDevice* pointer = gdk_seat_get_pointer(seat);

  int x, y;
  gdk_device_get_position(pointer, NULL, &x, &y);

  Point point;
  point.x = x;
  point.y = y;
  return point;
}

void DisplayManager::StartEventListening() {}
void DisplayManager::StopEventListening() {}

}  // namespace nativeapi
