#pragma once

// NativeActivity host seam; no managed companion or Flutter plugin.
#include <android/native_activity.h>
#include <jni.h>
#include <functional>
#include <string>
#include <vector>

namespace nativeapi::android {
// Call from ANativeActivity_onCreate on the activity/UI thread. The host owns
// its Looper. Existing focus/destroy callbacks are forwarded; install them first.
bool Initialize(ANativeActivity* activity);
bool IsInitialized();
bool IsActive();
bool Post(std::function<void()> work);
bool RunSync(std::function<void()> work);
bool SetClipboardPoll(std::function<void()> callback);
jobject Context();

class Environment {
 public:
  Environment();
  ~Environment();
  Environment(const Environment&) = delete;
  Environment& operator=(const Environment&) = delete;
  JNIEnv* get() const { return env_; }
  explicit operator bool() const { return env_ != nullptr; }

 private:
  JNIEnv* env_ = nullptr;
  bool attached_ = false;
  bool frame_ = false;
};
bool ClearException(JNIEnv* env);
jstring ToJavaString(JNIEnv* env, const std::string& value);
std::string FromJavaString(JNIEnv* env, jstring value);
jbyteArray ToJavaBytes(JNIEnv* env, const std::vector<unsigned char>& value);
std::vector<unsigned char> FromJavaBytes(JNIEnv* env, jbyteArray value);
}  // namespace nativeapi::android
