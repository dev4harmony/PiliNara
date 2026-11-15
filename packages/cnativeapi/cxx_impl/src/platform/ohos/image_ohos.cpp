#include "../../image.h"
#include "ohos_image.h"

#include <fcntl.h>
#include <multimedia/image_framework/image/image_packer_native.h>
#include <multimedia/image_framework/image/image_source_native.h>
#include <multimedia/image_framework/image/pixelmap_native.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>

namespace nativeapi {
namespace {
template <typename T, auto Release>
using Owned = std::unique_ptr<T, decltype(Release)>;
using Source = Owned<OH_ImageSourceNative, OH_ImageSourceNative_Release>;
using SourceInfo = Owned<OH_ImageSource_Info, OH_ImageSourceInfo_Release>;
using DecodeOptions = Owned<OH_DecodingOptions, OH_DecodingOptions_Release>;
using Packer = Owned<OH_ImagePackerNative, OH_ImagePackerNative_Release>;
using PackOptions = Owned<OH_PackingOptions, OH_PackingOptions_Release>;

PackOptions PackingOptions(const std::string& mime) {
  OH_PackingOptions* raw = nullptr;
  OH_PackingOptions_Create(&raw);
  PackOptions options(raw, OH_PackingOptions_Release);
  Image_MimeType type{const_cast<char*>(mime.data()), mime.size()};
  if (!options || OH_PackingOptions_SetMimeType(options.get(), &type) != IMAGE_SUCCESS ||
      OH_PackingOptions_SetQuality(options.get(), 100) != IMAGE_SUCCESS)
    return {nullptr, OH_PackingOptions_Release};
  return options;
}

const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::vector<uint8_t> Decode(std::string_view input) {
  if (input.substr(0, 5) == "data:") {
    const auto comma = input.find(',');
    if (comma == input.npos || input.substr(0, comma).find(";base64") == input.npos)
      return {};
    input.remove_prefix(comma + 1);
  }
  std::string clean;
  clean.reserve(input.size());
  for (const unsigned char c : input) {
    if (c == ' ' || c == '\r' || c == '\n' || c == '\t')
      continue;
    clean.push_back(c);
  }
  if (clean.empty() || clean.size() % 4 != 0)
    return {};
  std::vector<uint8_t> output;
  output.reserve(clean.size() / 4 * 3);
  for (size_t i = 0; i < clean.size(); i += 4) {
    unsigned bits = 0, padding = 0;
    for (size_t j = 0; j < 4; ++j) {
      const char c = clean[i + j];
      if (c == '=') {
        if (i + 4 != clean.size() || j < 2)
          return {};
        ++padding;
        bits <<= 6;
      } else {
        const auto position = std::string_view(alphabet, 64).find(c);
        if (padding || position == std::string_view::npos)
          return {};
        bits = (bits << 6) | static_cast<unsigned>(position);
      }
    }
    // Reject non-canonical padding instead of silently discarding data bits.
    if ((padding == 1 && (bits & 0xff)) || (padding == 2 && (bits & 0xffff)))
      return {};
    output.push_back(static_cast<uint8_t>(bits >> 16));
    if (padding < 2)
      output.push_back(static_cast<uint8_t>(bits >> 8));
    if (!padding)
      output.push_back(static_cast<uint8_t>(bits));
  }
  return output;
}
std::string Encode(const std::vector<uint8_t>& input) {
  std::string output = "data:image/png;base64,";
  output.reserve(output.size() + (input.size() + 2) / 3 * 4);
  for (size_t i = 0; i < input.size(); i += 3) {
    const unsigned bits = (static_cast<unsigned>(input[i]) << 16) |
                          (i + 1 < input.size() ? static_cast<unsigned>(input[i + 1]) << 8 : 0) |
                          (i + 2 < input.size() ? input[i + 2] : 0);
    output.push_back(alphabet[(bits >> 18) & 63]);
    output.push_back(alphabet[(bits >> 12) & 63]);
    output.push_back(i + 1 < input.size() ? alphabet[(bits >> 6) & 63] : '=');
    output.push_back(i + 2 < input.size() ? alphabet[bits & 63] : '=');
  }
  return output;
}
std::string FileMime(const std::string& path) {
  const auto dot = path.find_last_of('.');
  if (dot == path.npos)
    return {};
  auto extension = path.substr(dot + 1);
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (extension == "jpg" || extension == "jpeg")
    return "image/jpeg";
  if (extension == "heic" || extension == "heif")
    return "image/heic";
  if (extension == "png" || extension == "webp" || extension == "gif" || extension == "bmp")
    return "image/" + extension;
  return {};
}
}  // namespace

class Image::Impl {
 public:
  std::shared_ptr<OH_PixelmapNative> pixels;
  Size size{};
  std::string format;

  bool Load(OH_ImageSourceNative* source) {
    OH_ImageSource_Info* raw_info = nullptr;
    OH_DecodingOptions* raw_options = nullptr;
    OH_ImageSourceInfo_Create(&raw_info);
    OH_DecodingOptions_Create(&raw_options);
    SourceInfo info(raw_info, OH_ImageSourceInfo_Release);
    DecodeOptions options(raw_options, OH_DecodingOptions_Release);
    uint32_t width = 0, height = 0;
    if (!info || !options ||
        OH_ImageSourceNative_GetImageInfo(source, 0, info.get()) != IMAGE_SUCCESS ||
        OH_ImageSourceInfo_GetWidth(info.get(), &width) != IMAGE_SUCCESS ||
        OH_ImageSourceInfo_GetHeight(info.get(), &height) != IMAGE_SUCCESS || !width || !height)
      return false;
    OH_DecodingOptions_SetPixelFormat(options.get(), PIXEL_FORMAT_RGBA_8888);
    OH_PixelmapNative* raw_pixels = nullptr;
    if (OH_ImageSourceNative_CreatePixelmap(source, options.get(), &raw_pixels) != IMAGE_SUCCESS ||
        !raw_pixels)
      return false;
    pixels = std::shared_ptr<OH_PixelmapNative>(
        raw_pixels, [](OH_PixelmapNative* map) { OH_PixelmapNative_Destroy(&map); });
    size = {static_cast<double>(width), static_cast<double>(height)};
    Image_MimeType mime{};
    if (OH_ImageSourceInfo_GetMimeType(info.get(), &mime) == IMAGE_SUCCESS && mime.data) {
      const std::string name(mime.data, mime.size);
      // Borrowed from SourceInfo; its release also releases the MIME buffer.
      format = name.substr(name.find('/') + 1);
      if (format == "jpg")
        format = "jpeg";
      std::transform(format.begin(), format.end(), format.begin(),
                     [](unsigned char c) { return std::toupper(c); });
    }
    return true;
  }
};
Image::Image() : pimpl_(std::make_unique<Impl>()) {}
Image::~Image() = default;
Image::Image(const Image& other)
    : pimpl_(other.pimpl_ ? std::make_unique<Impl>(*other.pimpl_) : nullptr) {}
Image::Image(Image&& other) noexcept : pimpl_(std::move(other.pimpl_)) {}
std::shared_ptr<Image> Image::FromFile(const std::string& path) {
  if (path.empty() || path.find('\0') != path.npos)
    return nullptr;
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return nullptr;
  OH_ImageSourceNative* raw = nullptr;
  OH_ImageSourceNative_CreateFromFd(fd, &raw);
  Source source(raw, OH_ImageSourceNative_Release);
  auto image = std::shared_ptr<Image>(new Image());
  const bool loaded = source && image->pimpl_->Load(source.get());
  source.reset();
  close(fd);
  return loaded ? image : nullptr;
}
std::shared_ptr<Image> Image::FromBase64(const std::string& encoded) {
  auto bytes = Decode(encoded);
  if (bytes.empty())
    return nullptr;
  OH_ImageSourceNative* raw = nullptr;
  OH_ImageSourceNative_CreateFromData(bytes.data(), bytes.size(), &raw);
  Source source(raw, OH_ImageSourceNative_Release);
  auto image = std::shared_ptr<Image>(new Image());
  return source && image->pimpl_->Load(source.get()) ? image : nullptr;
}
Size Image::GetSize() const {
  return pimpl_ ? pimpl_->size : Size{};
}
std::string Image::GetFormat() const {
  return pimpl_ ? pimpl_->format : "";
}
std::string ohos::EncodePixelmapPng(OH_PixelmapNative* pixels) {
  if (!pixels)
    return {};
  OH_Pixelmap_ImageInfo* raw_info = nullptr;
  OH_PixelmapImageInfo_Create(&raw_info);
  Owned<OH_Pixelmap_ImageInfo, OH_PixelmapImageInfo_Release> info(raw_info,
                                                                  OH_PixelmapImageInfo_Release);
  uint32_t width = 0, height = 0;
  if (!info || OH_PixelmapNative_GetImageInfo(pixels, info.get()) != IMAGE_SUCCESS ||
      OH_PixelmapImageInfo_GetWidth(info.get(), &width) != IMAGE_SUCCESS ||
      OH_PixelmapImageInfo_GetHeight(info.get(), &height) != IMAGE_SUCCESS || !width || !height)
    return {};
  auto options = PackingOptions("image/png");
  OH_ImagePackerNative* raw = nullptr;
  OH_ImagePackerNative_Create(&raw);
  Packer packer(raw, OH_ImagePackerNative_Release);
  if (!options || !packer)
    return {};
  // Covers 8-byte HDR PixelMaps too, plus PNG filters/deflate chunk overhead.
  const uint64_t count = static_cast<uint64_t>(width) * height;
  if (count > (std::numeric_limits<size_t>::max() - 65536 - height) / 9)
    return {};
  const uint64_t bytes = count * 8;
  const uint64_t capacity = bytes + bytes / 10 + height + 65536;
  if (capacity > std::numeric_limits<size_t>::max())
    return {};
  std::vector<uint8_t> output(static_cast<size_t>(capacity));
  size_t size = output.size();
  if (OH_ImagePackerNative_PackToDataFromPixelmap(packer.get(), options.get(), pixels,
                                                  output.data(), &size) != IMAGE_SUCCESS ||
      size > output.size())
    return {};
  output.resize(size);
  return Encode(output);
}
std::string Image::ToBase64() const {
  return ohos::EncodePixelmapPng(pimpl_ ? pimpl_->pixels.get() : nullptr);
}

bool Image::SaveToFile(const std::string& path) const {
  if (!pimpl_ || !pimpl_->pixels || path.find('\0') != path.npos)
    return false;
  const auto mime = FileMime(path);
  if (mime.empty())
    return false;
  auto options = PackingOptions(mime);
  OH_ImagePackerNative* raw = nullptr;
  OH_ImagePackerNative_Create(&raw);
  Packer packer(raw, OH_ImagePackerNative_Release);
  if (!options || !packer)
    return false;
  const int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
  if (fd < 0)
    return false;
  const bool saved = OH_ImagePackerNative_PackToFileFromPixelmap(
                         packer.get(), options.get(), pimpl_->pixels.get(), fd) == IMAGE_SUCCESS;
  const bool closed = close(fd) == 0;
  return saved && closed;
}
void* Image::GetNativeObjectInternal() const {
  return pimpl_ ? pimpl_->pixels.get() : nullptr;
}
}  // namespace nativeapi
