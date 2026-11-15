#include <android/log.h>
#include "../../display_manager.h"

#define LOG_TAG "NativeApi"
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace nativeapi {

DisplayManager::DisplayManager() {}
DisplayManager::~DisplayManager() {}

bool DisplayManager::EnumerateNativeDisplays(std::vector<NativeDisplayInfo>& natives) {
  // Stub: a single default display.
  natives = {{"android_display_0", nullptr, true}};
  return true;
}

Point DisplayManager::GetCursorPosition() {
  return Point{0, 0};
}

void DisplayManager::StartEventListening() {}
void DisplayManager::StopEventListening() {}

}  // namespace nativeapi
