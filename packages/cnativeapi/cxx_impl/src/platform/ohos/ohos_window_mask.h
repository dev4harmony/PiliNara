#pragma once

// Private, platform-independent rasterizer for the SDK's physical-pixel mask.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "../../window_shape.h"

namespace nativeapi::ohos {
inline bool RasterizeWindowMask(const WindowShape& shape,
                                uint32_t width,
                                uint32_t height,
                                double scale,
                                std::vector<uint8_t>& mask) {
  constexpr uint64_t kMaxPixels = 16 * 1024 * 1024;
  if (shape.GetPointCount() < 3 || shape.GetPointCount() > 4096 || !width || !height ||
      width > 16384 || height > 16384 || uint64_t(width) * height > kMaxPixels ||
      !std::isfinite(scale) || scale <= 0 || scale > 64)
    return false;
  std::vector<Point> points;
  points.reserve(shape.GetPointCount());
  for (size_t i = 0; i < shape.GetPointCount(); ++i) {
    auto p = shape.GetPointAt(i);
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || p.x < 0 || p.y < 0 || p.x > 16384 ||
        p.y > 16384)
      return false;
    points.push_back({p.x * scale, p.y * scale});
  }
  mask.assign(size_t(width) * height, 0);
  std::vector<double> crossings;
  crossings.reserve(points.size());
  for (uint32_t y = 0; y < height; ++y) {
    crossings.clear();
    const double center = y + 0.5;
    auto previous = points.back();
    for (auto current : points) {
      // Half-open edges count a shared vertex once; horizontal edges contribute nothing.
      if ((previous.y > center) != (current.y > center))
        crossings.push_back(previous.x + (center - previous.y) * (current.x - previous.x) /
                                             (current.y - previous.y));
      previous = current;
    }
    std::sort(crossings.begin(), crossings.end());
    for (size_t i = 1; i < crossings.size(); i += 2) {
      auto clamp = [width](double x) {
        return static_cast<uint32_t>(std::max(0.0, std::min(double(width), std::ceil(x - 0.5))));
      };
      const auto left = clamp(crossings[i - 1]), right = clamp(crossings[i]);
      std::fill(mask.begin() + size_t(y) * width + left, mask.begin() + size_t(y) * width + right,
                255);
    }
  }
  return true;
}
}  // namespace nativeapi::ohos
