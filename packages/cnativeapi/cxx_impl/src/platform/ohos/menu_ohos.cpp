#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include "../../foundation/dispatcher_common.h"
#include "../../image.h"
#include "../../menu.h"
#include "../../window_manager.h"
#include "ohos_menus.h"
#include "ohos_tray.h"
#include "ohos_napi.h"
#include "ohos_runtime.h"
#include "ohos_window.h"

namespace nativeapi {
namespace {
struct MenuModel;
struct ItemModel;
struct Session;
bool Main() {
  return dispatcher_internal::IsMainThreadByCapturedId();
}
using Sink = std::function<void(const MenuEvent&)>;
struct MenuModel {
  MenuId id = IdAllocator::Allocate<Menu>();
  std::vector<std::shared_ptr<MenuItem>> items;
  std::map<void*, Sink> sinks;
  size_t owners = 0;
};
struct ItemModel {
  MenuItemId id = IdAllocator::Allocate<MenuItem>();
  MenuItemType type = MenuItemType::Normal;
  std::optional<std::string> label, tooltip;
  std::shared_ptr<Image> image;
  std::string icon;
  KeyboardAccelerator accelerator;
  bool enabled = true;
  MenuItemState state = MenuItemState::Unchecked;
  int radio_group = -1;
  std::shared_ptr<Menu> submenu;
  std::map<void*, Sink> sinks;
};
std::map<void*, std::weak_ptr<MenuModel>> menus;
std::map<void*, std::weak_ptr<ItemModel>> items;
struct Host {
  std::weak_ptr<ohos::WindowState> window;
  napi_env env = nullptr;
  napi_ref object = nullptr;
  size_t closed_listener = 0;
};
std::map<WindowId, Host> hosts;
std::set<napi_env> environments;
std::map<uintptr_t, std::shared_ptr<Session>> sessions;
uintptr_t next_session = 0;
template <class T>
std::shared_ptr<T> Lookup(const std::map<void*, std::weak_ptr<T>>& map, void* token) {
  auto found = map.find(token);
  return found == map.end() ? nullptr : found->second.lock();
}
std::shared_ptr<MenuModel> Model(const std::shared_ptr<Menu>& menu) {
  return menu ? Lookup(menus, menu->GetNativeObject()) : nullptr;
}
std::shared_ptr<ItemModel> Model(const std::shared_ptr<MenuItem>& item) {
  return item ? Lookup(items, item->GetNativeObject()) : nullptr;
}
template <class T, class E>
void Emit(const std::shared_ptr<T>& object, const E& event) {
  auto sinks = object->sinks;
  for (const auto& entry : sinks)
    entry.second(event);
}
bool HasMenu(const std::shared_ptr<MenuModel>& root, MenuId id, std::set<MenuId>& visited) {
  if (!root || !visited.insert(root->id).second)
    return false;
  if (root->id == id)
    return true;
  for (auto& item : root->items)
    if (auto model = Model(item))
      if (HasMenu(Model(model->submenu), id, visited))
        return true;
  return false;
}
bool HasItem(const std::shared_ptr<MenuModel>& root, MenuItemId id, std::set<MenuId>& visited) {
  if (!root || !visited.insert(root->id).second)
    return false;
  for (auto& item : root->items)
    if (auto model = Model(item)) {
      if (model->id == id || HasItem(Model(model->submenu), id, visited))
        return true;
    }
  return false;
}
bool Valid(const std::optional<std::string>& text) {
  return !text || text->find('\0') == text->npos;
}
void Refresh();
bool Active(MenuId id);
void End(const std::shared_ptr<Session>& session, bool notify);
bool RequestClose(const std::shared_ptr<Session>& session);

struct Session {
  uintptr_t token = ++next_session;
  std::shared_ptr<MenuModel> root;
  WindowId window = IdAllocator::kInvalidId;
  napi_ref callback = nullptr;
  napi_ref provider = nullptr;
  napi_env env = nullptr;
  bool opened = false, closing = false, ended = false, clicked = false;
  std::map<MenuItemId, std::shared_ptr<ItemModel>> entries;
  std::map<MenuItemId, std::pair<std::shared_ptr<ItemModel>, std::shared_ptr<MenuModel>>> submenus;
  std::map<MenuItemId, std::pair<std::shared_ptr<ItemModel>, std::shared_ptr<MenuModel>>>
      visible_submenus;
  ~Session() {
    if (callback && env)
      napi_delete_reference(env, callback);
    if (provider && env)
      napi_delete_reference(env, provider);
  }
};
void Set(napi_env env, napi_value object, const char* name, napi_value value) {
  napi_set_named_property(env, object, name, value);
}
void Text(napi_env env, napi_value object, const char* name, const std::string& value) {
  Set(env, object, name, ohos::String(env, value));
}
void Number(napi_env env, napi_value object, const char* name, double value) {
  Set(env, object, name, ohos::Number(env, value));
}
napi_value Snapshot(napi_env env,
                    const std::shared_ptr<MenuModel>& menu,
                    const std::shared_ptr<Session>& session,
                    std::set<MenuId>& path) {
  if (!menu || !path.insert(menu->id).second)
    return nullptr;
  napi_value result, children;
  napi_create_object(env, &result);
  napi_create_array_with_length(env, menu->items.size(), &children);
  Number(env, result, "id", menu->id);
  Set(env, result, "items", children);
  size_t index = 0;
  for (auto& wrapper : menu->items) {
    auto item = Model(wrapper);
    if (!item)
      return nullptr;
    napi_value entry;
    napi_create_object(env, &entry);
    Number(env, entry, "id", item->id);
    Number(env, entry, "type", static_cast<int>(item->type));
    Text(env, entry, "label", item->label.value_or(""));
    Text(env, entry, "tooltip", item->tooltip.value_or(""));
    Text(env, entry, "icon", item->icon);
    Number(env, entry, "state", static_cast<int>(item->state));
    Set(env, entry, "enabled", ohos::Boolean(env, item->enabled));
    Text(env, entry, "accelerator", item->accelerator.ToString());
    Text(env, entry, "key",
         (static_cast<uint32_t>(item->accelerator.modifiers) & ~uint32_t{7})
             ? ""
             : item->accelerator.key);
    Number(env, entry, "modifiers", static_cast<uint32_t>(item->accelerator.modifiers));
    session->entries[item->id] = item;
    if (auto child = Model(item->submenu)) {
      auto subtree = Snapshot(env, child, session, path);
      if (!subtree)
        return nullptr;
      Set(env, entry, "submenu", subtree);
      session->submenus[item->id] = {item, child};
    }
    napi_set_element(env, children, index++, entry);
  }
  path.erase(menu->id);
  return result;
}
bool CallHost(const std::shared_ptr<Session>& session,
              const char* method,
              std::vector<napi_value> args) {
  if (!session->provider || !environments.count(session->env))
    return false;
  napi_value object = nullptr, result = nullptr;
  if (napi_get_reference_value(session->env, session->provider, &object) != napi_ok ||
      !ohos::js::Call(session->env, object, method, args, result))
    return false;
  bool accepted = false;
  return napi_get_value_bool(session->env, result, &accepted) == napi_ok && accepted;
}
void End(const std::shared_ptr<Session>& session, bool notify) {
  if (session->ended)
    return;
  session->ended = true;
  sessions.erase(session->token);
  auto submenus = std::move(session->visible_submenus);
  if (notify) {
    for (auto& pair : submenus) {
      Emit(pair.second.second, MenuClosedEvent(pair.second.second->id));
      Emit(pair.second.first, MenuItemSubmenuClosedEvent(pair.first));
    }
    if (session->opened)
      Emit(session->root, MenuClosedEvent(session->root->id));
  }
}
napi_value NativeEvent(napi_env env, napi_callback_info info) {
  size_t count = 2;
  napi_value args[2]{};
  void* data = nullptr;
  napi_get_cb_info(env, info, &count, args, nullptr, &data);
  auto found = sessions.find(reinterpret_cast<uintptr_t>(data));
  if (count != 2 || found == sessions.end())
    return nullptr;
  auto session = found->second;
  uint32_t id = 0;
  if (session->env != env || session->ended || napi_get_value_uint32(env, args[1], &id) != napi_ok)
    return nullptr;
  const auto event = ohos::js::String(env, args[0]);
  if (event == "open" && id == session->root->id && !session->opened) {
    session->opened = true;
    Emit(session->root, MenuOpenedEvent(id));
  } else if (event == "close" && id == session->root->id)
    End(session, true);
  else if (event == "failed" && id == session->root->id)
    End(session, false);
  else if (event == "close-failed" && id == session->root->id)
    session->closing = false;
  else if ((event == "click" || event == "shortcut") && session->opened && !session->closing &&
           !session->clicked) {
    auto item = session->entries.find(id);
    if (item != session->entries.end() && item->second->enabled &&
        item->second->type != MenuItemType::Separator && !item->second->submenu) {
      session->clicked = true;
      Emit(item->second, MenuItemClickedEvent(id));
      if (event == "shortcut")
        RequestClose(session);
    }
  } else if (event == "submenu-open" && session->opened) {
    auto sub = session->submenus.find(id);
    if (sub != session->submenus.end() && !session->visible_submenus.count(id)) {
      session->visible_submenus[id] = sub->second;
      Emit(sub->second.second, MenuOpenedEvent(sub->second.second->id));
      Emit(sub->second.first, MenuItemSubmenuOpenedEvent(id));
    }
  } else if (event == "submenu-close") {
    auto sub = session->visible_submenus.find(id);
    if (sub != session->visible_submenus.end()) {
      auto pair = sub->second;
      session->visible_submenus.erase(sub);
      Emit(pair.second, MenuClosedEvent(pair.second->id));
      Emit(pair.first, MenuItemSubmenuClosedEvent(id));
    }
  }
  return nullptr;
}
bool RequestClose(const std::shared_ptr<Session>& session) {
  if (session->ended || session->closing)
    return false;
  bool accepted = false;
  ohos::WithEnvironment([&](napi_env env) {
    if (env == session->env)
      accepted = CallHost(session, "close", {ohos::Number(env, session->root->id)});
  });
  if (accepted)
    session->closing = true;
  return accepted;
}
bool Active(MenuId id) {
  for (auto& pair : sessions) {
    std::set<MenuId> visited;
    if (HasMenu(pair.second->root, id, visited))
      return true;
  }
  return false;
}
void Refresh() {
  if (!Main())
    return;
  ohos::RefreshTrayMenu();
  std::vector<std::shared_ptr<Session>> copy;
  for (auto& entry : sessions)
    copy.push_back(entry.second);
  for (auto& session : copy)
    if (!session->ended && !session->closing)
      ohos::WithEnvironment([&](napi_env env) {
        if (env != session->env)
          return;
        session->entries.clear();
        session->submenus.clear();
        std::set<MenuId> path;
        auto model = Snapshot(env, session->root, session, path);
        napi_value callback = nullptr;
        if (model && napi_get_reference_value(env, session->callback, &callback) == napi_ok)
          CallHost(session, "update", {model, callback});
      });
}
void RemoveHost(WindowId id, bool native_gone) {
  auto found = hosts.find(id);
  if (found == hosts.end())
    return;
  auto host = found->second;
  hosts.erase(found);
  if (host.closed_listener)
    WindowManager::GetInstance().RemoveListener(host.closed_listener);
  std::vector<std::shared_ptr<Session>> copy;
  for (auto& entry : sessions)
    if (entry.second->window == id)
      copy.push_back(entry.second);
  for (auto& session : copy) {
    RequestClose(session);
    if (native_gone)
      End(session, true);
  }
  napi_delete_reference(host.env, host.object);
}
void Cleanup(void* data) {
  auto env = static_cast<napi_env>(data);
  std::vector<std::shared_ptr<Session>> copy;
  for (auto& entry : sessions)
    if (entry.second->env == env)
      copy.push_back(entry.second);
  for (auto& session : copy)
    End(session, false);
  std::vector<WindowId> ids;
  for (auto& host : hosts)
    if (host.second.env == env)
      ids.push_back(host.first);
  for (auto id : ids)
    RemoveHost(id, false);
  environments.erase(env);
}
}  // namespace
namespace ohos {
bool DispatchTrayMenuItem(const std::shared_ptr<Menu>& root, MenuItemId id) {
  if (!Main())
    return false;
  std::set<MenuId> path;
  std::function<std::shared_ptr<ItemModel>(std::shared_ptr<MenuModel>)> find;
  find = [&](std::shared_ptr<MenuModel> menu) -> std::shared_ptr<ItemModel> {
    if (!menu || !path.insert(menu->id).second)
      return nullptr;
    for (auto& wrapper : menu->items) {
      auto item = Model(wrapper);
      if (!item || !item->enabled || item->type == MenuItemType::Separator)
        continue;
      if (item->id == id && !item->submenu)
        return item;
      if (auto child = find(Model(item->submenu)))
        return child;
    }
    return nullptr;
  };
  auto item = find(Model(root));
  if (!item)
    return false;
  Emit(item, MenuItemClickedEvent(id));
  return true;
}
bool AttachMenuHost(napi_env env, napi_value window, napi_value host) {
  if (!Main() || !IsInitialized())
    return false;
  auto wrapped = AttachWindow(env, window);
  auto state = wrapped ? FindWindowState(wrapped->GetNativeObject()) : nullptr;
  if (!state || state->env != env)
    return false;
  for (const char* name : {"show", "update", "close"}) {
    napi_value method;
    napi_valuetype type;
    if (!js::Property(env, host, name, method) || napi_typeof(env, method, &type) != napi_ok ||
        type != napi_function)
      return false;
  }
  napi_ref reference = nullptr;
  if (napi_create_reference(env, host, 1, &reference) != napi_ok)
    return false;
  if (!environments.count(env)) {
    if (napi_add_env_cleanup_hook(env, Cleanup, env) != napi_ok) {
      napi_delete_reference(env, reference);
      return false;
    }
    environments.insert(env);
  }
  RemoveHost(state->id, false);
  Host entry;
  entry.window = state;
  entry.env = env;
  entry.object = reference;
  entry.closed_listener = WindowManager::GetInstance().AddListener<WindowClosedEvent>(
      [id = state->id](const auto& event) {
        if (event.GetWindowId() == id)
          RemoveHost(id, true);
      });
  hosts.emplace(state->id, std::move(entry));
  return true;
}
}  // namespace ohos
class Menu::Impl {
 public:
  std::shared_ptr<MenuModel> model;
  bool borrowed = false;
};
class MenuItem::Impl {
 public:
  std::shared_ptr<ItemModel> model;
};
Menu::Menu() : Menu(nullptr) {}
Menu::Menu(void* native) : pimpl_(std::make_unique<Impl>()) {
  pimpl_->borrowed = native != nullptr;
  pimpl_->model = native ? Lookup(menus, native) : std::make_shared<MenuModel>();
  if (auto state = pimpl_->model) {
    menus[state.get()] = state;
    ++state->owners;
    state->sinks[this] = CreateGuardedCallback<const MenuEvent&>(
        Sink([this](const MenuEvent& event) { Emit(event); }));
  }
}
Menu::~Menu() {
  ShutdownEmitter();
  auto state = pimpl_->model;
  if (!state)
    return;
  state->sinks.erase(this);
  if (--state->owners == 0) {
    std::vector<std::shared_ptr<Session>> copy;
    for (auto& entry : sessions)
      if (entry.second->root == state)
        copy.push_back(entry.second);
    for (auto& session : copy) {
      RequestClose(session);
      End(session, false);
    }
    menus.erase(state.get());
  }
}
MenuId Menu::GetId() const {
  return pimpl_->model ? pimpl_->model->id : IdAllocator::kInvalidId;
}
bool Menu::SetBackend(MenuBackend backend) {
  return Main() && pimpl_->model && !pimpl_->borrowed && backend == MenuBackend::Native &&
         !Active(GetId());
}
MenuBackend Menu::GetBackend() const {
  return MenuBackend::Native;
}
bool Menu::IsBackendSupported(MenuBackend backend) {
  return backend == MenuBackend::Native;
}
void Menu::AddItem(std::shared_ptr<MenuItem> item) {
  InsertItem(GetItemCount(), std::move(item));
}
void Menu::InsertItem(size_t index, std::shared_ptr<MenuItem> item) {
  auto root = pimpl_->model;
  auto entry = Model(item);
  if (!Main() || !root || !entry)
    return;
  for (auto& existing : root->items)
    if (existing->GetId() == item->GetId())
      return;
  std::set<MenuId> visited;
  if (HasMenu(Model(entry->submenu), root->id, visited))
    return;
  index = std::min(index, root->items.size());
  root->items.insert(root->items.begin() + index, std::move(item));
  Refresh();
}
bool Menu::RemoveItem(std::shared_ptr<MenuItem> item) {
  return item && RemoveItemById(item->GetId());
}
bool Menu::RemoveItemById(MenuItemId id) {
  auto root = pimpl_->model;
  if (!Main() || !root)
    return false;
  for (size_t i = 0; i < root->items.size(); ++i)
    if (root->items[i]->GetId() == id)
      return RemoveItemAt(i);
  return false;
}
bool Menu::RemoveItemAt(size_t index) {
  auto root = pimpl_->model;
  if (!Main() || !root || index >= root->items.size())
    return false;
  root->items.erase(root->items.begin() + index);
  Refresh();
  return true;
}
void Menu::Clear() {
  if (Main() && pimpl_->model) {
    pimpl_->model->items.clear();
    Refresh();
  }
}
void Menu::AddSeparator() {
  AddItem(std::make_shared<MenuItem>("", MenuItemType::Separator));
}
void Menu::InsertSeparator(size_t index) {
  InsertItem(index, std::make_shared<MenuItem>("", MenuItemType::Separator));
}
size_t Menu::GetItemCount() const {
  return pimpl_->model ? pimpl_->model->items.size() : 0;
}
std::shared_ptr<MenuItem> Menu::GetItemAt(size_t index) const {
  return index < GetItemCount() ? pimpl_->model->items[index] : nullptr;
}
std::shared_ptr<MenuItem> Menu::GetItemById(MenuItemId id) const {
  for (auto& item : GetAllItems())
    if (item->GetId() == id)
      return item;
  return nullptr;
}
std::vector<std::shared_ptr<MenuItem>> Menu::GetAllItems() const {
  return pimpl_->model ? pimpl_->model->items : std::vector<std::shared_ptr<MenuItem>>{};
}
bool Menu::Open(const PositioningStrategy& strategy, Placement placement) {
  auto root = pimpl_->model;
  if (!Main() || !ohos::IsInitialized() || !root || root->items.empty() || Active(root->id) ||
      static_cast<int>(placement) < 0 || static_cast<int>(placement) > 11 ||
      strategy.GetType() == PositioningStrategy::Type::CursorPosition)
    return false;
  auto window = WindowManager::GetInstance().GetCurrent();
  if (strategy.GetRelativeWindow())
    window = WindowManager::GetInstance().Get(strategy.GetRelativeWindow()->GetId());
  if (!window || !hosts.count(window->GetId()))
    return false;
  Rectangle anchor{};
  if (strategy.GetType() == PositioningStrategy::Type::Absolute) {
    auto p = strategy.GetAbsolutePosition();
    anchor.x = p.x;
    anchor.y = p.y;
  } else {
    anchor = strategy.GetRelativeRectangle();
    auto p = strategy.GetRelativeOffset();
    anchor.x += p.x;
    anchor.y += p.y;
  }
  if (!std::isfinite(anchor.x) || !std::isfinite(anchor.y) || !std::isfinite(anchor.width) ||
      !std::isfinite(anchor.height) || anchor.width < 0 || anchor.height < 0)
    return false;
  auto session = std::make_shared<Session>();
  session->root = root;
  session->window = window->GetId();
  bool accepted = false;
  ohos::WithEnvironment([&](napi_env env) {
    session->env = env;
    auto host = hosts.find(session->window);
    napi_value provider = nullptr;
    if (host == hosts.end() || host->second.env != env ||
        napi_get_reference_value(env, host->second.object, &provider) != napi_ok ||
        napi_create_reference(env, provider, 1, &session->provider) != napi_ok)
      return;
    napi_value callback = nullptr;
    if (napi_create_function(env, "nativeapi.menuEvent", NAPI_AUTO_LENGTH, NativeEvent,
                             reinterpret_cast<void*>(session->token), &callback) != napi_ok ||
        napi_create_reference(env, callback, 1, &session->callback) != napi_ok)
      return;
    std::set<MenuId> path;
    auto model = Snapshot(env, root, session, path);
    if (!model)
      return;
    napi_value position;
    napi_create_object(env, &position);
    Number(env, position, "x", anchor.x);
    Number(env, position, "y", anchor.y);
    Number(env, position, "width", anchor.width);
    Number(env, position, "height", anchor.height);
    sessions[session->token] = session;
    accepted =
        CallHost(session, "show",
                 {model, position, ohos::Number(env, static_cast<int>(placement)), callback});
    if (!accepted)
      End(session, false);
  });
  return accepted;
}
bool Menu::Close() {
  if (!Main() || !pimpl_->model)
    return false;
  for (auto& entry : sessions)
    if (entry.second->root == pimpl_->model) {
      auto session = entry.second;
      return RequestClose(session);
    } else {
      for (auto& submenu : entry.second->visible_submenus)
        if (submenu.second.second == pimpl_->model) {
          auto session = entry.second;
          return RequestClose(session);
        }
    }
  return false;
}
void* Menu::GetNativeObjectInternal() const {
  return pimpl_->model.get();
}
MenuItem::MenuItem(const std::string& label, MenuItemType type)
    : MenuItem(static_cast<void*>(nullptr)) {
  auto state = pimpl_->model;
  state->type = type;
  if (!label.empty() && Valid(label))
    state->label = label;
}
MenuItem::MenuItem(void* native) : pimpl_(std::make_unique<Impl>()) {
  pimpl_->model = native ? Lookup(items, native) : std::make_shared<ItemModel>();
  if (auto state = pimpl_->model) {
    items[state.get()] = state;
    state->sinks[this] = CreateGuardedCallback<const MenuEvent&>(
        Sink([this](const MenuEvent& event) { Emit(event); }));
  }
}
MenuItem::~MenuItem() {
  ShutdownEmitter();
  if (auto state = pimpl_->model) {
    state->sinks.erase(this);
    if (state->sinks.empty())
      items.erase(state.get());
  }
}
MenuItemId MenuItem::GetId() const {
  return pimpl_->model ? pimpl_->model->id : IdAllocator::kInvalidId;
}
MenuItemType MenuItem::GetType() const {
  return pimpl_->model ? pimpl_->model->type : MenuItemType::Normal;
}
void MenuItem::SetLabel(const std::optional<std::string>& label) {
  if (Main() && pimpl_->model && Valid(label)) {
    pimpl_->model->label = label;
    Refresh();
  }
}
std::optional<std::string> MenuItem::GetLabel() const {
  return pimpl_->model ? pimpl_->model->label : std::nullopt;
}
void MenuItem::SetIcon(std::shared_ptr<Image> image) {
  if (!Main() || !pimpl_->model)
    return;
  auto data = image ? image->ToBase64() : "";
  if (image && data.empty())
    return;
  pimpl_->model->image = std::move(image);
  pimpl_->model->icon = std::move(data);
  Refresh();
}
std::shared_ptr<Image> MenuItem::GetIcon() const {
  return pimpl_->model ? pimpl_->model->image : nullptr;
}
void MenuItem::SetTooltip(const std::optional<std::string>& tooltip) {
  if (Main() && pimpl_->model && Valid(tooltip)) {
    pimpl_->model->tooltip = tooltip;
    Refresh();
  }
}
std::optional<std::string> MenuItem::GetTooltip() const {
  return pimpl_->model ? pimpl_->model->tooltip : std::nullopt;
}
void MenuItem::SetAccelerator(const std::optional<KeyboardAccelerator>& value) {
  if (Main() && pimpl_->model && (!value || Valid(value->key))) {
    pimpl_->model->accelerator = value.value_or(KeyboardAccelerator{});
    Refresh();
  }
}
KeyboardAccelerator MenuItem::GetAccelerator() const {
  return pimpl_->model ? pimpl_->model->accelerator : KeyboardAccelerator{};
}
void MenuItem::SetEnabled(bool value) {
  if (Main() && pimpl_->model) {
    pimpl_->model->enabled = value;
    Refresh();
  }
}
bool MenuItem::IsEnabled() const {
  return pimpl_->model && pimpl_->model->enabled;
}
void MenuItem::SetState(MenuItemState value) {
  auto state = pimpl_->model;
  if (!Main() || !state ||
      (state->type != MenuItemType::Checkbox && state->type != MenuItemType::Radio) ||
      static_cast<int>(value) < 0 || static_cast<int>(value) > 2 ||
      (state->type == MenuItemType::Radio && value == MenuItemState::Mixed))
    return;
  if (state->type == MenuItemType::Radio && state->radio_group >= 0 &&
      value == MenuItemState::Checked)
    for (auto& entry : items)
      if (auto other = entry.second.lock())
        if (other->type == MenuItemType::Radio && other->radio_group == state->radio_group)
          other->state = MenuItemState::Unchecked;
  state->state = value;
  Refresh();
}
MenuItemState MenuItem::GetState() const {
  return pimpl_->model ? pimpl_->model->state : MenuItemState::Unchecked;
}
void MenuItem::SetRadioGroup(int group) {
  if (Main() && pimpl_->model && pimpl_->model->type == MenuItemType::Radio) {
    pimpl_->model->radio_group = group;
    if (GetState() == MenuItemState::Checked)
      SetState(MenuItemState::Checked);
  }
}
int MenuItem::GetRadioGroup() const {
  return pimpl_->model && pimpl_->model->type == MenuItemType::Radio ? pimpl_->model->radio_group
                                                                     : -1;
}
void MenuItem::SetSubmenu(std::shared_ptr<Menu> menu) {
  auto state = pimpl_->model;
  if (!Main() || !state)
    return;
  auto child = Model(menu);
  if (menu && !child)
    return;
  std::set<MenuId> visited;
  if (HasItem(child, state->id, visited))
    return;
  state->submenu = std::move(menu);
  if (state->submenu)
    state->type = MenuItemType::Submenu;
  else if (state->type == MenuItemType::Submenu)
    state->type = MenuItemType::Normal;
  Refresh();
}
std::shared_ptr<Menu> MenuItem::GetSubmenu() const {
  return pimpl_->model ? pimpl_->model->submenu : nullptr;
}
void* MenuItem::GetNativeObjectInternal() const {
  return pimpl_->model.get();
}
}  // namespace nativeapi
