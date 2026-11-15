#pragma once
// SDK-independent pixel work. Seed the blur from the *clipped* native mask, so
// polygon vertices outside the window cannot cast a shadow from invisible area.
#include "../window_shadow_blur.h"
#include "ohos_window_mask.h"
namespace nativeapi::ohos {
struct ShadowRaster {
  int width = 0, height = 0, margin = 0;
  std::vector<uint8_t> rgba;
};
inline ShadowRaster RasterizeWindowShadow(int width,
                                          int height,
                                          double scale,
                                          const WindowShape& polygon,
                                          const WindowShadow& options) {
  ShadowRaster output;
  if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || !std::isfinite(scale) ||
      scale <= 0 || scale > 8)
    return output;
  const int margin = window_shadow::Margin(options, scale), wide = width + 2 * margin,
            high = height + 2 * margin;
  if (int64_t(wide) * high > 16000000)
    return output;
  std::vector<uint8_t> mask;
  if (polygon.GetPointCount()) {
    if (!RasterizeWindowMask(polygon, width, height, scale, mask))
      return output;
  } else
    mask.assign(size_t(width) * height, 255);
  std::vector<uint8_t> alpha(size_t(wide) * high, 0);
  const auto offset = options.GetOffset();
  const int dx = static_cast<int>(std::lround(offset.x * scale)),
            dy = static_cast<int>(std::lround(offset.y * scale));
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
      alpha[size_t(y + margin + dy) * wide + x + margin + dx] = mask[size_t(y) * width + x];
  window_shadow::Blur(alpha, wide, high, window_shadow::Radius(options, scale));
  // The helper may sit above a main window; its actual visible content must be
  // fully transparent. Holes in an explicit shape retain their exterior shadow.
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
      if (mask[size_t(y) * width + x])
        alpha[size_t(y + margin) * wide + x + margin] = 0;
  output.width = wide;
  output.height = high;
  output.margin = margin;
  output.rgba.resize(alpha.size() * 4);
  const auto color = options.GetColor();
  for (size_t i = 0; i < alpha.size(); ++i) {
    // Supply straight channels to the public NDK PixelMap creation path.
    output.rgba[i * 4] = color.r;
    output.rgba[i * 4 + 1] = color.g;
    output.rgba[i * 4 + 2] = color.b;
    output.rgba[i * 4 + 3] = (static_cast<unsigned>(alpha[i]) * color.a + 127) / 255;
  }
  return output;
}
}  // namespace nativeapi::ohos
