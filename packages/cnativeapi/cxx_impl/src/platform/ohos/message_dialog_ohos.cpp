#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <vector>
#include "../../foundation/dispatcher.h"
#include "../../message_dialog.h"
#include "../../window_manager.h"
#include "../message_dialog_state.h"
#include "ohos_arkui.h"
#include "ohos_runtime.h"

namespace nativeapi {
namespace {
bool TextValid(const std::string& value) {
  return value.find('\0') == value.npos;
}
struct Waiter {
  std::mutex mutex;
  std::condition_variable changed;
  bool done = false, success = false;
  void Finish(bool ok) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      done = true;
      success = ok;
    }
    changed.notify_all();
  }
};
struct DialogModel;
// SDK callbacks receive an opaque integer token, never a dereferenceable C++
// pointer. Late callbacks after disposal cannot access a destroyed model.
std::map<uintptr_t, std::weak_ptr<DialogModel>> callbacks;
uintptr_t next_token = 0;
napi_env cleanup_environment = nullptr;
void CleanupDialogs(void*);
struct BlockedWindow {
  std::shared_ptr<Window> window;
  bool focusable, ignore_mouse;
  size_t leases = 1;
};
std::map<WindowId, BlockedWindow> blocked_windows;
struct DialogModel : std::enable_shared_from_this<DialogModel> {
  MessageDialogState state;
  std::string title, message;
  ArkUI_NativeDialogHandle controller = nullptr;
  std::vector<ArkUI_NodeHandle> nodes;
  ArkUI_NodeHandle root = nullptr, title_node = nullptr, body = nullptr, input = nullptr,
                   check_row = nullptr, checkbox = nullptr, check_label = nullptr,
                   progress = nullptr, loading = nullptr;
  std::shared_ptr<DialogModel> keep_alive;
  std::shared_ptr<Waiter> waiter;
  std::shared_ptr<Window> parent;
  std::vector<WindowId> blocked;
  size_t parent_listener = 0, created_listener = 0;
  uintptr_t token = 0;
  bool closing = false, shutting_down = false;

  static std::shared_ptr<DialogModel> Find(void* data) {
    const auto found = callbacks.find(reinterpret_cast<uintptr_t>(data));
    return found == callbacks.end() ? nullptr : found->second.lock();
  }
  void* Token() const { return reinterpret_cast<void*>(token); }
  ArkUI_NodeHandle Node(ArkUI_NodeType type, ArkUI_NodeHandle owner = nullptr) {
    auto node = ohos::NodeApi()->createNode(type);
    if (!node)
      return nullptr;
    nodes.push_back(node);
    if (owner && ohos::NodeApi()->addChild(owner, node) != 0)
      return nullptr;
    return node;
  }
  static void Event(ArkUI_NodeEvent* event) {
    auto self = Find(OH_ArkUI_NodeEvent_GetUserData(event));
    if (!self || !self->state.open || self->closing)
      return;
    const auto target = OH_ArkUI_NodeEvent_GetTargetId(event);
    if (target >= 1 && target <= 3) {
      self->Close(static_cast<MessageDialogResult>(target));
    } else if (target == 4) {
      self->state.input = ohos::NodeTextValue(self->input, NODE_TEXT_INPUT_TEXT);
    } else if (target == 5) {
      auto* value = ohos::NodeApi()->getAttribute(self->checkbox, NODE_CHECKBOX_SELECT);
      if (value && value->value && value->size)
        self->state.checked = value->value[0].i32 != 0;
    }
  }
  bool Listen(ArkUI_NodeHandle node, ArkUI_NodeEventType type, int target) {
    return node && ohos::NodeApi()->addNodeEventReceiver(node, Event) == 0 &&
           ohos::NodeApi()->registerNodeEvent(node, type, target, Token()) == 0;
  }
  bool Block(const std::shared_ptr<Window>& window) {
    if (!window || !window->GetNativeObject() || window->GetId() == parent->GetId())
      return true;
    const auto id = window->GetId();
    if (std::find(blocked.begin(), blocked.end(), id) != blocked.end())
      return true;
    auto found = blocked_windows.find(id);
    if (found != blocked_windows.end()) {
      ++found->second.leases;
      blocked.push_back(id);
      return true;
    }
    BlockedWindow saved{window, window->IsFocusable(), window->IsIgnoreMouseEvents()};
    window->SetFocusable(false);
    if (window->IsFocusable() || !window->SetIgnoreMouseEvents(true, false)) {
      window->SetFocusable(saved.focusable);
      return false;
    }
    blocked_windows.emplace(id, std::move(saved));
    blocked.push_back(id);
    return true;
  }
  void Unblock() {
    if (created_listener)
      WindowManager::GetInstance().RemoveListener(created_listener);
    created_listener = 0;
    for (auto id : blocked) {
      auto found = blocked_windows.find(id);
      if (found == blocked_windows.end() || --found->second.leases)
        continue;
      auto saved = std::move(found->second);
      blocked_windows.erase(found);
      if (saved.window->GetNativeObject()) {
        saved.window->SetFocusable(saved.focusable);
        saved.window->SetIgnoreMouseEvents(saved.ignore_mouse, false);
      }
    }
    blocked.clear();
  }
  void Release() {
    callbacks.erase(token);
    token = 0;
    shutting_down = true;
    if (parent && parent_listener)
      parent->RemoveListener(parent_listener);
    parent_listener = 0;
    Unblock();
    if (controller) {
      auto* api = ohos::DialogApi();
      if (state.open)
        api->nativeDialogAPI1.close(controller);
      api->nativeDialogAPI1.removeContent(controller);
      api->nativeDialogAPI1.dispose(controller);
      controller = nullptr;
    }
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
      ohos::NodeApi()->removeNodeEventReceiver(*it, Event);
      ohos::NodeApi()->disposeNode(*it);
    }
    nodes.clear();
    root = title_node = body = input = check_row = checkbox = check_label = progress = loading =
        nullptr;
    state.open = false;
    parent.reset();
    keep_alive.reset();
  }
  void Abort() {
    auto held = shared_from_this();
    if (waiter)
      waiter->Finish(false);
    if (state.open)
      state.result = MessageDialogResult::Close;
    Release();
  }
  bool RefreshInput() {
    return !input || (ohos::NodeText(input, NODE_TEXT_INPUT_TEXT, state.input) &&
                      ohos::NodeInt(input, NODE_VISIBILITY, state.input_enabled ? 0 : 2));
  }
  bool RefreshCheckbox() {
    return !check_row ||
           (ohos::NodeText(check_label, NODE_TEXT_CONTENT, state.checkbox) &&
            ohos::NodeInt(checkbox, NODE_CHECKBOX_SELECT, state.checked) &&
            ohos::NodeInt(check_row, NODE_VISIBILITY, state.checkbox.empty() ? 2 : 0));
  }
  bool RefreshProgress() {
    if (!progress)
      return true;
    bool ok = ohos::NodeInt(progress, NODE_VISIBILITY, state.progress >= 0 ? 0 : 2);
    ok &= ohos::NodeInt(loading, NODE_VISIBILITY, state.progress == -1 ? 0 : 2);
    if (state.progress >= 0)
      ok &= ohos::NodeFloat(progress, NODE_PROGRESS_VALUE, state.progress * 100);
    return ok;
  }
  bool Refresh() {
    return !root || (ohos::NodeText(title_node, NODE_TEXT_CONTENT, title) &&
                     ohos::NodeText(body, NODE_TEXT_CONTENT, message) && RefreshInput() &&
                     RefreshCheckbox() && RefreshProgress());
  }
  bool Close(MessageDialogResult result) {
    if (!state.open || closing || !controller)
      return false;
    state.result = result;
    closing = true;
    if (ohos::DialogApi()->nativeDialogAPI1.close(controller) == 0)
      return true;
    closing = false;
    state.result = MessageDialogResult::None;
    return false;
  }
  bool Open(DialogModality mode, const std::shared_ptr<Waiter>& completion) {
    if (state.open || controller || !ohos::NodeApi() || !ohos::DialogApi())
      return false;
    parent = state.parent ? state.parent : WindowManager::GetInstance().GetCurrent();
    if (!parent || !parent->GetNativeObject() || !parent->IsVisible() ||
        blocked_windows.count(parent->GetId()))
      return false;
    bool cleanup_ready = false;
    ohos::WithEnvironment([&](napi_env env) {
      if (!cleanup_environment &&
          napi_add_env_cleanup_hook(env, CleanupDialogs, nullptr) == napi_ok)
        cleanup_environment = env;
      cleanup_ready = cleanup_environment == env;
    });
    if (!cleanup_ready)
      return false;
    waiter = completion;
    token = ++next_token;
    callbacks.emplace(token, weak_from_this());
    keep_alive = shared_from_this();
    closing = shutting_down = false;
    state.result = MessageDialogResult::None;
    auto* api = ohos::DialogApi();
    controller = api->nativeDialogAPI1.create();
    root = Node(ARKUI_NODE_COLUMN);
    title_node = Node(ARKUI_NODE_TEXT, root);
    body = Node(ARKUI_NODE_TEXT, root);
    input = Node(ARKUI_NODE_TEXT_INPUT, root);
    check_row = Node(ARKUI_NODE_ROW, root);
    checkbox = Node(ARKUI_NODE_CHECKBOX, check_row);
    check_label = Node(ARKUI_NODE_TEXT, check_row);
    progress = Node(ARKUI_NODE_PROGRESS, root);
    loading = Node(ARKUI_NODE_LOADING_PROGRESS, root);
    auto buttons = Node(ARKUI_NODE_COLUMN, root);
    bool ok = controller && root && title_node && body && input && check_row && checkbox &&
              check_label && progress && loading && buttons;
    if (ok) {
      ok &= ohos::NodeFloat(root, NODE_WIDTH, 320);
      ok &= ohos::NodeFloat(root, NODE_PADDING, 20);
      ohos::NodeFloat(title_node, NODE_FONT_SIZE, 22);
      ohos::NodeFloat(body, NODE_FONT_SIZE, 16);
      ohos::NodeFloat(input, NODE_WIDTH, 280);
      ohos::NodeFloat(input, NODE_HEIGHT, 44);
      ohos::NodeFloat(progress, NODE_WIDTH, 280);
      ohos::NodeFloat(loading, NODE_WIDTH, 32);
      ohos::NodeFloat(loading, NODE_HEIGHT, 32);
      ohos::NodeFloat(progress, NODE_PROGRESS_TOTAL, 100);
      ohos::NodeFloat(buttons, NODE_WIDTH, 280);
      ok &= Listen(input, NODE_TEXT_INPUT_ON_CHANGE, 4);
      ok &= Listen(checkbox, NODE_CHECKBOX_EVENT_ON_CHANGE, 5);
      const std::pair<MessageDialogResult, std::string> labels[] = {
          {MessageDialogResult::Primary, state.primary},
          {MessageDialogResult::Secondary, state.secondary},
          {MessageDialogResult::Close, state.close}};
      for (const auto& entry : labels) {
        if (entry.second.empty())
          continue;
        auto button = Node(ARKUI_NODE_BUTTON, buttons);
        ok &= ohos::NodeText(button, NODE_BUTTON_LABEL, entry.second);
        ohos::NodeFloat(button, NODE_MARGIN, 4);
        ohos::NodeFloat(button, NODE_WIDTH, 272);
        ohos::NodeInt(button, NODE_DEFAULT_FOCUS, state.default_button == entry.first);
        ok &= Listen(button, NODE_ON_CLICK, static_cast<int>(entry.first));
      }
      ok &= Refresh();
      ok &= api->nativeDialogAPI1.setContent(controller, root) == 0;
      ok &= api->nativeDialogAPI1.setModalMode(controller, mode != DialogModality::None) == 0;
      ok &= api->nativeDialogAPI1.setAutoCancel(controller, false) == 0;
      api->nativeDialogAPI1.setBackgroundColor(controller, 0xfff6f6f6);
      ok &= api->nativeDialogAPI1.registerOnWillDismissWithUserData(
                controller, Token(), [](ArkUI_DialogDismissEvent* event) {
                  if (auto self = Find(OH_ArkUI_DialogDismissEvent_GetUserData(event))) {
                    self->state.result = MessageDialogResult::Close;
                    self->closing = true;
                  }
                  OH_ArkUI_DialogDismissEvent_SetShouldBlockDismiss(event, false);
                }) == 0;

      ok &= api->registerOnDidDisappear(controller, Token(), [](void* token) {
        auto self = Find(token);
        if (!self || self->shutting_down)
          return;
        self->state.open = false;
        if (self->state.result == MessageDialogResult::None)
          self->state.result = MessageDialogResult::Close;
        self->Unblock();
        if (self->waiter)
          self->waiter->Finish(true);
        if (!ohos::Post([self] { self->Release(); }))
          self->Release();
      }) == 0;
    }
    if (!ok) {
      Release();
      return false;
    }
    state.open = true;
    if (api->nativeDialogAPI1.show(controller, false) != 0) {
      Release();
      return false;
    }
    parent_listener =
        parent->AddListener<WindowClosedEvent>([weak = weak_from_this()](const auto&) {
          if (auto self = weak.lock())
            self->Abort();
        });
    if (mode == DialogModality::Application) {
      for (auto& window : WindowManager::GetInstance().GetAll()) {
        if (!Block(window)) {
          Abort();
          return false;
        }
      }
      created_listener = WindowManager::GetInstance().AddListener<WindowCreatedEvent>(
          [weak = weak_from_this()](const auto& event) {
            if (auto self = weak.lock()) {
              if (!self->Block(WindowManager::GetInstance().Get(event.GetWindowId())))
                self->Abort();
            }
          });
    }
    return true;
  }
};
void CleanupDialogs(void*) {
  std::vector<std::shared_ptr<DialogModel>> models;
  for (const auto& item : callbacks)
    if (auto model = item.second.lock())
      models.push_back(model);
  for (auto& model : models)
    model->Abort();
  callbacks.clear();
  cleanup_environment = nullptr;
}
}  // namespace

class MessageDialog::Impl {
 public:
  std::shared_ptr<DialogModel> model = std::make_shared<DialogModel>();
  Impl(const std::string& title, const std::string& message) {
    model->title = title;
    model->message = message;
  }
  ~Impl() {
    auto held = model;
    ohos::RunSync([held] {
      if (held->controller)
        held->Abort();
    });
  }
};
MessageDialog::MessageDialog(const std::string& title, const std::string& message)
    : pimpl_(std::make_unique<Impl>(title, message)) {
  modality_ = DialogModality::None;
}
MessageDialog::~MessageDialog() = default;
bool MessageDialog::IsExtendedSupported() {
  bool supported = false;
  ohos::WithEnvironment([&](napi_env) { supported = ohos::NodeApi() && ohos::DialogApi(); });
  return supported;
}
void MessageDialog::SetTitle(const std::string& text) {
  if (TextValid(text))
    ohos::RunSync([&] {
      auto& model = *pimpl_->model;
      model.title = text;
      if (model.title_node)
        ohos::NodeText(model.title_node, NODE_TEXT_CONTENT, text);
    });
}
std::string MessageDialog::GetTitle() const {
  std::string text;
  ohos::RunSync([&] { text = pimpl_->model->title; });
  return text;
}
void MessageDialog::SetMessage(const std::string& text) {
  if (TextValid(text))
    ohos::RunSync([&] {
      auto& model = *pimpl_->model;
      model.message = text;
      if (model.body)
        ohos::NodeText(model.body, NODE_TEXT_CONTENT, text);
    });
}
std::string MessageDialog::GetMessage() const {
  std::string text;
  ohos::RunSync([&] { text = pimpl_->model->message; });
  return text;
}
void MessageDialog::SetModality(DialogModality mode) {
  modality_ = mode;
}
DialogModality MessageDialog::GetModality() const {
  return modality_;
}
bool MessageDialog::Open() {
  const auto mode = modality_;
  if (mode < DialogModality::None || mode > DialogModality::Window ||
      (mode != DialogModality::None && IsMainThread()))
    return false;
  const auto model = pimpl_->model;
  const auto waiter = std::make_shared<Waiter>();
  bool opened = false;
  ohos::RunSync([&] {
    if (!TextValid(model->title) || !TextValid(model->message))
      return;
    ohos::WithUiContext(model->state.parent, [&] { opened = model->Open(mode, waiter); });
  });
  if (!opened || mode == DialogModality::None)
    return opened;
  std::unique_lock<std::mutex> lock(waiter->mutex);
  waiter->changed.wait(lock, [&] { return waiter->done; });
  return waiter->success;
}
bool MessageDialog::Close() {
  bool ok = false;
  ohos::RunSync([&] { ok = pimpl_->model->Close(MessageDialogResult::Close); });
  return ok;
}
bool MessageDialog::SetButtons(const std::string& a, const std::string& b, const std::string& c) {
  if (!TextValid(a) || !TextValid(b) || !TextValid(c) || (a.empty() && b.empty() && c.empty()))
    return false;
  bool ok = false;
  ohos::RunSync([&] {
    auto& s = pimpl_->model->state;
    if (!s.open) {
      s.primary = a;
      s.secondary = b;
      s.close = c;
      ok = true;
    }
  });
  return ok;
}
bool MessageDialog::SetDefaultButton(MessageDialogResult button) {
  if (button < MessageDialogResult::None || button > MessageDialogResult::Close)
    return false;
  bool ok = false;
  ohos::RunSync([&] {
    if (!pimpl_->model->state.open) {
      pimpl_->model->state.default_button = button;
      ok = true;
    }
  });
  return ok;
}
bool MessageDialog::SetParentWindow(std::shared_ptr<Window> parent) {
  bool ok = false;
  ohos::RunSync([&] {
    if (!pimpl_->model->state.open && (!parent || parent->GetNativeObject())) {
      pimpl_->model->state.parent = parent;
      ok = true;
    }
  });
  return ok;
}
MessageDialogResult MessageDialog::GetResult() const {
  MessageDialogResult value = MessageDialogResult::None;
  ohos::RunSync([&] { value = pimpl_->model->state.result; });
  return value;
}
bool MessageDialog::IsOpen() const {
  bool value = false;
  ohos::RunSync([&] { value = pimpl_->model->state.open; });
  return value;
}
bool MessageDialog::SetInputEnabled(bool enabled) {
  bool ok = false;
  ohos::RunSync([&] {
    auto& model = *pimpl_->model;
    model.state.input_enabled = enabled;
    ok = !model.input || ohos::NodeInt(model.input, NODE_VISIBILITY, enabled ? 0 : 2);
  });
  return ok;
}
bool MessageDialog::SetInputText(const std::string& text) {
  if (!TextValid(text))
    return false;
  bool ok = false;
  ohos::RunSync([&] {
    auto& model = *pimpl_->model;
    model.state.input = text;
    ok = !model.input || ohos::NodeText(model.input, NODE_TEXT_INPUT_TEXT, text);
  });
  return ok;
}
std::string MessageDialog::GetInputText() const {
  std::string value;
  ohos::RunSync([&] {
    auto& m = *pimpl_->model;
    if (m.input)
      m.state.input = ohos::NodeTextValue(m.input, NODE_TEXT_INPUT_TEXT);
    value = m.state.input;
  });
  return value;
}
bool MessageDialog::SetCheckbox(const std::string& label, bool checked) {
  if (!TextValid(label))
    return false;
  bool ok = false;
  ohos::RunSync([&] {
    auto& m = *pimpl_->model;
    m.state.checkbox = label;
    m.state.checked = checked;
    ok = m.RefreshCheckbox();
  });
  return ok;
}
bool MessageDialog::IsCheckboxChecked() const {
  bool value = false;
  ohos::RunSync([&] {
    auto& m = *pimpl_->model;
    if (m.checkbox) {
      auto* a = ohos::NodeApi()->getAttribute(m.checkbox, NODE_CHECKBOX_SELECT);
      if (a && a->value && a->size)
        m.state.checked = a->value[0].i32 != 0;
    }
    value = m.state.checked;
  });
  return value;
}
bool MessageDialog::SetProgress(double progress) {
  if (!(progress == -2 || progress == -1 || (progress >= 0 && progress <= 1)))
    return false;
  bool ok = false;
  ohos::RunSync([&] {
    pimpl_->model->state.progress = progress;
    ok = pimpl_->model->RefreshProgress();
  });
  return ok;
}
}  // namespace nativeapi
