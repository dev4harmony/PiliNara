#pragma once
#include <multimedia/image_framework/image/pixelmap_native.h>
#include <string>
namespace nativeapi::ohos {
// Encodes actual native dimensions; callers retain the PixelMap during this call.
std::string EncodePixelmapPng(OH_PixelmapNative* pixels);
}  // namespace nativeapi::ohos
