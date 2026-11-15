#include "../../drop_target_impl.h"

#include <arkui/drag_and_drop.h>
#include <database/udmf/udmf_err_code.h>
#include <database/udmf/udmf_meta.h>
#include <database/udmf/uds.h>
#include <database/udmf/utd.h>
#include <array>
#include <cstring>
#include <map>
#include <set>
#include "../../view.h"
#include "ohos_arkui.h"
#include "ohos_file_uri.h"
#include "ohos_runtime.h"
#include "ohos_window.h"

namespace nativeapi {
// Native ArkUI destinations belong to a component, not the window manager. The
// host must mount NativeAPI's content root before creating a live destination.
struct DropTarget::Impl::Platform {
  struct Lease {
    Impl* impl = nullptr;
    napi_env env = nullptr;
    ArkUI_NodeHandle node = nullptr;
    uintptr_t token = 0;
    int events = 0;
    bool receiver = false;
    std::shared_ptr<View> root;
    std::shared_ptr<ohos::WindowState> window;
  };
  inline static std::map<uintptr_t, std::shared_ptr<Lease>> leases;
  inline static std::set<napi_env> environments;
  inline static uintptr_t next_token = 0;
  std::shared_ptr<Lease> lease;
  inline static constexpr std::array<ArkUI_NodeEventType, 4> events{
      NODE_ON_DRAG_ENTER, NODE_ON_DRAG_MOVE, NODE_ON_DRAG_LEAVE, NODE_ON_DROP};

  static void Remove(const std::shared_ptr<Lease>& state) {
    if (!state)
      return;
    state->impl = nullptr;
    leases.erase(state->token);
    if (state->node && ohos::NodeApi()) {
      for (int i = 0; i < state->events; ++i)
        ohos::NodeApi()->unregisterNodeEvent(state->node, events[i]);
      if (state->receiver)
        ohos::NodeApi()->removeNodeEventReceiver(state->node, Event);
      OH_ArkUI_DisallowNodeAnyDropDataTypes(state->node);
    }
    state->node = nullptr;
    state->events = 0;
    state->root.reset();
    state->window.reset();
  }
  static void Cleanup(void* data) {
    const auto env = static_cast<napi_env>(data);
    std::vector<std::shared_ptr<Lease>> pending;
    for (const auto& item : leases)
      if (item.second->env == env)
        pending.push_back(item.second);
    for (const auto& state : pending) {
      if (state->impl) {
        state->impl->registered = false;
        state->impl->accepted = false;
      }
      Remove(state);
    }
    environments.erase(env);
  }
  static bool HasData(ArkUI_DragEvent* event) {
    int32_t count = 0;
    if (OH_ArkUI_DragEvent_GetDataTypeCount(event, &count) != 0 || count < 1 || count > 256)
      return false;
    std::vector<std::array<char, 1024>> storage(count);
    std::vector<char*> types;
    for (auto& type : storage)
      types.push_back(type.data());
    if (OH_ArkUI_DragEvent_GetDataTypes(event, types.data(), count, storage[0].size()) != 0)
      return false;
    for (const char* type : types)
      if (!std::strcmp(type, UDMF_META_PLAIN_TEXT) ||
          !std::strcmp(type, UDMF_META_GENERAL_FILE_URI) ||
          OH_Utd_BelongsTo(type, UDMF_META_GENERAL_FILE) ||
          OH_Utd_BelongsTo(type, UDMF_META_FOLDER))
        return true;
    return false;
  }
  static unsigned Operations(ArkUI_DragEvent* event) {
    // Public ArkUI exposes a suggested operation, not an allowed-operation mask.
    // Only the non-destructive Copy path is currently supported. Never silently
    // downgrade an explicit Move or claim the SDK's missing Link operation.
    ArkUI_DropOperation operation = ARKUI_DROP_OPERATION_MOVE;
    return OH_ArkUI_DragEvent_GetDropOperation(event, &operation) == 0 &&
                   operation == ARKUI_DROP_OPERATION_COPY
               ? OperationBit(DragOperation::Copy)
               : 0;
  }
  static bool Read(ArkUI_DragEvent* event, std::vector<std::string>& files, std::string& text) {
    std::unique_ptr<OH_UdmfData, decltype(&OH_UdmfData_Destroy)> data(OH_UdmfData_Create(),
                                                                      OH_UdmfData_Destroy);
    if (!data || OH_ArkUI_DragEvent_GetUdmfData(event, data.get()) != UDMF_E_OK)
      return false;
    bool has_data = false;
    if (OH_UdmfData_HasType(data.get(), UDMF_META_PLAIN_TEXT)) {
      std::unique_ptr<OH_UdsPlainText, decltype(&OH_UdsPlainText_Destroy)> plain(
          OH_UdsPlainText_Create(), OH_UdsPlainText_Destroy);
      if (!plain || OH_UdmfData_GetPrimaryPlainText(data.get(), plain.get()) != UDMF_E_OK)
        return false;
      const char* content = OH_UdsPlainText_GetContent(plain.get());
      if (!content)
        return false;
      text = content;
      has_data = true;
    }
    // File URI may be a secondary entry of a record whose primary type is a
    // more specific file format; inspect every record, as in the pasteboard.
    {
      unsigned count = 0;
      auto** records = OH_UdmfData_GetRecords(data.get(), &count);
      if (!records || count > 4096)
        return false;
      for (unsigned i = 0; i < count; ++i) {
        unsigned type_count = 0;
        auto** types = OH_UdmfRecord_GetTypes(records[i], &type_count);
        bool is_file = false;
        for (unsigned t = 0; types && t < type_count; ++t)
          is_file |= types[t] && !std::strcmp(types[t], UDMF_META_GENERAL_FILE_URI);
        if (!is_file)
          continue;
        std::unique_ptr<OH_UdsFileUri, decltype(&OH_UdsFileUri_Destroy)> file(
            OH_UdsFileUri_Create(), OH_UdsFileUri_Destroy);
        if (!file || OH_UdmfRecord_GetFileUri(records[i], file.get()) != UDMF_E_OK)
          return false;
        const char* uri = OH_UdsFileUri_GetFileUri(file.get());
        std::string path;
        if (!uri || !ohos::UriToLocalPath(uri, path))
          return false;
        files.push_back(std::move(path));
        has_data = true;
      }
    }
    return has_data;
  }
  static void Event(ArkUI_NodeEvent* node_event) {
    const auto type = OH_ArkUI_NodeEvent_GetEventType(node_event);
    const int target = OH_ArkUI_NodeEvent_GetTargetId(node_event);
    if (target < 1 || target > 4 || type != events[target - 1])
      return;
    auto found =
        leases.find(reinterpret_cast<uintptr_t>(OH_ArkUI_NodeEvent_GetUserData(node_event)));
    if (found == leases.end())
      return;
    const auto state = found->second;  // A listener may remove/destroy its target.
    auto* impl = state->impl;
    auto* event = OH_ArkUI_NodeEvent_GetDragEvent(node_event);
    if (!impl || !impl->registered || !state->window || !state->window->alive || !event)
      return;
    const auto scale = ohos::WindowScale(state->window);
    const auto bounds = ohos::WindowBounds(state->window);
    const auto content = ohos::WindowBounds(state->window, true);
    const Point position{
        OH_ArkUI_DragEvent_GetTouchPointXToWindow(event) / scale - (content.x - bounds.x),
        OH_ArkUI_DragEvent_GetTouchPointYToWindow(event) / scale - (content.y - bounds.y)};
    if (target == 3) {
      impl->Exited(position);
      return;
    }
    const auto operations = Operations(event);
    DragOperation operation = DragOperation::None;
    if (target == 1)
      operation = impl->Entered(position, operations, HasData(event));
    else if (target == 2)
      operation = impl->Moved(position, operations);
    else if (target == 4) {
      std::vector<std::string> files;
      std::string text;
      if (impl->accepted && impl->operation == DragOperation::Copy && operations &&
          Read(event, files, text)) {
        OH_ArkUI_DragEvent_SetSuggestedDropOperation(event, ARKUI_DROP_OPERATION_COPY);
        OH_ArkUI_DragEvent_SetDragResult(event, ARKUI_DRAG_RESULT_SUCCESSFUL);
        impl->Dropped(position, std::move(files), std::move(text));
      } else {
        OH_ArkUI_DragEvent_SetDragResult(event, ARKUI_DRAG_RESULT_FAILED);
        impl->Exited(position);
      }
      return;
    }
    // User callbacks can unregister, destroy, or change the target operation.
    const bool accepted = state->impl && state->impl->registered &&
                          state->impl->operation == DragOperation::Copy &&
                          operation == DragOperation::Copy;
    OH_ArkUI_DragEvent_SetDragResult(
        event, accepted ? ARKUI_DRAG_RESULT_SUCCESSFUL : ARKUI_DRAG_RESULT_FAILED);
    if (accepted)
      OH_ArkUI_DragEvent_SetSuggestedDropOperation(event, ARKUI_DROP_OPERATION_COPY);
  }
};

bool DropTarget::IsSupported() {
  bool supported = false;
  ohos::RunSync([&] { supported = ohos::NodeApi() != nullptr; });
  return supported;
}
DropTarget::Impl::Impl(DropTarget* owner, std::shared_ptr<Window> window)
    : owner(owner),
      window(std::move(window)),
      window_id(this->window ? this->window->GetId() : 0),
      platform(std::make_unique<Platform>()) {}
DropTarget::Impl::~Impl() {
  Unregister();
}

bool DropTarget::Impl::Register() {
  bool success = false;
  ohos::RunSync([&] {
    auto root = window->GetContentView();
    auto node = root ? static_cast<ArkUI_NodeHandle>(root->GetNativeObject()) : nullptr;
    auto state = ohos::FindWindowState(window->GetNativeObject());
    if (!node || !state || !state->alive || !ohos::NodeApi())
      return;
    for (const auto& item : Platform::leases)
      if (item.second->node == node)
        return;  // Exactly one active target owns a NativeAPI root.
    if (!Platform::environments.count(state->env)) {
      if (napi_add_env_cleanup_hook(state->env, Platform::Cleanup, state->env) != napi_ok)
        return;
      Platform::environments.insert(state->env);
    }
    auto lease = std::make_shared<Platform::Lease>();
    lease->root = root;
    lease->node = node;
    lease->window = state;
    lease->env = state->env;
    lease->impl = this;
    lease->token = ++Platform::next_token;
    Platform::leases.emplace(lease->token, lease);
    const char* types[]{UDMF_META_PLAIN_TEXT, UDMF_META_GENERAL_FILE_URI, UDMF_META_GENERAL_FILE,
                        UDMF_META_FOLDER};
    bool ok = OH_ArkUI_SetNodeAllowedDropDataTypes(node, types, 4) == 0;
    if (ok) {
      ok = ohos::NodeApi()->addNodeEventReceiver(node, Platform::Event) == 0;
      lease->receiver = ok;
    }
    for (int i = 0; ok && i < 4; ++i) {
      ok = ohos::NodeApi()->registerNodeEvent(node, Platform::events[i], i + 1,
                                              reinterpret_cast<void*>(lease->token)) == 0;
      if (ok)
        ++lease->events;
    }
    if (!ok) {
      Platform::Remove(lease);
      return;
    }
    platform->lease = std::move(lease);
    success = true;
  });
  return success;
}
void DropTarget::Impl::Unregister() {
  auto lease = std::move(platform->lease);
  if (lease)
    ohos::RunSync([lease] { Platform::Remove(lease); });
}
}  // namespace nativeapi
