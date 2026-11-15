#include "ohos_arkui.h"
#include <map>
#include <dlfcn.h>
#include "../../window_manager.h"
#include "ohos_napi.h"
#include "ohos_window.h"

namespace nativeapi::ohos {
ArkUI_NativeNodeAPI_1* NodeApi() {
  static ArkUI_NativeNodeAPI_1* api = nullptr;
  if (!api)
    api = reinterpret_cast<ArkUI_NativeNodeAPI_1*>(
        OH_ArkUI_QueryModuleInterfaceByName(ARKUI_NATIVE_NODE, "ArkUI_NativeNodeAPI_1"));
  return api;
}
int32_t NodeUniqueId(ArkUI_NodeHandle node) {
  if (!node || !NodeApi()) return -1;
  using Query = int32_t (*)(ArkUI_NodeHandle, int32_t*);
  static const auto query = reinterpret_cast<Query>(dlsym(RTLD_DEFAULT, "OH_ArkUI_NodeUtils_GetNodeUniqueId"));
  if (query) {
    int32_t id = -1;
    return query(node, &id) == 0 ? id : -1;
  }
  const auto* item = NodeApi()->getAttribute(node, NODE_UNIQUE_ID);
  return item && item->value && item->size > 0 ? item->value[0].i32 : -1;
}
ArkUI_NativeDialogAPI_3* DialogApi() {
  static ArkUI_NativeDialogAPI_3* api = nullptr;
  if (!api)
    api = reinterpret_cast<ArkUI_NativeDialogAPI_3*>(
        OH_ArkUI_QueryModuleInterfaceByName(ARKUI_NATIVE_DIALOG, "ArkUI_NativeDialogAPI_3"));
  return api;
}
namespace {
struct ScopeTask {
  const std::function<void()>* work;
  bool invoked = false, success = false;
};
// Only live synchronous calls are registered. An unexpectedly retained JS callback
// sees a missing token instead of dereferencing a caller's stack after return.
std::map<uintptr_t, ScopeTask*> scope_tasks;
uintptr_t next_scope_token = 0;
}  // namespace
bool WithUiContext(const std::shared_ptr<Window>& window, const std::function<void()>& work) {
  auto parent = window ? window : WindowManager::GetInstance().GetCurrent();
  if (!parent)
    return false;
  auto state = FindWindowState(parent->GetNativeObject());
  bool success = false;
  UseWindow(state, [&] {
    napi_value context = nullptr, callback = nullptr, ignored = nullptr;
    if (!js::Call(state->env, WindowObject(state), "getUIContext", {}, context))
      return;
    ScopeTask task{&work};
    const auto token = ++next_scope_token;
    if (napi_create_function(
            state->env, "nativeapi.scopedTask", NAPI_AUTO_LENGTH,
            [](napi_env env, napi_callback_info info) -> napi_value {
              void* data = nullptr;
              size_t count = 0;
              napi_get_cb_info(env, info, &count, nullptr, nullptr, &data);
              auto found = scope_tasks.find(reinterpret_cast<uintptr_t>(data));
              if (found != scope_tasks.end() && !found->second->invoked) {
                auto& task = *found->second;
                task.invoked = true;
                try {
                  (*task.work)();
                  task.success = true;
                } catch (...) {
                }
              }
              napi_value value = nullptr;
              napi_get_undefined(env, &value);
              return value;
            },
            reinterpret_cast<void*>(token), &callback) != napi_ok)
      return;
    scope_tasks.emplace(token, &task);
    // The public UIContext scope API avoids allocating an opaque native context
    // wrapper: GetContextFromNapiValue has no matching public release operation.
    const bool called = js::Call(state->env, context, "runScopedTask", {callback}, ignored);
    scope_tasks.erase(token);
    success = called && task.success;
  });
  return success;
}
bool NodeFloat(ArkUI_NodeHandle node, ArkUI_NodeAttributeType attribute, float number) {
  ArkUI_NumberValue value{};
  value.f32 = number;
  ArkUI_AttributeItem item{&value, 1, nullptr, nullptr};
  return node && NodeApi() && NodeApi()->setAttribute(node, attribute, &item) == 0;
}
bool NodeInt(ArkUI_NodeHandle node, ArkUI_NodeAttributeType attribute, int32_t number) {
  ArkUI_NumberValue value{};
  value.i32 = number;
  ArkUI_AttributeItem item{&value, 1, nullptr, nullptr};
  return node && NodeApi() && NodeApi()->setAttribute(node, attribute, &item) == 0;
}
bool NodeText(ArkUI_NodeHandle node, ArkUI_NodeAttributeType attribute, const std::string& text) {
  ArkUI_AttributeItem item{nullptr, 0, text.c_str(), nullptr};
  return node && NodeApi() && NodeApi()->setAttribute(node, attribute, &item) == 0;
}
std::string NodeTextValue(ArkUI_NodeHandle node, ArkUI_NodeAttributeType attribute) {
  if (!node || !NodeApi())
    return {};
  auto* item = NodeApi()->getAttribute(node, attribute);
  return item && item->string ? item->string : "";
}
}  // namespace nativeapi::ohos
