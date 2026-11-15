#include "ohos_window_modes.h"

#include "ohos_napi.h"
#include "ohos_runtime.h"
#include "ohos_sdk_request.h"

namespace nativeapi::ohos {
namespace {
using State = std::shared_ptr<WindowState>;
constexpr uint32_t kFull = 1u << 0, kSplit = 1u << 1, kFloating = 1u << 2;
bool Valid(uint32_t mask, bool primary) {
  return mask && !(mask & ~(kFull | kSplit | kFloating)) &&
         (mask & (kFull | kFloating)) && (primary || !(mask & kSplit));
}
std::optional<uint32_t> DeclaredModes() {
  std::optional<uint32_t> result;
  WithContext([&](napi_env env, napi_value context) {
    napi_value info = nullptr, modes = nullptr;
    bool array = false;
    uint32_t length = 0, mask = 0;
    if (!js::Property(env, context, "abilityInfo", info) ||
        !js::Property(env, info, "supportWindowModes", modes) ||
        napi_is_array(env, modes, &array) != napi_ok || !array ||
        napi_get_array_length(env, modes, &length) != napi_ok || !length || length > 3)
      return;
    for (uint32_t i = 0; i < length; ++i) {
      napi_value value = nullptr;
      double number = -1;
      if (napi_get_element(env, modes, i, &value) != napi_ok ||
          napi_get_value_double(env, value, &number) != napi_ok ||
          (number != 0 && number != 1 && number != 2))
        return;
      const auto bit = 1u << static_cast<uint32_t>(number);
      if (mask & bit) return;
      mask |= bit;
    }
    if (Valid(mask, true)) result = mask;
  });
  return result;
}
std::optional<uint32_t> Baseline(const State& state) {
  if (state->supported_window_modes) return state->supported_window_modes;
  if (state->primary) return DeclaredModes();
  // A floating child proves that FLOATING is allowed. SPLIT is never legal for
  // children. Do not invent a floating capability for an already-fullscreen
  // child whose host may have configured FULL_SCREEN only.
  if (WindowStatus(state) == 4) return kFloating;
  return std::nullopt;
}
void Cancel(const State& state) {
  while (!state->mode_requests.empty()) {
    auto request = state->mode_requests.front().second;
    state->mode_requests.pop_front();
    request->Finish(false, false);
  }
  state->cancel_modes = {};
}
void ApplyNext(const State& state) try {
  if (state->mode_requests.empty()) return;
  if (!state->alive || state->destroying) { Cancel(state); return; }
  const auto enabled = state->mode_requests.front().first;
  auto request = state->mode_requests.front().second;
  const auto baseline = Baseline(state);
  const uint32_t mask = baseline ? (enabled ? *baseline | kFull : *baseline & ~kFull) : 0;
  request->completed = [weak = std::weak_ptr<WindowState>(state),
                        operation = std::weak_ptr<SdkRequest>(request), mask](bool ok) {
    auto current = weak.lock();
    if (!current || current->mode_requests.empty() ||
        current->mode_requests.front().second != operation.lock()) return;
    if (ok && current->alive) current->supported_window_modes = mask;
    current->mode_requests.pop_front();
    if (current->mode_requests.empty()) current->cancel_modes = {};
    else if (!Post([weak] { if (auto s = weak.lock()) ApplyNext(s); })) Cancel(current);
    if (ok && current->alive) RefreshWindowProperties(current);
  };
  if (!baseline || !Valid(mask, state->primary)) { request->Finish(false); return; }
  napi_value array = nullptr;
  if (napi_create_array(state->env, &array) != napi_ok) { request->Finish(false); return; }
  uint32_t index = 0;
  for (uint32_t mode = 0; mode < 3; ++mode) {
    if ((mask & (1u << mode)) &&
        napi_set_element(state->env, array, index++, Number(state->env, mode)) != napi_ok) {
      request->Finish(false); return;
    }
  }
  napi_value object = WindowObject(state), method = nullptr;
  napi_valuetype type = napi_undefined;
  if (!js::Property(state->env, object, "setSupportedWindowModes", method) ||
      napi_typeof(state->env, method, &type) != napi_ok) { request->Finish(false); return; }
  if (type == napi_undefined && state->primary) object = WindowStageObject(state);
  else if (type != napi_function) { request->Finish(false); return; }
  request->Start(state->env, "setSupportedWindowModes", object, {array});
} catch (...) {
  Cancel(state);
}
}  // namespace

bool SetWindowModeBaseline(const std::shared_ptr<Window>& window,
                           const std::vector<int32_t>& modes) try {
  if (!window) return false;
  const auto state = FindWindowState(window->GetNativeObject());
  bool applied = false;
  UseWindow(state, [&] {
    if (state->destroying || !state->mode_requests.empty()) return;
    uint32_t mask = 0;
    if (modes.empty() || modes.size() > 3) return;
    for (const auto mode : modes) {
      if (mode < 0 || mode > 2 || (mask & (1u << mode))) return;
      mask |= 1u << mode;
    }
    if (!Valid(mask, state->primary)) return;
    state->supported_window_modes = mask;
    applied = true;
    RefreshWindowProperties(state);
  });
  return applied;
} catch (...) { return false; }

void SetWindowFullScreenable(const State& state, bool enabled) try {
  auto request = std::make_shared<SdkRequest>();
  bool queued = false;
  UseWindow(state, [&] {
    if (state->destroying) return;
    state->cancel_modes = [weak = std::weak_ptr<WindowState>(state)] {
      if (auto current = weak.lock()) Cancel(current);
    };
    const bool start = state->mode_requests.empty();
    state->mode_requests.emplace_back(enabled, request);
    queued = true;
    if (start) ApplyNext(state);
  });
  request->Result(queued);
} catch (...) {}

bool WindowFullScreenable(const State& state) try {
  bool enabled = false;
  UseWindow(state, [&] {
    // A child has no public capability getter. Until the host supplies a known
    // mode mask or an SDK policy request succeeds, false includes unknown.
    const auto modes = state->supported_window_modes ? state->supported_window_modes :
                       (state->primary ? DeclaredModes() : std::optional<uint32_t>{});
    enabled = modes && (*modes & kFull);
  });
  return enabled;
} catch (...) { return false; }
}  // namespace nativeapi::ohos
