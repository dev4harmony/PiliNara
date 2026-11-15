#include "ohos_window_shadow.h"
#include <multimedia/image_framework/image/pixelmap_native.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>
#include "../window_shadow_blur.h"
#include "ohos_napi.h"
#include "ohos_runtime.h"
#include "ohos_sdk_request.h"
#include "ohos_shadow_raster.h"

namespace nativeapi::ohos {
struct CustomWindowShadow {
  std::shared_ptr<WindowShadow> options;
  std::shared_ptr<SdkRequest> request;
  uint64_t generation = 0;
  bool dirty = false;
  std::optional<bool> queued_visibility;
};
namespace {
struct ShadowHost {
  napi_env env = nullptr;
  napi_ref value = nullptr;
  uint64_t generation = 0;
};
ShadowHost& Host() {
  static auto* host = new ShadowHost;
  return *host;
}
void Cleanup(void*) {
  auto& host = Host();
  if (host.value)
    napi_delete_reference(host.env, host.value);
  host.value = nullptr;
  host.env = nullptr;
  ++host.generation;
}
napi_value HostValue(napi_env env) {
  napi_value value = nullptr;
  if (Host().env == env && Host().value)
    napi_get_reference_value(env, Host().value, &value);
  return value;
}
struct Validity {
  std::weak_ptr<WindowState> window;
  std::weak_ptr<CustomWindowShadow> shadow;
  uint64_t generation = 0, host_generation = 0, geometry_epoch = 0;
  bool IsValid() const {
    auto state = window.lock();
    auto current = shadow.lock();
    return state && state->alive && !state->destroying && current &&
           state->custom_shadow == current && current->generation == generation &&
           Host().generation == host_generation && Host().env == state->env &&
           state->shadow_geometry_epoch == geometry_epoch;
  }
};
napi_value Valid(napi_env env, napi_callback_info info) {
  void* data = nullptr;
  size_t count = 0;
  napi_get_cb_info(env, info, &count, nullptr, nullptr, &data);
  napi_value result = nullptr;
  napi_get_boolean(env, data && static_cast<Validity*>(data)->IsValid(), &result);
  return result;
}
napi_value ValidFunction(napi_env env, const Validity& validity) {
  auto held = std::make_unique<Validity>(validity);
  napi_value value = nullptr;
  if (napi_create_function(env, "nativeapi.shadow.valid", NAPI_AUTO_LENGTH, Valid, held.get(),
                           &value) != napi_ok ||
      napi_add_finalizer(
          env, value, held.get(),
          [](napi_env, void* p, void*) { delete static_cast<Validity*>(p); }, nullptr,
          nullptr) != napi_ok)
    return nullptr;
  held.release();
  return value;
}
WindowShape Contour(const std::shared_ptr<WindowState>& state,
                    int width,
                    int height,
                    double scale) {
  if (state->applied_shape)
    return *state->applied_shape;
  WindowShape polygon;
  napi_value value = nullptr;
  double radius = 0;
  if (js::Call(state->env, WindowObject(state), "getWindowCornerRadius", {}, value))
    napi_get_value_double(state->env, value, &radius);
  const double w = width / scale, h = height / scale;
  radius = std::max(0.0, std::min(radius, std::min(w, h) / 2));
  if (radius <= 0)
    return polygon;  // The shared renderer defaults to the full rectangle.
  constexpr double pi = 3.14159265358979323846;
  for (int corner = 0; corner < 4; ++corner) {
    const double cx = corner == 0 || corner == 3 ? radius : w - radius,
                 cy = corner < 2 ? radius : h - radius;
    for (int i = 0; i <= 12; ++i) {
      const double angle = (-180 + corner * 90 + i * 7.5) * pi / 180;
      polygon.AddPoint({cx + radius * std::cos(angle), cy + radius * std::sin(angle)});
    }
  }
  return polygon;
}
// Some phone runtimes reject the caption query with 801 even for a real
// undecorated child. Use an established successful style request, or initially
// prove captionlessness from a full-frame drawable rectangle. Resized buffers
// can retain their old drawable dimensions without changing the caption mode.
bool ReadDecor(const std::shared_ptr<WindowState>& state,
               const WindowManager_WindowProperties& properties,
               bool& decorated) {
  napi_value method = nullptr, value = nullptr;
  auto object = WindowObject(state);
  if (!js::Property(state->env, object, "getWindowDecorVisible", method))
    return false;
  if (napi_call_function(state->env, object, method, 0, nullptr, &value) == napi_ok) {
    if (napi_get_value_bool(state->env, value, &decorated) != napi_ok)
      return false;
    state->shadow_captionless_without_query = !decorated;
    return true;
  }
  bool pending = false;
  int32_t code = -1;
  napi_value error = nullptr;
  if (napi_is_exception_pending(state->env, &pending) == napi_ok && pending &&
      napi_get_and_clear_last_exception(state->env, &error) == napi_ok &&
      js::Property(state->env, error, "code", value))
    napi_get_value_int32(state->env, value, &code);
  const auto& frame = properties.windowRect;
  const auto& content = properties.drawableRect;
  if (code == 801 && !state->primary) {
    if (!state->shadow_captionless_without_query && content.posX == 0 && content.posY == 0 &&
        content.width == frame.width && content.height == frame.height)
      state->shadow_captionless_without_query = true;
    if (state->shadow_captionless_without_query.has_value()) {
      // A successful library style setter/query takes precedence. A resized
      // drawable buffer cannot turn an established mode into a new caption.
      decorated = !*state->shadow_captionless_without_query;
      return true;
    }
  }
  return false;
}
bool ReadFrameObject(napi_env env, napi_value rect, Rectangle& frame) {
  napi_value value = nullptr;
  double* fields[]{&frame.x, &frame.y, &frame.width, &frame.height};
  const char* names[]{"left", "top", "width", "height"};
  for (int i = 0; i < 4; ++i)
    if (!js::Property(env, rect, names[i], value) ||
        napi_get_value_double(env, value, fields[i]) != napi_ok || !std::isfinite(*fields[i]))
      return false;
  return frame.width >= 1 && frame.height >= 1 && frame.width <= 16384 && frame.height <= 16384 &&
         std::floor(frame.width) == frame.width && std::floor(frame.height) == frame.height;
}
bool ReadSdkFrame(const std::shared_ptr<WindowState>& state, Rectangle& frame) {
  if (state->custom_shadow && state->shadow_event_frame.width > 0) {
    frame = state->shadow_event_frame;
    return true;
  }
  napi_value properties = nullptr, rect = nullptr;
  return js::Call(state->env, WindowObject(state), "getWindowProperties", {}, properties) &&
         js::Property(state->env, properties, "windowRect", rect) &&
         ReadFrameObject(state->env, rect, frame);
}
bool SameFrame(Rectangle a, Rectangle b) {
  return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
struct RenderWork {
  napi_async_work work = nullptr;
  Validity validity;
  std::shared_ptr<SdkRequest> request;
  std::shared_ptr<WindowShadow> options;
  WindowShape polygon;
  int width = 0, height = 0, margin = 0;
  double scale = 1;
  std::vector<uint8_t> bytes;
  bool enabled = false, primary = false;
  void Render() {
    auto raster = RasterizeWindowShadow(width, height, scale, polygon, *options);
    margin = raster.margin;
    bytes = std::move(raster.rgba);
  }
  void Publish(napi_env env) {
    auto state = validity.window.lock();
    auto shadow = validity.shadow.lock();
    if (!validity.IsValid() || !state || !shadow || request->done) {
      request->Finish(false);
      return;
    }
    Rectangle current_frame{};
    if (enabled && (!ReadSdkFrame(state, current_frame) || current_frame.width != width ||
                    current_frame.height != height)) {
      shadow->dirty = true;
      request->Finish(false);
      return;
    }
    napi_value pixels = nullptr;
    if (enabled) {
      if (bytes.empty()) {
        request->Finish(false);
        return;
      }
      OH_Pixelmap_InitializationOptions* init = nullptr;
      OH_PixelmapInitializationOptions_Create(&init);
      if (!init) {
        request->Finish(false);
        return;
      }
      OH_PixelmapInitializationOptions_SetWidth(init, width + 2 * margin);
      OH_PixelmapInitializationOptions_SetHeight(init, height + 2 * margin);
      OH_PixelmapInitializationOptions_SetPixelFormat(init, PIXEL_FORMAT_RGBA_8888);
      OH_PixelmapInitializationOptions_SetSrcPixelFormat(init, PIXEL_FORMAT_RGBA_8888);
      OH_PixelmapInitializationOptions_SetAlphaType(init, PIXELMAP_ALPHA_TYPE_UNPREMULTIPLIED);
      OH_PixelmapNative* map = nullptr;
      auto status = OH_PixelmapNative_CreatePixelmap(bytes.data(), bytes.size(), init, &map);
      OH_PixelmapInitializationOptions_Release(init);
      if (status != IMAGE_SUCCESS || !map) {
        request->Finish(false);
        return;
      }
      status = OH_PixelmapNative_ConvertPixelmapNativeToNapi(env, map, &pixels);
      OH_PixelmapNative_Destroy(&map);
      if (status != IMAGE_SUCCESS) {
        js::ClearException(env);
        request->Finish(false);
        return;
      }
    } else
      napi_get_undefined(env, &pixels);
    auto valid = ValidFunction(env, validity);
    auto host = HostValue(env);
    if (!valid || !host || !request->Keep(valid) || (enabled && !request->Keep(pixels))) {
      request->Finish(false);
      return;
    }
    request->Start(
        env, "apply", host,
        {WindowObject(state), pixels, Number(env, margin), Number(env, width + 2 * margin),
         Number(env, height + 2 * margin), Boolean(env, enabled), Boolean(env, primary), valid});
  }
};
bool Start(const std::shared_ptr<WindowState>& state,
           const std::shared_ptr<WindowShadow>& options,
           bool enabled,
           bool reset = false,
           bool refresh = false) {
  auto request = std::make_shared<SdkRequest>();
  bool submitted = false;
  UseWindow(state, [&] {
    if (!HostValue(state->env) || state->destroying)
      return;
    auto shadow = state->custom_shadow;
    if (shadow && shadow->request)
      return;
    auto window = WindowObject(state);
    WindowManager_WindowProperties properties{};
    if (!ReadWindow(state, properties) || !properties.windowRect.width ||
        !properties.windowRect.height)
      return;
    Rectangle frame{};
    if (!ReadSdkFrame(state, frame))
      return;
    bool decorated = true;
    if (!reset && !ReadDecor(state, properties, decorated))
      return;
    const bool suspended = !reset && decorated;
    if (suspended && !refresh)
      return;
    if (!shadow) {
      shadow = std::make_shared<CustomWindowShadow>();
      state->custom_shadow = shadow;
      state->shadow_event_frame = {};
      ++state->shadow_geometry_epoch;
    }
    request->env = state->env;
    request->name = "custom shadow render";
    if (napi_add_env_cleanup_hook(state->env, SdkRequest::Cleanup, request.get()) != napi_ok) {
      if (!shadow->options)
        state->custom_shadow.reset();
      return;
    }
    request->hook = true;
    const uint64_t generation = ++shadow->generation;
    shadow->request = request;
    request->completed = [weak = std::weak_ptr<WindowState>(state),
                          held = std::weak_ptr<CustomWindowShadow>(shadow), generation, options,
                          enabled, reset, frame](bool ok) {
      auto current = weak.lock();
      auto config = held.lock();
      if (!current || !config || config->generation != generation)
        return;
      config->request.reset();
      if (ok && current->alive && current->custom_shadow == config) {
        current->shadow = enabled;
        if (reset)
          current->custom_shadow.reset();
        else
          config->options = options;
      }
      if (!ok && !config->options && current->custom_shadow == config)
        current->custom_shadow.reset();
      // A phone's NDK rectangle can lag its public SDK resize notification.
      // Refresh only on an observed geometry change, never retry generic errors.
      Rectangle latest{};
      if (config->options && current->alive && ReadSdkFrame(current, latest) &&
          !SameFrame(frame, latest))
        config->dirty = true;
      const auto visibility = std::exchange(config->queued_visibility, std::nullopt);
      if (visibility && config->options && current->alive && current->custom_shadow == config)
        ToggleWindowCustomShadow(current, *visibility);
      const bool dirty = std::exchange(config->dirty, false);
      if (dirty && current->alive && current->custom_shadow == config)
        RefreshWindowCustomShadow(current);
      if (!current->custom_shadow)
        ReleaseWindowShadowGeometry(current);
    };
    if (reset || suspended) {
      submitted = request->Start(
          state->env, "clear", HostValue(state->env),
          {window, Boolean(state->env, enabled), Boolean(state->env, state->primary)});
      return;
    }
    if (!ObserveWindowShadowGeometry(state)) {
      request->Finish(false);
      return;
    }
    auto job = std::make_unique<RenderWork>();
    job->validity = {state, shadow, generation, Host().generation, state->shadow_geometry_epoch};
    job->request = request;
    job->options = options;
    job->enabled = enabled;
    job->primary = state->primary;
    job->width = static_cast<int>(frame.width);
    job->height = static_cast<int>(frame.height);
    job->scale = WindowScale(state);
    job->polygon = Contour(state, job->width, job->height, job->scale);
    if (!enabled) {
      job->Publish(state->env);
      submitted = true;
      return;
    }
    napi_value name = String(state->env, "nativeapi.shadow.raster");
    if (napi_create_async_work(
            state->env, nullptr, name,
            [](napi_env, void* p) {
              try {
                static_cast<RenderWork*>(p)->Render();
              } catch (...) {
              }
            },
            [](napi_env env, napi_status status, void* p) {
              std::unique_ptr<RenderWork> job(static_cast<RenderWork*>(p));
              try {
                if (status == napi_ok)
                  job->Publish(env);
                else
                  job->request->Finish(false);
              } catch (...) {
                job->request->Finish(false);
              }
              napi_delete_async_work(env, job->work);
            },
            job.get(), &job->work) != napi_ok ||
        napi_queue_async_work(state->env, job->work) != napi_ok) {
      if (job->work)
        napi_delete_async_work(state->env, job->work);
      request->Finish(false);
      return;
    }
    job.release();
    submitted = true;
  });
  // UseWindow catches allocation/SDK exceptions. Do not strand a protected
  // worker request or leave the target permanently pending after submission fails.
  if (!submitted)
    WithEnvironment([&](napi_env env) {
      if (env != request->env)
        return;
      request->Finish(false);
      // If callback allocation itself failed, Finish has no completion closure
      // to release the pending record. Do not strand subsequent requests.
      if (state && state->env == env && state->custom_shadow &&
          state->custom_shadow->request == request) {
        auto config = state->custom_shadow;
        config->request.reset();
        if (!config->options)
          state->custom_shadow.reset();
        if (!state->custom_shadow)
          ReleaseWindowShadowGeometry(state);
      }
    });
  return request->Result(submitted) && state && state->alive;
}
}  // namespace
bool AttachWindowShadowHost(napi_env env, napi_value value) {
  bool same = false;
  WithEnvironment([&](napi_env current) { same = current == env; });
  if (!same || !IsMainThread())
    return false;
  napi_value method = nullptr;
  napi_valuetype type = napi_undefined;
  for (const char* name : {"apply", "clear", "hide", "dispose"})
    if (!js::Property(env, value, name, method) || napi_typeof(env, method, &type) != napi_ok ||
        type != napi_function)
      return false;
  if (Host().value)
    return false;  // Explicit detach/Stage teardown owns replacement.
  napi_ref ref = nullptr;
  if (napi_create_reference(env, value, 1, &ref) != napi_ok)
    return false;
  if (napi_add_env_cleanup_hook(env, Cleanup, nullptr) != napi_ok) {
    napi_delete_reference(env, ref);
    return false;
  }
  Host().env = env;
  Host().value = ref;
  ++Host().generation;
  return true;
}
bool SetWindowCustomShadow(const std::shared_ptr<WindowState>& state,
                           std::shared_ptr<WindowShadow> shadow) try {
  bool enabled = false;
  UseWindow(state, [&] { enabled = state->shadow; });
  return Start(state, shadow ? std::make_shared<WindowShadow>(*shadow) : nullptr, enabled, !shadow);
} catch (...) {
  return false;
}
std::shared_ptr<WindowShadow> WindowCustomShadow(const std::shared_ptr<WindowState>& state) try {
  std::shared_ptr<WindowShadow> result;
  UseWindow(state, [&] {
    if (state->custom_shadow && state->custom_shadow->options)
      result = std::make_shared<WindowShadow>(*state->custom_shadow->options);
  });
  return result;
} catch (...) {
  return nullptr;
}
bool ToggleWindowCustomShadow(const std::shared_ptr<WindowState>& state, bool enabled) {
  std::shared_ptr<WindowShadow> options;
  bool present = false;
  bool queued = false;
  UseWindow(state, [&] {
    present = bool(state->custom_shadow);
    if (present) {
      options = state->custom_shadow->options;
      if (state->custom_shadow->request) {
        state->custom_shadow->queued_visibility = enabled;
        queued = true;
      }
    }
  });
  if (!present)
    return false;
  if (options && !queued)
    Start(state, options, enabled);
  return true;
}
void ObserveWindowCustomShadowFrame(const std::shared_ptr<WindowState>& state,
                                    napi_value event) try {
  if (!state || !state->custom_shadow)
    return;
  napi_value rect = nullptr;
  Rectangle frame{};
  if (js::Property(state->env, event, "rect", rect) && ReadFrameObject(state->env, rect, frame) &&
      !SameFrame(frame, state->shadow_event_frame)) {
    state->shadow_event_frame = frame;
    ++state->shadow_geometry_epoch;
  }
} catch (...) {
}
void ObserveWindowCustomShadowSize(const std::shared_ptr<WindowState>& state, napi_value size) try {
  if (!state || !state->custom_shadow)
    return;
  Rectangle frame{};
  napi_value width = nullptr, height = nullptr;
  if (!ReadSdkFrame(state, frame) || !js::Property(state->env, size, "width", width) ||
      !js::Property(state->env, size, "height", height) ||
      napi_get_value_double(state->env, width, &frame.width) != napi_ok ||
      napi_get_value_double(state->env, height, &frame.height) != napi_ok ||
      !std::isfinite(frame.width) || !std::isfinite(frame.height) || frame.width < 1 ||
      frame.height < 1 || frame.width > 16384 || frame.height > 16384 ||
      std::floor(frame.width) != frame.width || std::floor(frame.height) != frame.height)
    return;
  if (!SameFrame(frame, state->shadow_event_frame)) {
    state->shadow_event_frame = frame;
    ++state->shadow_geometry_epoch;
  }
} catch (...) {
}
void RefreshWindowCustomShadow(const std::shared_ptr<WindowState>& state) try {
  if (!state || !state->custom_shadow)
    return;
  auto shadow = state->custom_shadow;
  if (shadow->request) {
    shadow->dirty = true;
    return;
  }
  if (!shadow->options)
    return;
  // This observer executes on UI, so Start never blocks the event loop.
  Start(state, shadow->options, state->shadow, false, true);
} catch (...) {
}
void ForgetWindowCustomShadow(const std::shared_ptr<WindowState>& state, bool notify) {
  if (!state || !state->custom_shadow)
    return;
  auto shadow = state->custom_shadow;
  ++shadow->generation;
  if (shadow->request) {
    shadow->request->Finish(false, false);
    shadow->request.reset();
  }
  state->custom_shadow.reset();
  if (notify) {
    napi_value host = HostValue(state->env), ignored = nullptr;
    if (host)
      js::Call(state->env, host, "hide", {Number(state->env, state->system_id)}, ignored);
  }
}
}  // namespace nativeapi::ohos
