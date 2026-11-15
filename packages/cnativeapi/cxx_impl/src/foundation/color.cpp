#include "color.h"
#include <cstddef>

namespace nativeapi {

namespace {
int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
}  // namespace

Color Color::FromHex(const char* hex) {
  if (!hex) return Color::Transparent;
  if (*hex == '#') ++hex;
  size_t length = 0;
  while (length < 9 && hex[length]) ++length;
  if (length != 3 && length != 4 && length != 6 && length != 8)
    return Color::Transparent;
  unsigned char channels[4]{0, 0, 0, 255};
  const bool short_form = length <= 4;
  const size_t count = short_form ? length : length / 2;
  for (size_t i = 0; i < count; ++i) {
    const int high = HexDigit(hex[i * (short_form ? 1 : 2)]);
    const int low = short_form ? high : HexDigit(hex[i * 2 + 1]);
    if (high < 0 || low < 0) return Color::Transparent;
    channels[i] = static_cast<unsigned char>(high * 16 + low);
  }
  return {channels[0], channels[1], channels[2], channels[3]};
}

// Color constants
const Color Color::Transparent = {0, 0, 0, 0};
const Color Color::Black = {0, 0, 0, 255};
const Color Color::White = {255, 255, 255, 255};
const Color Color::Red = {255, 0, 0, 255};
const Color Color::Green = {0, 255, 0, 255};
const Color Color::Blue = {0, 0, 255, 255};
const Color Color::Yellow = {255, 255, 0, 255};
const Color Color::Cyan = {0, 255, 255, 255};
const Color Color::Magenta = {255, 0, 255, 255};

}  // namespace nativeapi