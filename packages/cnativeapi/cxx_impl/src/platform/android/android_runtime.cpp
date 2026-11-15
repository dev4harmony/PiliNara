#include "android_runtime.h"
#include "../../foundation/dispatcher.h"

#include <android/log.h>
#include <android/looper.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <deque>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace nativeapi::android {
namespace {
JavaVM* vm = nullptr;
jobject context = nullptr;
ALooper* looper = nullptr;
int wake_fd = -1;
int timer_fd = -1;
std::atomic<bool> initialized{false};
std::mutex queue_mutex;
std::deque<std::function<void()>> tasks;
std::function<void()> clipboard_poll;
bool active = false;
void (*previous_focus)(ANativeActivity*, int) = nullptr;
void (*previous_destroy)(ANativeActivity*) = nullptr;

int DrainTasks(int fd, int, void*) {
  uint64_t count;
  while (read(fd, &count, sizeof(count)) > 0) {
  }
  std::deque<std::function<void()>> ready;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    ready.swap(tasks);
  }
  while (!ready.empty()) {
    try {
      ready.front()();
    } catch (...) {
      __android_log_print(ANDROID_LOG_ERROR, "nativeapi", "Android UI task failed");
    }
    ready.pop_front();
  }
  return 1;
}
int PollClipboard(int fd, int, void*) {
  uint64_t count;
  auto poll = clipboard_poll;
  if (read(fd, &count, sizeof(count)) > 0 && active && poll)
    poll();
  return 1;
}
void FocusChanged(ANativeActivity* activity, int focused) {
  active = focused != 0;
  SetClipboardPoll(clipboard_poll);
  auto poll = clipboard_poll;
  if (active && poll)
    poll();
  if (previous_focus)
    previous_focus(activity, focused);
}
void Destroyed(ANativeActivity* activity) {
  active = false;
  SetClipboardPoll(clipboard_poll);
  if (previous_destroy)
    previous_destroy(activity);
}
}  // namespace

bool Initialize(ANativeActivity* activity) {
  if (IsInitialized()) {
    if (!activity || !activity->callbacks || ALooper_forThread() != looper)
      return false;
    active = false;
    if (activity->callbacks->onWindowFocusChanged != FocusChanged) {
      previous_focus = activity->callbacks->onWindowFocusChanged;
      activity->callbacks->onWindowFocusChanged = FocusChanged;
    }
    if (activity->callbacks->onDestroy != Destroyed) {
      previous_destroy = activity->callbacks->onDestroy;
      activity->callbacks->onDestroy = Destroyed;
    }
    return true;
  }
  if (!activity || !activity->env || !activity->clazz || !activity->callbacks)
    return false;
  auto* env = activity->env;
  auto* current_looper = ALooper_forThread();
  if (!current_looper)
    return false;
  auto cls = env->GetObjectClass(activity->clazz);
  auto method = env->GetMethodID(cls, "getApplicationContext", "()Landroid/content/Context;");
  auto app = method ? env->CallObjectMethod(activity->clazz, method) : nullptr;
  if (ClearException(env) || !app || env->GetJavaVM(&vm) != JNI_OK)
    return false;
  auto retained = env->NewGlobalRef(app);
  env->DeleteLocalRef(app);
  env->DeleteLocalRef(cls);
  if (ClearException(env) || !retained)
    return false;
  const int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  const int timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (wake < 0 || timer < 0 ||
      ALooper_addFd(current_looper, wake, ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, DrainTasks,
                    nullptr) != 1 ||
      ALooper_addFd(current_looper, timer, ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT,
                    PollClipboard, nullptr) != 1) {
    if (wake >= 0) {
      ALooper_removeFd(current_looper, wake);
      close(wake);
    }
    if (timer >= 0) {
      ALooper_removeFd(current_looper, timer);
      close(timer);
    }
    env->DeleteGlobalRef(retained);
    return false;
  }
  context = retained;
  looper = current_looper;
  ALooper_acquire(looper);
  wake_fd = wake;
  timer_fd = timer;
  previous_focus = activity->callbacks->onWindowFocusChanged;
  previous_destroy = activity->callbacks->onDestroy;
  activity->callbacks->onWindowFocusChanged = FocusChanged;
  activity->callbacks->onDestroy = Destroyed;
  SetMainThread();
  initialized.store(true, std::memory_order_release);
  return true;
}
bool IsInitialized() {
  return initialized.load(std::memory_order_acquire);
}
jobject Context() {
  return IsInitialized() ? context : nullptr;
}
bool IsActive() {
  return active;
}

Environment::Environment() {
  if (!IsInitialized())
    return;
  const jint result = vm->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    if (vm->AttachCurrentThread(&env_, nullptr) != JNI_OK) {
      env_ = nullptr;
      return;
    }
    attached_ = true;
  } else if (result != JNI_OK) {
    env_ = nullptr;
    return;
  }
  if (env_->PushLocalFrame(64) != JNI_OK) {
    ClearException(env_);
    if (attached_)
      vm->DetachCurrentThread();
    env_ = nullptr;
    attached_ = false;
    return;
  }
  frame_ = true;
}
Environment::~Environment() {
  if (frame_)
    env_->PopLocalFrame(nullptr);
  if (attached_)
    vm->DetachCurrentThread();
}
bool ClearException(JNIEnv* env) {
  if (!env || !env->ExceptionCheck())
    return false;
  env->ExceptionClear();
  return true;
}
bool Post(std::function<void()> work) {
  if (!IsInitialized())
    return false;
  if (!work)
    return true;
  std::lock_guard<std::mutex> lock(queue_mutex);
  tasks.push_back(std::move(work));
  uint64_t signal = 1;
  ssize_t written;
  do {
    written = write(wake_fd, &signal, sizeof(signal));
  } while (written < 0 && errno == EINTR);
  // A full eventfd is already readable, so EAGAIN does not lose the wakeup.
  if (written == sizeof(signal) || (written < 0 && errno == EAGAIN))
    return true;
  tasks.pop_back();
  return false;
}
bool RunSync(std::function<void()> work) {
  if (!IsInitialized())
    return false;
  if (IsMainThread()) {
    work();
    return true;
  }
  auto done = std::make_shared<std::promise<void>>();
  auto result = done->get_future();
  if (!Post([done, work = std::move(work)] {
        try {
          work();
          done->set_value();
        } catch (...) {
          done->set_exception(std::current_exception());
        }
      }))
    return false;
  result.get();
  return true;
}
bool SetClipboardPoll(std::function<void()> callback) {
  if (!IsInitialized())
    return false;
  itimerspec interval{};
  if (callback && active) {
    interval.it_value.tv_nsec = 100000000;
    interval.it_interval = interval.it_value;
  }
  if (timerfd_settime(timer_fd, 0, &interval, nullptr) != 0)
    return false;
  clipboard_poll = std::move(callback);
  return true;
}

jbyteArray ToJavaBytes(JNIEnv* env, const std::vector<unsigned char>& value) {
  if (value.size() > static_cast<size_t>(std::numeric_limits<jsize>::max()))
    return nullptr;
  auto bytes = env->NewByteArray(static_cast<jsize>(value.size()));
  if (bytes && !value.empty())
    env->SetByteArrayRegion(bytes, 0, value.size(), reinterpret_cast<const jbyte*>(value.data()));
  return ClearException(env) ? nullptr : bytes;
}
std::vector<unsigned char> FromJavaBytes(JNIEnv* env, jbyteArray value) {
  if (!value)
    return {};
  std::vector<unsigned char> result(env->GetArrayLength(value));
  if (!result.empty())
    env->GetByteArrayRegion(value, 0, result.size(), reinterpret_cast<jbyte*>(result.data()));
  return ClearException(env) ? std::vector<unsigned char>{} : result;
}
jstring ToJavaString(JNIEnv* env, const std::string& value) {
  // JNI's NewStringUTF is modified UTF-8: it does not accept ordinary emoji.
  auto bytes = ToJavaBytes(env, std::vector<unsigned char>(value.begin(), value.end()));
  auto cls = env->FindClass("java/lang/String");
  auto ctor = cls ? env->GetMethodID(cls, "<init>", "([BLjava/lang/String;)V") : nullptr;
  auto charset = env->NewStringUTF("UTF-8");
  if (ClearException(env) || !bytes || !ctor || !charset)
    return nullptr;
  auto result = static_cast<jstring>(env->NewObject(cls, ctor, bytes, charset));
  return ClearException(env) ? nullptr : result;
}
std::string FromJavaString(JNIEnv* env, jstring value) {
  if (!value)
    return {};
  const jsize length = env->GetStringLength(value);
  const jchar* chars = env->GetStringChars(value, nullptr);
  if (ClearException(env) || !chars)
    throw std::runtime_error("Cannot convert Java string");
  auto release = [env, value](const jchar* data) { env->ReleaseStringChars(value, data); };
  std::unique_ptr<const jchar, decltype(release)> retained(chars, release);
  std::string result;
  bool valid = true;
  for (jsize i = 0; i < length; ++i) {
    uint32_t scalar = chars[i];
    if (scalar >= 0xd800 && scalar <= 0xdbff) {
      if (++i >= length || chars[i] < 0xdc00 || chars[i] > 0xdfff) {
        valid = false;
        break;
      }
      scalar = 0x10000 + ((scalar - 0xd800) << 10) + (chars[i] - 0xdc00);
    } else if (scalar >= 0xdc00 && scalar <= 0xdfff) {
      valid = false;
      break;
    }
    if (scalar < 0x80)
      result += static_cast<char>(scalar);
    else if (scalar < 0x800) {
      result += static_cast<char>(0xc0 | (scalar >> 6));
      result += static_cast<char>(0x80 | (scalar & 63));
    } else if (scalar < 0x10000) {
      result += static_cast<char>(0xe0 | (scalar >> 12));
      result += static_cast<char>(0x80 | ((scalar >> 6) & 63));
      result += static_cast<char>(0x80 | (scalar & 63));
    } else {
      result += static_cast<char>(0xf0 | (scalar >> 18));
      result += static_cast<char>(0x80 | ((scalar >> 12) & 63));
      result += static_cast<char>(0x80 | ((scalar >> 6) & 63));
      result += static_cast<char>(0x80 | (scalar & 63));
    }
  }
  if (!valid)
    throw std::runtime_error("Invalid UTF-16 clipboard text");
  return result;
}

}  // namespace nativeapi::android
