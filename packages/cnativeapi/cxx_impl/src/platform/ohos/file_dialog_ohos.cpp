#include <condition_variable>
#include <mutex>
#include "../../file_dialog_impl.h"
#include "../../foundation/dispatcher.h"
#include "../../window_manager.h"
#include "ohos_arkui.h"
#include "ohos_napi.h"
#include "ohos_runtime.h"
#include "ohos_window.h"

namespace nativeapi {
namespace {
napi_value Text(napi_env env, const std::string& text) {
  napi_value value = nullptr;
  napi_create_string_utf8(env, text.data(), text.size(), &value);
  return value;
}
bool Set(napi_env env, napi_value object, const char* name, napi_value value) {
  return value && napi_set_named_property(env, object, name, value) == napi_ok;
}
napi_value Strings(napi_env env, const std::vector<std::string>& values) {
  napi_value array = nullptr;
  if (napi_create_array_with_length(env, values.size(), &array) != napi_ok)
    return nullptr;
  for (size_t i = 0; i < values.size(); ++i)
    if (napi_set_element(env, array, i, Text(env, values[i])) != napi_ok)
      return nullptr;
  return array;
}
struct PickSession : std::enable_shared_from_this<PickSession> {
  std::mutex mutex;
  std::condition_variable changed;
  bool done = false;
  FileDialogResult result = FileDialogResult::Failed;
  std::vector<std::string> paths;
  std::string error;
  napi_env env = nullptr;
  napi_ref picker = nullptr;
  bool cleanup_hook = false;
  bool multiple = false;

  static void Cleanup(void* data) {
    auto held = static_cast<PickSession*>(data)->shared_from_this();
    held->cleanup_hook = false;
    held->Finish(FileDialogResult::Failed, {}, "Picker UI environment was destroyed");
  }
  void Finish(FileDialogResult value, std::vector<std::string> selected, std::string failure) {
    // Called on the owning UI thread, before waking the worker. Late SDK results
    // retain this session but never reference a FileDialog or its PIMPL.
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (done)
        return;
    }
    if (cleanup_hook) {
      cleanup_hook = false;
      napi_remove_env_cleanup_hook(env, Cleanup, this);
    }
    if (picker) {
      auto reference = picker;
      picker = nullptr;
      napi_delete_reference(env, reference);
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      result = value;
      paths = std::move(selected);
      error = std::move(failure);
      done = true;
    }
    changed.notify_all();
  }
  static napi_value Completed(napi_env env, napi_callback_info info) {
    napi_value args[2]{}, ignored = nullptr;
    size_t count = 2;
    void* data = nullptr;
    napi_get_cb_info(env, info, &count, args, nullptr, &data);
    auto self = *static_cast<std::shared_ptr<PickSession>*>(data);
    try {
      napi_valuetype type = napi_undefined;
      napi_value null_value = nullptr;
      bool is_null = false;
      napi_get_null(env, &null_value);
      if (count && args[0]) {
        napi_typeof(env, args[0], &type);
        napi_strict_equals(env, args[0], null_value, &is_null);
      }
      double numeric_error = -1;
      const bool numeric_success = type == napi_number &&
                                   napi_get_value_double(env, args[0], &numeric_error) == napi_ok &&
                                   numeric_error == 0;
      // API 26's native bridge uses numeric zero for success, while the public
      // AsyncCallback type also permits the usual null/undefined error value.
      bool empty_array = false;
      uint32_t returned_count = 0;
      if (count >= 2 && napi_is_array(env, args[1], &empty_array) == napi_ok && empty_array)
        empty_array =
            napi_get_array_length(env, args[1], &returned_count) == napi_ok && returned_count == 0;
      if (type == napi_number && numeric_error == -1 && empty_array) {
        // Native DocumentViewPicker reports user dismissal as status -1 with
        // an empty URI array, rather than a BusinessError object.
        self->Finish(FileDialogResult::Cancelled, {}, {});
      } else if (type != napi_undefined && !is_null && !numeric_success) {
        napi_value code_value = nullptr;
        int32_t code = 0;
        if (type == napi_number)
          napi_get_value_int32(env, args[0], &code);
        else if (ohos::js::Property(env, args[0], "code", code_value))
          napi_get_value_int32(env, code_value, &code);
        auto message = ohos::js::StringProperty(env, args[0], "message");
        self->Finish(FileDialogResult::Failed, {},
                     "System document picker failed (" + std::to_string(code) + "): " + message);
      } else {
        bool array = false;
        uint32_t length = 0;
        if (count < 2 || napi_is_array(env, args[1], &array) != napi_ok || !array ||
            napi_get_array_length(env, args[1], &length) != napi_ok ||
            (!self->multiple && length > 1)) {
          self->Finish(FileDialogResult::Failed, {}, "Invalid system picker result");
        } else {
          std::vector<std::string> selected;
          bool valid = true;
          for (uint32_t i = 0; i < length; ++i) {
            napi_value value = nullptr;
            if (napi_get_element(env, args[1], i, &value) != napi_ok) {
              valid = false;
              break;
            }
            auto uri = ohos::js::String(env, value);
            if (uri.empty() || uri.find('\0') != uri.npos) {
              valid = false;
              break;
            }
            selected.push_back(std::move(uri));
          }
          if (!valid)
            self->Finish(FileDialogResult::Failed, {}, "Invalid document URI from system picker");
          else {
            const auto result =
                selected.empty() ? FileDialogResult::Cancelled : FileDialogResult::Accepted;
            self->Finish(result, std::move(selected), {});
          }
        }
      }
    } catch (...) {
      self->Finish(FileDialogResult::Failed, {}, "Could not process the system picker result");
    }
    napi_get_undefined(env, &ignored);
    return ignored;
  }
  bool Start(napi_env environment,
             napi_value context,
             napi_value window,
             FileDialogMode mode,
             const std::vector<std::string>& extensions,
             std::string name) {
    env = environment;
    multiple = mode == FileDialogMode::OpenFiles;
    napi_value module = nullptr, constructor = nullptr, instance = nullptr, options = nullptr;
    napi_value callback = nullptr, returned = nullptr;
    if (!ohos::js::Module(env, "@ohos.file.picker", module) ||
        !ohos::js::Property(env, module, "DocumentViewPicker", constructor))
      return false;
    napi_value arguments[] = {context, window};
    if (napi_new_instance(env, constructor, 2, arguments, &instance) != napi_ok ||
        napi_create_object(env, &options) != napi_ok ||
        napi_create_reference(env, instance, 1, &picker) != napi_ok) {
      ohos::js::ClearException(env);
      return false;
    }
    bool configured = true;
    if (mode == FileDialogMode::SaveFile) {
      if (name.empty())
        name = "Untitled";
      if (!extensions.empty() && name.find('.') == name.npos)
        name += extensions.front();
      configured &= Set(env, options, "newFileNames", Strings(env, {name}));
      if (!extensions.empty())
        configured &= Set(env, options, "fileSuffixChoices", Strings(env, extensions));
    } else {
      // Leave the multiple-selection limit to the system provider.
      if (!multiple)
        configured &= Set(env, options, "maxSelectNumber", ohos::js::Number(env, 1));
      configured &= Set(env, options, "selectMode",
                        ohos::js::Number(env, mode == FileDialogMode::SelectFolder ? 1 : 0));
      if (!extensions.empty() && mode != FileDialogMode::SelectFolder) {
        std::string filter;
        for (const auto& ext : extensions) {
          if (ext == "*") {
            filter = ".*";
            break;
          }
          if (!filter.empty())
            filter += ',';
          filter += ext;
        }
        // The SDK array represents alternative filter choices. One comma-joined
        // choice preserves nativeapi's union of allowed file extensions.
        configured &= Set(env, options, "fileSuffixFilters", Strings(env, {filter}));
      }
    }
    if (!configured || napi_add_env_cleanup_hook(env, Cleanup, this) != napi_ok)
      return false;
    cleanup_hook = true;
    auto* holder = new std::shared_ptr<PickSession>(shared_from_this());
    if (napi_create_function(env, "nativeapi.filePicker", NAPI_AUTO_LENGTH, Completed, holder,
                             &callback) != napi_ok) {
      delete holder;
      return false;
    }
    if (napi_add_finalizer(
            env, callback, holder,
            [](napi_env, void* data, void*) {
              delete static_cast<std::shared_ptr<PickSession>*>(data);
            },
            nullptr, nullptr) != napi_ok) {
      delete holder;
      return false;
    }
    return ohos::js::Call(env, instance, mode == FileDialogMode::SaveFile ? "save" : "select",
                          {options, callback}, returned);
  }
};
}  // namespace
bool FileDialog::IsSupported() {
  bool supported = false;
  ohos::WithContext([&](napi_env env, napi_value) {
    napi_value module = nullptr, constructor = nullptr;
    napi_valuetype type;
    supported = ohos::js::Module(env, "@ohos.file.picker", module) &&
                ohos::js::Property(env, module, "DocumentViewPicker", constructor) &&
                napi_typeof(env, constructor, &type) == napi_ok && type == napi_function;
  });
  return supported;
}
FileDialog::Impl::~Impl() = default;
bool FileDialog::Impl::Open() {
  if (IsMainThread()) {
    error = "Open the system picker from a worker; the ArkTS UI thread must remain responsive";
    result = FileDialogResult::Failed;
    return false;
  }
  for (const auto& extension : extensions) {
    if (extension.find(',') != extension.npos) {
      error = "The system picker cannot represent a literal comma inside a file extension";
      result = FileDialogResult::Failed;
      return false;
    }
  }
  auto session = std::make_shared<PickSession>();
  bool submitted = false;
  const bool dispatched = ohos::RunSync([&] {
    auto owner = parent ? parent : WindowManager::GetInstance().GetCurrent();
    auto state = owner ? ohos::FindWindowState(owner->GetNativeObject()) : nullptr;
    if (state && owner->IsVisible()) {
      ohos::WithUiContext(owner, [&] {
        ohos::WithContext([&](napi_env env, napi_value context) {
          submitted = session->Start(env, context, ohos::WindowObject(state), mode, extensions,
                                     suggested_name);
        });
      });
    }
    if (!submitted)
      session->Finish(
          FileDialogResult::Failed, {},
          "Cannot start system picker: missing host/context, invalid parent or SDK failure");
  });
  if (!dispatched) {
    error = "The ArkTS UI host is not initialized or is shutting down";
    result = FileDialogResult::Failed;
    return false;
  }
  std::unique_lock<std::mutex> lock(session->mutex);
  session->changed.wait(lock, [&] { return session->done; });
  result = session->result;
  paths = session->paths;
  error = session->error;
  return result == FileDialogResult::Accepted || result == FileDialogResult::Cancelled;
}
bool FileDialog::Impl::Close() {
  // DocumentViewPicker exposes no public cancellation operation. Do not resolve
  // the worker as cancelled while a live system picker remains on screen.
  return false;
}
}  // namespace nativeapi
