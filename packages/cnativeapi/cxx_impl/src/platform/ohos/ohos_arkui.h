#pragma once
// Internal native ArkUI seam. All operations execute on the owning UI instance.
#include <arkui/native_dialog.h>
#include <arkui/native_interface.h>
#include <arkui/native_node.h>
#include <functional>
#include <memory>
#include <string>
namespace nativeapi {
class Window;
namespace ohos {
ArkUI_NativeNodeAPI_1* NodeApi();
// API 20 native query, with the public legacy node attribute on older systems.
int32_t NodeUniqueId(ArkUI_NodeHandle node);
ArkUI_NativeDialogAPI_3* DialogApi();
bool WithUiContext(const std::shared_ptr<Window>& window, const std::function<void()>& work);
bool NodeFloat(ArkUI_NodeHandle node, ArkUI_NodeAttributeType attribute, float value);
bool NodeInt(ArkUI_NodeHandle node, ArkUI_NodeAttributeType attribute, int32_t value);
bool NodeText(ArkUI_NodeHandle node, ArkUI_NodeAttributeType attribute, const std::string& value);
std::string NodeTextValue(ArkUI_NodeHandle node, ArkUI_NodeAttributeType attribute);
}  // namespace ohos
}  // namespace nativeapi
