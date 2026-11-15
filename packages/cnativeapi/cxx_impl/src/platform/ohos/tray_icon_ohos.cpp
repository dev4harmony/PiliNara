#include <multimedia/image_framework/image/pixelmap_native.h>
#include <uv.h>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include "../../app_info.h"
#include "../../application.h"
#include "../../foundation/object_registry.h"
#include "../../image.h"
#include "../../tray_icon.h"
#include "ohos_napi.h"
#include "ohos_runtime.h"
#include "ohos_tray.h"
#include "ohos_window.h"

namespace nativeapi {
namespace {
using Sink = std::function<void(const TrayIconEvent&)>;
struct Model {
  TrayIconId id = IdAllocator::Allocate<TrayIcon>();
  std::string identifier;
  size_t owners = 0;
  std::map<void*, Sink> sinks;
  std::shared_ptr<Image> icon;
  std::shared_ptr<View> content;
  std::shared_ptr<Menu> menu;
  Size size{18, 18};
  TrayIconPosition position = TrayIconPosition::Left;
  ContextMenuTrigger trigger = ContextMenuTrigger::None;
  std::optional<std::string> title, tooltip;
  bool visible = false, pending = false, alive = true;
  bool icon_template = false;
  uint64_t generation = 0;
  bool menu_dirty = false;
  std::map<std::string, MenuItemId> commands;
  std::optional<std::shared_ptr<Image>> queued_icon;
  std::optional<bool> queued_template;
  std::optional<std::optional<std::string>> queued_tooltip;
  bool drain_scheduled = false;
};
struct Delayed {
  uv_timer_t timer{};
  std::weak_ptr<Model> model;
};
struct Hub {
  std::recursive_mutex mutex;
  ObjectRegistry<Model, TrayIconId> models;
  std::shared_ptr<Model> active;
  napi_env env = nullptr;
  napi_ref module = nullptr;
  napi_ref click = nullptr;
  napi_ref menu_click = nullptr;
  size_t activation = 0;
  uintptr_t next_operation = 0;
  std::chrono::steady_clock::time_point last_icon_update{};
  enum class Kind { Show, Hide, Icon, Tooltip, Menu };
  struct Operation {
    std::shared_ptr<Model> model;
    uint64_t generation;
    Kind kind;
    std::shared_ptr<Image> icon;
    bool icon_template = false, clear_icon = false;
    std::optional<std::string> tooltip;
    std::map<std::string, MenuItemId> commands;
  };
  std::map<uintptr_t, Operation> operations;
  std::set<Delayed*> delayed;
};
Hub& H() {
  static auto* hub = new Hub;
  return *hub;
}
// State and SDK use are serialized on the owning UI loop after initialization.
// Before initialization only local configuration/identity operations can run.
template <class F>
bool Use(F work) {
  auto invoke = [&] {
    std::lock_guard<std::recursive_mutex> lock(H().mutex);
    work();
  };
  if (ohos::IsInitialized())
    return ohos::RunSync(invoke);
  invoke();
  return true;
}
napi_value Object(napi_env env) {
  napi_value v = nullptr;
  napi_create_object(env, &v);
  return v;
}
void Set(napi_env env, napi_value o, const char* key, napi_value v) {
  napi_set_named_property(env, o, key, v);
}
bool Module(napi_env env, napi_value& module) {
  if (H().env == env && H().module)
    return napi_get_reference_value(env, H().module, &module) == napi_ok && module;
  return ohos::js::Module(env, "@hms.pcService.statusBarManager", module);
}
bool Supported(napi_env env) {
  napi_value global, result, module;
  bool supported = false;
  return napi_get_global(env, &global) == napi_ok &&
         ohos::js::Call(env, global, "canIUse",
                        {ohos::String(env, "SystemCapability.PCService.StatusBarManager")},
                        result) &&
         napi_get_value_bool(env, result, &supported) == napi_ok && supported &&
         Module(env, module);
}
bool Visible(const std::shared_ptr<Model>& model, bool visible);
void UpdateMenu(const std::shared_ptr<Model>& model);
void Drain(const std::shared_ptr<Model>& model);
void ScheduleDrain(const std::shared_ptr<Model>& model);
void Cleanup(void*) {
  auto& h = H();
  std::lock_guard<std::recursive_mutex> lock(h.mutex);
  if (h.activation)
    Application::GetInstance().RemoveListener(h.activation);
  h.activation = 0;
  h.env = nullptr;
  h.module = nullptr;
  h.click = nullptr;
  h.menu_click = nullptr;
  for (auto& m : h.models.GetAll()) {
    m->alive = false;
    m->visible = false;
    m->pending = false;
    ++m->generation;
  }
  if (h.active) {
    h.active->alive = false;
    h.active->visible = false;
    h.active->pending = false;
    ++h.active->generation;
  }
  h.operations.clear();
  for (auto* delayed : h.delayed) {
    uv_timer_stop(&delayed->timer);
    uv_close(reinterpret_cast<uv_handle_t*>(&delayed->timer),
             [](uv_handle_t* handle) { delete static_cast<Delayed*>(handle->data); });
  }
  h.delayed.clear();
  h.active.reset();
}
napi_value Click(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value arg = nullptr, data = nullptr;
  napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr);
  std::vector<Sink> sinks;
  TrayIconId id = IdAllocator::kInvalidId;
  {
    std::lock_guard<std::recursive_mutex> lock(H().mutex);
    auto m = H().active;
    if (argc != 1 || H().env != env || !m || !m->alive || !m->visible || !m->owners ||
        !ohos::js::Property(env, arg, "data", data) ||
        ohos::js::StringProperty(env, data, "iconClickType") != "leftClick")
      return nullptr;
    id = m->id;
    for (auto& s : m->sinks)
      sinks.push_back(s.second);
  }
  TrayIconClickedEvent event(id);
  for (auto& sink : sinks)
    sink(event);
  return nullptr;
}
napi_value MenuClick(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value arg = nullptr, data = nullptr;
  napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr);
  std::shared_ptr<Menu> menu;
  MenuItemId id = IdAllocator::kInvalidId;
  {
    std::lock_guard<std::recursive_mutex> lock(H().mutex);
    auto m = H().active;
    if (argc != 1 || env != H().env || !m || !m->visible || !m->alive || !m->owners ||
        m->trigger != ContextMenuTrigger::RightClicked ||
        !ohos::js::Property(env, arg, "data", data))
      return nullptr;
    auto command = ohos::js::StringProperty(env, data, "menuCode");
    auto found = m->commands.find(command);
    if (found == m->commands.end())
      return nullptr;
    menu = m->menu;
    id = found->second;
  }
  // A menu selection is not a TrayIconRightClickedEvent. Recheck the live tree,
  // including enabled ancestors, so stale SDK commands cannot invoke removed items.
  ohos::DispatchTrayMenuItem(menu, id);
  return nullptr;
}
bool Initialize(napi_env env) {
  auto& h = H();
  if (h.env)
    return h.env == env;
  napi_value module, callback, result;
  if (!Supported(env) || !Module(env, module) ||
      napi_create_function(env, "nativeapi.tray.click", NAPI_AUTO_LENGTH, Click, nullptr,
                           &callback) != napi_ok ||
      napi_create_reference(env, callback, 1, &h.click) != napi_ok)
    return false;
  if (!ohos::js::Call(env, module, "on", {ohos::String(env, "statusBarIconClick"), callback},
                      result)) {
    napi_delete_reference(env, h.click);
    h.click = nullptr;
    return false;
  }
  if (napi_add_env_cleanup_hook(env, Cleanup, nullptr) != napi_ok) {
    ohos::js::Call(env, module, "off", {ohos::String(env, "statusBarIconClick"), callback}, result);
    napi_delete_reference(env, h.click);
    h.click = nullptr;
    return false;
  }
  if (napi_create_function(env, "nativeapi.tray.menu", NAPI_AUTO_LENGTH, MenuClick, nullptr,
                           &callback) != napi_ok ||
      napi_create_reference(env, callback, 1, &h.menu_click) != napi_ok ||
      !ohos::js::Call(env, module, "on", {ohos::String(env, "rightMenuClick"), callback}, result)) {
    napi_value click;
    napi_get_reference_value(env, h.click, &click);
    ohos::js::Call(env, module, "off", {ohos::String(env, "statusBarIconClick"), click}, result);
    napi_delete_reference(env, h.click);
    h.click = nullptr;
    if (h.menu_click)
      napi_delete_reference(env, h.menu_click);
    h.menu_click = nullptr;
    napi_remove_env_cleanup_hook(env, Cleanup, nullptr);
    return false;
  }
  if (napi_create_reference(env, module, 1, &h.module) != napi_ok) {
    napi_value click, menu_click;
    napi_get_reference_value(env, h.click, &click);
    napi_get_reference_value(env, h.menu_click, &menu_click);
    ohos::js::Call(env, module, "off", {ohos::String(env, "statusBarIconClick"), click}, result);
    ohos::js::Call(env, module, "off", {ohos::String(env, "rightMenuClick"), menu_click}, result);
    napi_delete_reference(env, h.click);
    napi_delete_reference(env, h.menu_click);
    h.click = nullptr;
    h.menu_click = nullptr;
    napi_remove_env_cleanup_hook(env, Cleanup, nullptr);
    return false;
  }
  h.env = env;
  h.activation = Application::GetInstance().AddListener<ApplicationActivatedEvent>([](const auto&) {
    Use([] {
      auto m = H().active;
      if (m && !m->owners && m->visible && !m->pending)
        Visible(m, false);
    });
  });
  return true;
}
bool Success(napi_env env, napi_value error) {
  napi_valuetype type = napi_undefined;
  napi_typeof(env, error, &type);
  if (type == napi_undefined || type == napi_null)
    return true;
  napi_value code = nullptr;
  int32_t value = -1;
  return ohos::js::Property(env, error, "code", code) &&
         napi_get_value_int32(env, code, &value) == napi_ok && value == 0;
}
napi_value Complete(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value error = nullptr;
  void* data = nullptr;
  napi_get_cb_info(env, info, &argc, &error, nullptr, &data);
  Use([&] {
    auto& h = H();
    auto it = h.operations.find(reinterpret_cast<uintptr_t>(data));
    if (env != h.env || it == h.operations.end())
      return;
    auto op = it->second;
    h.operations.erase(it);
    auto m = op.model;
    if (!m->alive || m->generation != op.generation)
      return;
    m->pending = false;
    if (Success(env, error)) {
      if (op.kind == Hub::Kind::Show)
        m->visible = true;
      if (op.kind == Hub::Kind::Hide) {
        m->visible = false;
        if (op.clear_icon)
          m->icon.reset();
      }
      if (op.kind == Hub::Kind::Icon) {
        m->icon = op.icon;
        m->icon_template = op.icon_template;
      }
      if (op.kind == Hub::Kind::Tooltip)
        m->tooltip = op.tooltip;
      if (op.kind == Hub::Kind::Show || op.kind == Hub::Kind::Menu)
        m->commands = std::move(op.commands);
    }
    if (!m->visible && h.active == m)
      h.active.reset();
    // The last wrapper may have gone away while addToStatusBar was pending.
    if (!m->owners && m->visible && op.kind != Hub::Kind::Hide)
      Visible(m, false);
    else if (m->owners && !m->pending &&
             (m->queued_icon || m->queued_template || m->queued_tooltip || m->menu_dirty))
      ScheduleDrain(m);
  });
  return nullptr;
}
bool Icons(napi_env env, const std::shared_ptr<Image>& image, bool as_template, napi_value& icons) {
  auto pixels = image ? static_cast<OH_PixelmapNative*>(image->GetNativeObject()) : nullptr;
  napi_value map = nullptr;
  if (!pixels)
    return false;
  icons = Object(env);
  if (!as_template) {
    // The status service can release its JS PixelMaps when replacing an icon.
    // Give it separate owned copies, never the caller's original or one shared
    // JS map for both themes.
    for (const char* name : {"white", "black"}) {
      OH_PixelmapNative* copy = nullptr;
      if (OH_PixelmapNative_Clone(pixels, &copy) != IMAGE_SUCCESS || !copy)
        return false;
      auto converted = OH_PixelmapNative_ConvertPixelmapNativeToNapi(env, copy, &map);
      OH_PixelmapNative_Destroy(&copy);
      if (converted != IMAGE_SUCCESS) {
        ohos::js::ClearException(env);
        return false;
      }
      Set(env, icons, name, map);
    }
    return true;
  }
  // Image decodes into RGBA_8888. Preserve the alpha bytes and let HMS select
  // between genuinely white/black pixels; never modify the caller's PixelMap.
  const auto size = image->GetSize();
  if (size.width <= 0 || size.height <= 0 || size.width * size.height > 1024 * 1024)
    return false;
  std::vector<uint8_t> bytes(static_cast<size_t>(size.width * size.height) * 4);
  size_t length = bytes.size();
  if (OH_PixelmapNative_ReadPixels(pixels, bytes.data(), &length) != IMAGE_SUCCESS ||
      length != bytes.size())
    return false;
  for (const char* name : {"white", "black"}) {
    const uint8_t value = name[0] == 'w' ? 255 : 0;
    for (size_t i = 0; i < bytes.size(); i += 4)
      bytes[i] = bytes[i + 1] = bytes[i + 2] = value;
    OH_Pixelmap_InitializationOptions* options = nullptr;
    if (OH_PixelmapInitializationOptions_Create(&options) != IMAGE_SUCCESS)
      return false;
    OH_PixelmapInitializationOptions_SetWidth(options, static_cast<uint32_t>(size.width));
    OH_PixelmapInitializationOptions_SetHeight(options, static_cast<uint32_t>(size.height));
    OH_PixelmapInitializationOptions_SetPixelFormat(options, PIXEL_FORMAT_RGBA_8888);
    OH_PixelmapInitializationOptions_SetSrcPixelFormat(options, PIXEL_FORMAT_RGBA_8888);
    OH_PixelmapInitializationOptions_SetAlphaType(options, PIXELMAP_ALPHA_TYPE_UNPREMULTIPLIED);
    OH_PixelmapNative* replacement = nullptr;
    auto created =
        OH_PixelmapNative_CreatePixelmap(bytes.data(), bytes.size(), options, &replacement);
    OH_PixelmapInitializationOptions_Release(options);
    if (created != IMAGE_SUCCESS || !replacement)
      return false;
    auto converted = OH_PixelmapNative_ConvertPixelmapNativeToNapi(env, replacement, &map);
    OH_PixelmapNative_Destroy(&replacement);
    if (converted != IMAGE_SUCCESS) {
      ohos::js::ClearException(env);
      return false;
    }
    Set(env, icons, name, map);
  }
  return true;
}
bool MenuSnapshot(napi_env env,
                  const std::shared_ptr<Model>& m,
                  napi_value context,
                  napi_value& groups,
                  std::map<std::string, MenuItemId>& commands) {
  napi_create_array(env, &groups);
  if (!m->menu || m->trigger != ContextMenuTrigger::RightClicked)
    return true;
  napi_value ability_info;
  if (!ohos::js::Property(env, context, "abilityInfo", ability_info))
    return false;
  const auto ability = ohos::js::StringProperty(env, ability_info, "name");
  const auto module = ohos::js::StringProperty(env, ability_info, "moduleName");
  if (ability.empty())
    return false;
  std::set<MenuId> menus;
  std::set<MenuItemId> ids;
  std::function<napi_value(const std::shared_ptr<MenuItem>&, bool)> entry;
  entry = [&](const std::shared_ptr<MenuItem>& item, bool sub) -> napi_value {
    if (!item || !item->IsEnabled() || !ids.insert(item->GetId()).second ||
        item->GetType() == MenuItemType::Separator || item->GetState() == MenuItemState::Mixed ||
        !item->GetAccelerator().ToString().empty() || item->GetTooltip().has_value())
      return nullptr;
    auto value = Object(env);
    Set(env, value, sub ? "subTitle" : "title", ohos::String(env, item->GetLabel().value_or("")));
    auto options = Object(env);
    Set(env, options, "selected", ohos::Boolean(env, item->GetState() == MenuItemState::Checked));
    if (auto image = item->GetIcon()) {
      napi_value icons;
      if (!Icons(env, image, false, icons))
        return nullptr;
      Set(env, options, "icon", icons);
    }
    Set(env, value, "options", options);
    if (auto child = item->GetSubmenu()) {
      if (sub || !menus.insert(child->GetId()).second)
        return nullptr;
      auto children = child->GetAllItems();
      if (children.empty() || children.size() > 20)
        return nullptr;
      napi_value array;
      napi_create_array(env, &array);
      size_t i = 0;
      for (auto& c : children) {
        auto v = entry(c, true);
        if (!v)
          return nullptr;
        napi_set_element(env, array, i++, v);
      }
      Set(env, value, "subMenu", array);
    } else {
      auto command = "nativeapi:" + std::to_string(m->id) + ":" +
                     std::to_string(m->generation + 1) + ":" + std::to_string(item->GetId());
      Set(env, value, "menuCode", ohos::String(env, command));
      auto action = Object(env);
      Set(env, action, "abilityName", ohos::String(env, ability));
      Set(env, action, "moduleName", ohos::String(env, module));
      Set(env, action, "notifyOnly", ohos::Boolean(env, true));
      Set(env, value, "menuAction", action);
      commands[command] = item->GetId();
    }
    return value;
  };
  menus.insert(m->menu->GetId());
  napi_value group;
  napi_create_array(env, &group);
  size_t g = 0, i = 0, total = 0;
  for (auto& item : m->menu->GetAllItems()) {
    if (item && item->GetType() == MenuItemType::Separator) {
      if (i) {
        napi_set_element(env, groups, g++, group);
        napi_create_array(env, &group);
        i = 0;
      }
      continue;
    }
    if (++total > 20)
      return false;
    auto value = entry(item, false);
    if (!value)
      return false;
    napi_set_element(env, group, i++, value);
  }
  if (i)
    napi_set_element(env, groups, g, group);
  return true;
}
bool Visible(const std::shared_ptr<Model>& m, bool visible) {
  if (!m || !m->alive || m->pending)
    return false;
  if (m->visible == visible)
    return true;
  if (visible && H().active && H().active != m)
    return false;
  bool accepted = false;
  ohos::WithContext([&](napi_env env, napi_value context) {
    if (!Initialize(env))
      return;
    napi_value module, callback, result, request = nullptr;
    std::map<std::string, MenuItemId> commands;
    if (!Module(env, module))
      return;
    std::vector<napi_value> args{context};
    if (visible) {
      napi_value icons;
      if (!Icons(env, m->icon, m->icon_template, icons))
        return;
      // HMS owns icon size/position. No fabricated inline title or tray view.
      request = Object(env);
      Set(env, request, "icons", icons);
      auto quick = Object(env);
      Set(env, quick, "title",
          ohos::String(env, m->title.value_or(AppInfo::GetInstance().GetName())));
      Set(env, quick, "height", ohos::Number(env, 1));
      Set(env, quick, "abilityName", ohos::String(env, ""));
      Set(env, request, "quickOperation", quick);
      if (m->tooltip && !m->tooltip->empty())
        Set(env, request, "hoverTips", ohos::String(env, *m->tooltip));
      napi_value groups;
      if (!MenuSnapshot(env, m, context, groups, commands))
        return;
      Set(env, request, "statusBarGroupMenu", groups);
    }
    auto token = ++H().next_operation;
    if (napi_create_function(env, "nativeapi.tray.complete", NAPI_AUTO_LENGTH, Complete,
                             reinterpret_cast<void*>(token), &callback) != napi_ok)
      return;
    H().operations.emplace(
        token, Hub::Operation{m, ++m->generation, visible ? Hub::Kind::Show : Hub::Kind::Hide});
    H().operations.at(token).commands = std::move(commands);
    m->menu_dirty = false;
    m->pending = true;
    if (visible)
      H().active = m;
    if (visible)
      args.push_back(request);
    args.push_back(callback);
    accepted = ohos::js::Call(env, module, visible ? "addToStatusBar" : "removeFromStatusBar", args,
                              result);
    if (!accepted) {
      H().operations.erase(token);
      m->pending = false;
      if (!m->visible && H().active == m)
        H().active.reset();
    }
  });
  return accepted;
}
void UpdateMenu(const std::shared_ptr<Model>& m) {
  if (!m || !m->alive || !m->visible)
    return;
  if (m->pending) {
    m->menu_dirty = true;
    return;
  }
  m->menu_dirty = false;
  ohos::WithContext([&](napi_env env, napi_value context) {
    napi_value groups, module, callback, result;
    std::map<std::string, MenuItemId> commands;
    if (env != H().env || !Module(env, module))
      return;
    if (!MenuSnapshot(env, m, context, groups, commands)) {
      napi_create_array(env, &groups);
      commands.clear();
    }
    auto token = ++H().next_operation;
    if (napi_create_function(env, "nativeapi.tray.menu.update", NAPI_AUTO_LENGTH, Complete,
                             reinterpret_cast<void*>(token), &callback) != napi_ok)
      return;
    auto op = Hub::Operation{m, ++m->generation, Hub::Kind::Menu};
    op.commands = std::move(commands);
    H().operations.emplace(token, std::move(op));
    m->pending = true;
    if (!ohos::js::Call(env, module, "updateStatusBarMenu", {context, groups, callback}, result)) {
      H().operations.erase(token);
      m->pending = false;
    }
  });
}
void UpdateIcon(const std::shared_ptr<Model>& m, std::shared_ptr<Image> image, bool as_template) {
  if (!m || !m->alive)
    return;
  if (m->pending || m->drain_scheduled) {
    m->queued_icon = std::move(image);
    return;
  }
  if (!m->visible) {
    m->icon = std::move(image);
    m->icon_template = as_template;
    return;
  }
  if (!image) {
    if (Visible(m, false)) {
      for (auto& op : H().operations)
        if (op.second.model == m && op.second.kind == Hub::Kind::Hide)
          op.second.clear_icon = true;
    }
    return;
  }
  if (std::chrono::steady_clock::now() - H().last_icon_update < std::chrono::seconds(1)) {
    m->queued_icon = std::move(image);
    m->queued_template = as_template;
    ScheduleDrain(m);
    return;
  }
  ohos::WithContext([&](napi_env env, napi_value context) {
    napi_value icons, module, callback, result;
    if (env != H().env || !Icons(env, image, as_template, icons) || !Module(env, module))
      return;
    auto token = ++H().next_operation;
    if (napi_create_function(env, "nativeapi.tray.icon", NAPI_AUTO_LENGTH, Complete,
                             reinterpret_cast<void*>(token), &callback) != napi_ok)
      return;
    auto op = Hub::Operation{m, ++m->generation, Hub::Kind::Icon};
    op.icon = image;
    op.icon_template = as_template;
    H().operations.emplace(token, std::move(op));
    m->pending = true;
    H().last_icon_update = std::chrono::steady_clock::now();
    if (!ohos::js::Call(env, module, "updateStatusBarIcon", {context, icons, callback}, result)) {
      H().operations.erase(token);
      m->pending = false;
    }
  });
}
void UpdateTooltip(const std::shared_ptr<Model>& m, std::optional<std::string> tooltip) {
  if (!m || !m->alive)
    return;
  if (m->pending || m->drain_scheduled) {
    m->queued_tooltip.emplace(std::move(tooltip));
    return;
  }
  if (!m->visible) {
    m->tooltip = std::move(tooltip);
    return;
  }
  ohos::WithContext([&](napi_env env, napi_value context) {
    napi_value module, promise, callback, result;
    if (env != H().env || !Module(env, module))
      return;
    // SDK has no reset operation; clearing restores the module's display name.
    auto text = tooltip.value_or(AppInfo::GetInstance().GetName());
    if (text.empty())
      text = AppInfo::GetInstance().GetName();
    if (!ohos::js::Call(env, module, "updateStatusBarHoverTips", {context, ohos::String(env, text)},
                        promise))
      return;
    auto token = ++H().next_operation;
    if (napi_create_function(env, "nativeapi.tray.tooltip", NAPI_AUTO_LENGTH, Complete,
                             reinterpret_cast<void*>(token), &callback) != napi_ok)
      return;
    auto op = Hub::Operation{m, ++m->generation, Hub::Kind::Tooltip};
    op.tooltip = tooltip;
    H().operations.emplace(token, std::move(op));
    m->pending = true;
    // A fulfilled void promise supplies undefined; rejection supplies the SDK error.
    if (!ohos::js::Call(env, promise, "then", {callback, callback}, result)) {
      H().operations.erase(token);
      m->pending = false;
    }
  });
}
void Drain(const std::shared_ptr<Model>& m) {
  if (!m || !m->alive || m->pending)
    return;
  if (m->queued_icon || m->queued_template) {
    auto icon = m->queued_icon ? *m->queued_icon : m->icon;
    auto as_template = m->queued_template.value_or(m->icon_template);
    m->queued_icon.reset();
    m->queued_template.reset();
    UpdateIcon(m, std::move(icon), as_template);
  }
  if (!m->pending && m->queued_tooltip) {
    auto tip = std::move(*m->queued_tooltip);
    m->queued_tooltip.reset();
    UpdateTooltip(m, std::move(tip));
  }
  if (!m->pending && m->menu_dirty)
    UpdateMenu(m);
}
void DrainTimer(uv_timer_t* timer) {
  auto* delayed = static_cast<Delayed*>(timer->data);
  auto m = delayed->model.lock();
  H().delayed.erase(delayed);
  uv_timer_stop(timer);
  uv_close(reinterpret_cast<uv_handle_t*>(timer),
           [](uv_handle_t* handle) { delete static_cast<Delayed*>(handle->data); });
  if (!m)
    return;
  Use([&] {
    m->drain_scheduled = false;
    if (m->owners)
      Drain(m);
  });
}
void ScheduleDrain(const std::shared_ptr<Model>& m) {
  if (m->drain_scheduled || !m->alive || !m->owners)
    return;
  ohos::WithEnvironment([&](napi_env env) {
    if (env != H().env)
      return;
    uv_loop_t* loop = nullptr;
    if (napi_get_uv_event_loop(env, &loop) != napi_ok || !loop)
      return;
    auto* delayed = new Delayed;
    delayed->model = m;
    if (uv_timer_init(loop, &delayed->timer) != 0) {
      delete delayed;
      return;
    }
    delayed->timer.data = delayed;
    // HMS rejects immediate repeated refreshes. Coalesce queued setters for one
    // second; the timer submits work only, and never changes applied SDK state.
    if (uv_timer_start(&delayed->timer, DrainTimer, 1000, 0) != 0) {
      uv_close(reinterpret_cast<uv_handle_t*>(&delayed->timer),
               [](uv_handle_t* handle) { delete static_cast<Delayed*>(handle->data); });
      return;
    }
    uv_unref(reinterpret_cast<uv_handle_t*>(&delayed->timer));
    H().delayed.insert(delayed);
    m->drain_scheduled = true;
  });
}
bool TextValid(const std::optional<std::string>& s) {
  return !s || s->find('\0') == s->npos;
}
}  // namespace
class TrayIcon::Impl {
 public:
  std::shared_ptr<Model> model;
};
TrayIcon::TrayIcon() : TrayIcon(std::string{}) {}
TrayIcon::TrayIcon(const std::string& identifier) : pimpl_(std::make_unique<Impl>()) {
  if (identifier.find('\0') != identifier.npos)
    return;
  auto m = std::make_shared<Model>();
  m->identifier = identifier;
  Use([&] {
    m->owners = 1;
    H().models.Add(m->id, m);
    pimpl_->model = m;
    m->sinks[this] =
        CreateGuardedCallback<const TrayIconEvent&>(Sink([this](const auto& e) { Emit(e); }));
  });
}
TrayIcon::TrayIcon(void* native) : pimpl_(std::make_unique<Impl>()) {
  Use([&] {
    for (auto& m : H().models.GetAll())
      if (m.get() == native && m->owners) {
        ++m->owners;
        pimpl_->model = m;
        m->sinks[this] =
            CreateGuardedCallback<const TrayIconEvent&>(Sink([this](const auto& e) { Emit(e); }));
        break;
      }
  });
}
TrayIcon::~TrayIcon() {
  ShutdownEmitter();
  auto m = pimpl_->model;
  Use([&] {
    if (!m)
      return;
    m->sinks.erase(this);
    if (--m->owners == 0) {
      H().models.Remove(m->id);
      if (m->visible && !m->pending)
        Visible(m, false);
    }
  });
}
TrayIconId TrayIcon::GetId() {
  return pimpl_->model ? pimpl_->model->id : IdAllocator::kInvalidId;
}
std::string TrayIcon::GetIdentifier() const {
  return pimpl_->model ? pimpl_->model->identifier : "";
}
void TrayIcon::SetIcon(std::shared_ptr<Image> image) {
  Use([&] {
    auto m = pimpl_->model;
    if (m)
      UpdateIcon(m, std::move(image), m->icon_template);
  });
}
std::shared_ptr<Image> TrayIcon::GetIcon() const {
  std::shared_ptr<Image> v;
  Use([&] {
    if (pimpl_->model)
      v = pimpl_->model->icon;
  });
  return v;
}
void TrayIcon::SetIconTemplate(bool value) {
  Use([&] {
    auto m = pimpl_->model;
    if (m && (m->pending || m->drain_scheduled))
      m->queued_template = value;
    else if (m)
      UpdateIcon(m, m->icon, value);
  });
}
bool TrayIcon::IsIconTemplate() const {
  bool v = false;
  Use([&] {
    if (pimpl_->model)
      v = pimpl_->model->icon_template;
  });
  return v;
}
void TrayIcon::SetIconSize(Size size) {
  if (!std::isfinite(size.width) || !std::isfinite(size.height) || size.width <= 0 ||
      size.height <= 0)
    return;
  Use([&] {
    if (pimpl_->model)
      pimpl_->model->size = size;
  });
}
Size TrayIcon::GetIconSize() const {
  Size v{18, 18};
  Use([&] {
    if (pimpl_->model)
      v = pimpl_->model->size;
  });
  return v;
}
void TrayIcon::SetIconPosition(TrayIconPosition p) {
  if (p != TrayIconPosition::Left && p != TrayIconPosition::Right)
    return;
  Use([&] {
    if (pimpl_->model)
      pimpl_->model->position = p;
  });
}
TrayIconPosition TrayIcon::GetIconPosition() const {
  auto v = TrayIconPosition::Left;
  Use([&] {
    if (pimpl_->model)
      v = pimpl_->model->position;
  });
  return v;
}
void TrayIcon::SetContentView(std::shared_ptr<View> view) {
  Use([&] {
    if (pimpl_->model)
      pimpl_->model->content = std::move(view);
  });
}
std::shared_ptr<View> TrayIcon::GetContentView() const {
  std::shared_ptr<View> v;
  Use([&] {
    if (pimpl_->model)
      v = pimpl_->model->content;
  });
  return v;
}
void TrayIcon::SetTitle(std::optional<std::string> title) {
  if (!TextValid(title))
    return;
  Use([&] {
    if (pimpl_->model)
      pimpl_->model->title = std::move(title);
  });
}
std::optional<std::string> TrayIcon::GetTitle() {
  std::optional<std::string> v;
  Use([&] {
    if (pimpl_->model)
      v = pimpl_->model->title;
  });
  return v;
}
void TrayIcon::SetTooltip(std::optional<std::string> tooltip) {
  if (!TextValid(tooltip))
    return;
  Use([&] { UpdateTooltip(pimpl_->model, std::move(tooltip)); });
}
std::optional<std::string> TrayIcon::GetTooltip() {
  std::optional<std::string> v;
  Use([&] {
    if (pimpl_->model)
      v = pimpl_->model->tooltip;
  });
  return v;
}
void TrayIcon::SetContextMenu(std::shared_ptr<Menu> menu) {
  Use([&] {
    if (auto m = pimpl_->model) {
      m->menu = std::move(menu);
      UpdateMenu(m);
    }
  });
}
std::shared_ptr<Menu> TrayIcon::GetContextMenu() {
  std::shared_ptr<Menu> v;
  Use([&] {
    if (pimpl_->model)
      v = pimpl_->model->menu;
  });
  return v;
}
void TrayIcon::SetContextMenuTrigger(ContextMenuTrigger t) {
  if (t != ContextMenuTrigger::None && t != ContextMenuTrigger::RightClicked)
    return;
  Use([&] {
    if (auto m = pimpl_->model) {
      m->trigger = t;
      UpdateMenu(m);
    }
  });
}
ContextMenuTrigger TrayIcon::GetContextMenuTrigger() {
  auto v = ContextMenuTrigger::None;
  Use([&] {
    if (pimpl_->model)
      v = pimpl_->model->trigger;
  });
  return v;
}
Rectangle TrayIcon::GetBounds() {
  return {};
}
bool TrayIcon::SetVisible(bool visible) {
  bool v = false;
  Use([&] { v = Visible(pimpl_->model, visible); });
  return v;
}
bool TrayIcon::IsVisible() {
  bool v = false;
  Use([&] {
    if (pimpl_->model)
      v = pimpl_->model->visible;
  });
  return v;
}
bool TrayIcon::OpenContextMenu() {
  return false;
}
bool TrayIcon::CloseContextMenu() {
  return false;
}
void TrayIcon::StartEventListening() {}
void TrayIcon::StopEventListening() {}
void* TrayIcon::GetNativeObjectInternal() const {
  return pimpl_->model.get();
}
namespace ohos {
void RefreshTrayMenu() {
  Use([] {
    auto m = H().active;
    if (m)
      UpdateMenu(m);
  });
}
bool TraySupported() {
  bool v = false;
  WithContext([&](napi_env env, napi_value) { v = Supported(env); });
  return v;
}
std::shared_ptr<TrayIcon> FindTray(TrayIconId id) {
  std::shared_ptr<TrayIcon> v;
  Use([&] {
    auto m = H().models.Get(id);
    if (m && m->owners)
      v = std::make_shared<TrayIcon>(m.get());
  });
  return v;
}
std::vector<std::shared_ptr<TrayIcon>> AllTrays() {
  std::vector<std::shared_ptr<TrayIcon>> v;
  Use([&] {
    for (auto& m : H().models.GetAll())
      if (m->owners)
        v.push_back(std::make_shared<TrayIcon>(m.get()));
  });
  return v;
}
}  // namespace ohos
}  // namespace nativeapi
