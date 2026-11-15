#include <fstream>
#include <iterator>
#include "../../clipboard_impl.h"
#include "../../image.h"
#include "android_runtime.h"

namespace nativeapi {
class Image::Impl {
 public:
  jobject bitmap = nullptr;
  Size size{0, 0};
  ~Impl() {
    android::Environment scope;
    if (scope && bitmap)
      scope.get()->DeleteGlobalRef(bitmap);
  }
};
Image::Image() : pimpl_(std::make_unique<Impl>()) {}
Image::~Image() = default;
Image::Image(const Image& other) : Image() {
  android::Environment scope;
  if (scope && other.pimpl_ && other.pimpl_->bitmap) {
    pimpl_->bitmap = scope.get()->NewGlobalRef(other.pimpl_->bitmap);
    android::ClearException(scope.get());
    pimpl_->size = other.pimpl_->size;
  }
}
Image::Image(Image&& other) noexcept : pimpl_(std::move(other.pimpl_)) {}
std::shared_ptr<Image> Image::FromFile(const std::string& path) {
  std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
  if (!file)
    return nullptr;
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
  if (file.bad())
    return nullptr;
  return FromBase64(clipboard_internal::Base64(bytes.data(), bytes.size()));
}
std::shared_ptr<Image> Image::FromBase64(const std::string& base64) {
  auto bytes = clipboard_internal::DecodeBase64(base64);
  if (bytes.empty())
    return nullptr;
  android::Environment scope;
  if (!scope)
    return nullptr;
  auto* env = scope.get();
  auto factory = env->FindClass("android/graphics/BitmapFactory");
  auto method = factory ? env->GetStaticMethodID(factory, "decodeByteArray",
                                                 "([BII)Landroid/graphics/Bitmap;")
                        : nullptr;
  if (android::ClearException(env))
    return nullptr;
  auto data = android::ToJavaBytes(env, bytes);
  if (!method || !data)
    return nullptr;
  auto bitmap =
      env->CallStaticObjectMethod(factory, method, data, 0, static_cast<jint>(bytes.size()));
  if (android::ClearException(env) || !bitmap)
    return nullptr;
  auto cls = env->GetObjectClass(bitmap);
  auto width = env->GetMethodID(cls, "getWidth", "()I");
  auto height = env->GetMethodID(cls, "getHeight", "()I");
  if (android::ClearException(env) || !width || !height)
    return nullptr;
  auto image = std::shared_ptr<Image>(new Image());
  image->pimpl_->size = {static_cast<double>(env->CallIntMethod(bitmap, width)),
                         static_cast<double>(env->CallIntMethod(bitmap, height))};
  image->pimpl_->bitmap = env->NewGlobalRef(bitmap);
  if (android::ClearException(env) || !image->pimpl_->bitmap)
    return nullptr;
  return image;
}
Size Image::GetSize() const {
  return pimpl_ ? pimpl_->size : Size{0, 0};
}
std::string Image::GetFormat() const {
  return GetNativeObjectInternal() ? "PNG" : "";
}
std::string Image::ToBase64() const {
  android::Environment scope;
  if (!scope || !GetNativeObjectInternal())
    return "";
  auto* env = scope.get();
  auto stream_class = env->FindClass("java/io/ByteArrayOutputStream");
  auto format_class = env->FindClass("android/graphics/Bitmap$CompressFormat");
  auto bitmap_class = env->GetObjectClass(pimpl_->bitmap);
  if (android::ClearException(env) || !stream_class || !format_class || !bitmap_class)
    return "";
  auto ctor = env->GetMethodID(stream_class, "<init>", "()V");
  auto field =
      env->GetStaticFieldID(format_class, "PNG", "Landroid/graphics/Bitmap$CompressFormat;");
  auto compress =
      env->GetMethodID(bitmap_class, "compress",
                       "(Landroid/graphics/Bitmap$CompressFormat;ILjava/io/OutputStream;)Z");
  auto extract = env->GetMethodID(stream_class, "toByteArray", "()[B");
  if (android::ClearException(env) || !ctor || !field || !compress || !extract)
    return "";
  auto stream = env->NewObject(stream_class, ctor);
  auto format = env->GetStaticObjectField(format_class, field);
  if (android::ClearException(env) || !stream || !format)
    return "";
  auto ok = env->CallBooleanMethod(pimpl_->bitmap, compress, format, 100, stream);
  if (android::ClearException(env) || !ok)
    return "";
  auto data = static_cast<jbyteArray>(env->CallObjectMethod(stream, extract));
  if (android::ClearException(env) || !data)
    return "";
  auto bytes = android::FromJavaBytes(env, data);
  return bytes.empty()
             ? ""
             : "data:image/png;base64," + clipboard_internal::Base64(bytes.data(), bytes.size());
}
bool Image::SaveToFile(const std::string& path) const {
  auto bytes = clipboard_internal::DecodeBase64(ToBase64());
  if (bytes.empty())
    return false;
  std::ofstream output(std::filesystem::u8path(path), std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return output.good();
}
void* Image::GetNativeObjectInternal() const {
  return pimpl_ ? pimpl_->bitmap : nullptr;
}
}  // namespace nativeapi
