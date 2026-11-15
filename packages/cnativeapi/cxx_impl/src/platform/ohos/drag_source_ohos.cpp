#include "../../drag_source_impl.h"

#include <arkui/drag_and_drop.h>
#include <database/udmf/udmf_err_code.h>
#include <database/udmf/udmf_meta.h>
#include <database/udmf/uds.h>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include "../../image.h"
#include "ohos_arkui.h"
#include "ohos_file_uri.h"
#include "ohos_runtime.h"
#include "ohos_window.h"

namespace nativeapi {

bool DragSource::IsOperationSelectionSupported() {
  return false;
}
struct DragSource::Impl::Platform {
  template <class T, auto Free>
  using Owned = std::unique_ptr<T, decltype(Free)>;
  struct Session {
    Impl* impl = nullptr;  // Read/written only on the UI thread.
    napi_env env = nullptr;
    uintptr_t token = 0;
    std::atomic<bool> active{true}, started{false}, ended{false};
    Owned<ArkUI_DragAction, OH_ArkUI_DragAction_Dispose> action{nullptr,
                                                                OH_ArkUI_DragAction_Dispose};
    Owned<OH_UdmfData, OH_UdmfData_Destroy> data{nullptr, OH_UdmfData_Destroy};
    std::shared_ptr<Image> preview;
    std::function<void(DragOperation)> deliver;
    std::function<void()> cancel;
  };
  inline static std::mutex mutex;
  inline static std::map<uintptr_t, std::shared_ptr<Session>> sessions;
  inline static std::set<napi_env> environments;
  inline static uintptr_t next_token = 0;
  std::shared_ptr<Session> session;

  static void Forget(const std::shared_ptr<Session>& state) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      sessions.erase(state->token);
    }
    if (state->action)
      OH_ArkUI_DragAction_UnregisterStatusListener(state->action.get());
    state->action.reset();
    state->data.reset();
    state->preview.reset();
  }
  static void Cleanup(void* data) {
    std::vector<std::shared_ptr<Session>> pending;
    const auto env = static_cast<napi_env>(data);
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (const auto& item : sessions)
        if (item.second->env == env)
          pending.push_back(item.second);
      environments.erase(env);
    }
    for (const auto& state : pending) {
      state->active = false;
      if (state->cancel)
        state->cancel();
      state->impl = nullptr;
      Forget(state);
    }
  }
  static void Status(ArkUI_DragAndDropInfo* info, void* data) {
    std::shared_ptr<Session> state;
    {
      std::lock_guard<std::mutex> lock(mutex);
      auto found = sessions.find(reinterpret_cast<uintptr_t>(data));
      if (found == sessions.end())
        return;
      state = found->second;
    }
    const auto status = OH_ArkUI_DragAndDropInfo_GetDragStatus(info);
    if (status == ARKUI_DRAG_STATUS_STARTED) {
      state->started = true;
      return;
    }
    if (status != ARKUI_DRAG_STATUS_ENDED || state->ended.exchange(true))
      return;
    auto* event = OH_ArkUI_DragAndDropInfo_GetDragEvent(info);
    ArkUI_DragResult result = ARKUI_DRAG_RESULT_FAILED;
    ArkUI_DropOperation operation = ARKUI_DROP_OPERATION_MOVE;
    DragOperation completed = DragOperation::None;
    if (state->started && event && OH_ArkUI_DragEvent_GetDragResult(event, &result) == 0 &&
        result == ARKUI_DRAG_RESULT_SUCCESSFUL &&
        OH_ArkUI_DragEvent_GetDropOperation(event, &operation) == 0) {
      if (operation == ARKUI_DROP_OPERATION_COPY)
        completed = DragOperation::Copy;
      else if (operation == ARKUI_DROP_OPERATION_MOVE)
        completed = DragOperation::Move;
    }
    // Status notifications arrive on an SDK worker. Borrowed event/info objects
    // must not escape this callback, and no generic inline dispatch fallback is safe.
    ohos::Post([state, completed] {
      Forget(state);
      state->deliver(completed);
    });
  }
  static bool Data(Impl& impl, const std::shared_ptr<Session>& state) {
    if (impl.operation != DragOperation::Copy ||
        (impl.text && impl.text->find('\0') != std::string::npos))
      return false;
    state->data.reset(OH_UdmfData_Create());
    if (!state->data)
      return false;
    if (impl.text) {
      Owned<OH_UdsPlainText, OH_UdsPlainText_Destroy> text(OH_UdsPlainText_Create(),
                                                           OH_UdsPlainText_Destroy);
      Owned<OH_UdmfRecord, OH_UdmfRecord_Destroy> record(OH_UdmfRecord_Create(),
                                                         OH_UdmfRecord_Destroy);
      if (!text || !record ||
          OH_UdsPlainText_SetContent(text.get(), impl.text->c_str()) != UDMF_E_OK ||
          OH_UdmfRecord_AddPlainText(record.get(), text.get()) != UDMF_E_OK ||
          OH_UdmfData_AddRecord(state->data.get(), record.get()) != UDMF_E_OK)
        return false;
    }
    for (const auto& path : impl.file_paths) {
      std::string uri;
      bool directory = false;
      if (!ohos::LocalPathToUri(path, uri, directory))
        return false;
      Owned<OH_UdsFileUri, OH_UdsFileUri_Destroy> file(OH_UdsFileUri_Create(),
                                                       OH_UdsFileUri_Destroy);
      Owned<OH_UdmfRecord, OH_UdmfRecord_Destroy> record(OH_UdmfRecord_Create(),
                                                         OH_UdmfRecord_Destroy);
      if (!file || !record || OH_UdsFileUri_SetFileUri(file.get(), uri.c_str()) != UDMF_E_OK ||
          OH_UdsFileUri_SetFileType(
              file.get(), directory ? UDMF_META_FOLDER : UDMF_META_GENERAL_FILE) != UDMF_E_OK ||
          OH_UdmfRecord_AddFileUri(record.get(), file.get()) != UDMF_E_OK ||
          OH_UdmfData_AddRecord(state->data.get(), record.get()) != UDMF_E_OK)
        return false;
    }
    state->preview = impl.image;
    if (!state->preview) {
      // The SDK requires a PixelMap even for text/file drags. Supply a compact
      // default tile when the caller did not provide a custom preview.
      state->preview = Image::FromBase64(
          "iVBORw0KGgoAAAANSUhEUgAAABgAAAAYCAYAAADgdz34AAAAJElEQVR4nGNg+P//"
          "P03xqAWjFoxaMGrBqAWjFoxaMGrB0LAAAMXouZvMgZVyAAAAAElFTkSuQmCC");
    }
    return state->preview && state->preview->GetNativeObject();
  }
};

bool DragSource::IsSupported() {
  bool supported = false;
  ohos::RunSync([&] { supported = ohos::NodeApi() != nullptr; });
  return supported;
}
bool DragSource::IsPositionSupported() {
  return false;
}
DragSource::Impl::Impl(DragSource* owner) : owner(owner), platform(std::make_unique<Platform>()) {}
DragSource::Impl::~Impl() {
  if (auto state = platform->session) {
    state->active = false;
    // The SDK owns an ongoing drag until release. Keep its immutable payload
    // and preview in the registry; the final notification retires them.
    ohos::RunSync([state] { state->impl = nullptr; });
  }
}
bool DragSource::Impl::Start() {
  bool success = false;
  ohos::RunSync([&] {
    {
      std::lock_guard<std::mutex> lock(Platform::mutex);
      for (const auto& item : Platform::sessions)
        if (!item.second->ended)
          return;  // Do not disturb another NativeAPI source's live SDK session.
    }
    auto window_state = ohos::FindWindowState(window->GetNativeObject());
    auto state = std::make_shared<Platform::Session>();
    if (!window_state || !window_state->alive || !Platform::Data(*this, state))
      return;
    state->impl = this;
    state->env = window_state->env;
    if (!Platform::environments.count(state->env)) {
      if (napi_add_env_cleanup_hook(state->env, Platform::Cleanup, state->env) != napi_ok)
        return;
      Platform::environments.insert(state->env);
    }
    // An ephemeral scoped node supplies the UI instance without replacing the
    // application's content or allocating a native UIContext wrapper.
    ohos::WithUiContext(window, [&] {
      auto node = ohos::NodeApi()->createNode(ARKUI_NODE_STACK);
      if (node) {
        state->action.reset(OH_ArkUI_CreateDragActionWithNode(node));
        ohos::NodeApi()->disposeNode(node);
      }
    });
    auto* action = state->action.get();
    auto* pixels = static_cast<OH_PixelmapNative*>(state->preview->GetNativeObject());
    if (!action || OH_ArkUI_DragAction_SetPointerId(action, 0) != 0 ||
        OH_ArkUI_DragAction_SetData(action, state->data.get()) != 0 ||
        OH_ArkUI_DragAction_SetPixelMaps(action, &pixels, 1) != 0)
      return;
    {
      std::lock_guard<std::mutex> lock(Platform::mutex);
      state->token = ++Platform::next_token;
      Platform::sessions.emplace(state->token, state);
    }
    platform->session = state;
    std::weak_ptr<Platform::Session> weak = state;
    state->cancel = owner->CreateGuardedCallback<>(std::function<void()>([weak] {
      auto state = weak.lock();
      if (!state || !state->impl)
        return;
      auto* impl = state->impl;
      impl->dragging = false;
      auto window = std::move(impl->window);
      impl->platform->session.reset();
    }));
    state->deliver = owner->CreateGuardedCallback<DragOperation>(
        std::function<void(DragOperation)>([weak](DragOperation result) {
          auto state = weak.lock();
          if (!state || !state->active.exchange(false))
            return;
          auto* impl = state->impl;
          state->impl = nullptr;
          if (!impl)
            return;
          impl->platform->session.reset();
          // The public SDK supplies no end coordinates; the capability query
          // distinguishes this origin fallback from an observed position.
          impl->Finish({}, result);
        }));
    const bool submitted =
        OH_ArkUI_DragAction_RegisterStatusListener(action, reinterpret_cast<void*>(state->token),
                                                   Platform::Status) == 0 &&
        OH_ArkUI_StartDrag(action) == 0;
    // The public StartDrag wrapper may return zero even when the engine refused
    // the request. An immediate ENDED without STARTED is a failed submission.
    if (!submitted || (state->ended && !state->started)) {
      state->active = false;
      state->impl = nullptr;
      platform->session.reset();
      Platform::Forget(state);
      return;
    }
    success = true;
  });
  return success;
}
}  // namespace nativeapi
