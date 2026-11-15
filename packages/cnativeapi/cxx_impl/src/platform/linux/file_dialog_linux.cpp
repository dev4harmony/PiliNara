#include <gtk/gtk.h>
#include <cctype>
#include "../../file_dialog_impl.h"

namespace nativeapi {
namespace {
// GTK globs are case-sensitive; ".txt" must still match "README.TXT".
std::string CaseInsensitiveGlob(const std::string& extension) {
  if (extension == "*") return "*";
  std::string glob = "*";
  for (unsigned char c : extension) {
    if (std::isalpha(c)) {
      glob += '[';
      glob += static_cast<char>(std::tolower(c));
      glob += static_cast<char>(std::toupper(c));
      glob += ']';
    } else {
      glob += static_cast<char>(c);
    }
  }
  return glob;
}
GtkWindow* ParentToplevel(const std::shared_ptr<Window>& parent) {
  auto* widget = parent ? static_cast<GtkWidget*>(parent->GetNativeObject()) : nullptr;
  if (!widget || !GTK_IS_WIDGET(widget)) return nullptr;
  GtkWidget* toplevel = gtk_widget_get_toplevel(widget);
  return GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : nullptr;
}
struct Session {
  GMainLoop* loop = nullptr;
  gint response = GTK_RESPONSE_NONE;
  bool done = false;
};
void OnResponse(GtkNativeDialog*, gint response, gpointer data) {
  auto* session = static_cast<Session*>(data);
  session->response = response;
  session->done = true;
  g_main_loop_quit(session->loop);
}
}  // namespace

// GtkFileChooserNative goes through xdg-desktop-portal when GTK picks it
// (Flatpak, GTK_USE_PORTAL=1) and exports the parent's Wayland/X11 handle for
// it; otherwise it shows an in-process GtkFileChooserDialog transient for the
// parent toplevel.
bool FileDialog::IsSupported() { return true; }
FileDialog::Impl::~Impl() = default;

bool FileDialog::Impl::Close() { return open && cancel && cancel(); }

bool FileDialog::Impl::Open() {
  if (!gdk_display_get_default() && !gtk_init_check(nullptr, nullptr)) {
    error = "Cannot open a GTK display";
    result = FileDialogResult::Failed;
    return false;
  }
  if (parent && !ParentToplevel(parent)) {
    error = "Invalid picker parent";
    result = FileDialogResult::Failed;
    return false;
  }
  GtkFileChooserAction action = GTK_FILE_CHOOSER_ACTION_OPEN;
  const char* accept = "_Open";
  const char* title = mode == FileDialogMode::OpenFiles ? "Open Files" : "Open File";
  if (mode == FileDialogMode::SaveFile) {
    action = GTK_FILE_CHOOSER_ACTION_SAVE;
    accept = "_Save";
    title = "Save File";
  } else if (mode == FileDialogMode::SelectFolder) {
    action = GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER;
    accept = "_Select";
    title = "Select Folder";
  }
  GtkWindow* owner = ParentToplevel(parent);
  GtkFileChooserNative* dialog = gtk_file_chooser_native_new(title, owner, action, accept, "_Cancel");
  auto* chooser = GTK_FILE_CHOOSER(dialog);
  gtk_native_dialog_set_modal(GTK_NATIVE_DIALOG(dialog), TRUE);
  gtk_file_chooser_set_local_only(chooser, TRUE);
  gtk_file_chooser_set_select_multiple(chooser, mode == FileDialogMode::OpenFiles);
  if (mode == FileDialogMode::SaveFile) {
    gtk_file_chooser_set_do_overwrite_confirmation(chooser, TRUE);
    std::string name = suggested_name;
    // Mirror IFileDialog::SetDefaultExtension: a bare name gets the first type.
    if (!extensions.empty() && extensions.front() != "*" && name.find('.') == std::string::npos)
      name = (name.empty() ? "Untitled" : name) + extensions.front();
    if (!name.empty()) gtk_file_chooser_set_current_name(chooser, name.c_str());
  }
  if (!extensions.empty() && mode != FileDialogMode::SelectFolder) {
    GtkFileFilter* filter = gtk_file_filter_new();
    std::string label;
    for (const auto& ext : extensions) {
      gtk_file_filter_add_pattern(filter, CaseInsensitiveGlob(ext).c_str());
      label += (label.empty() ? "" : ", ") + (ext == "*" ? std::string("*") : "*" + ext);
    }
    gtk_file_filter_set_name(filter, label.c_str());
    gtk_file_chooser_add_filter(chooser, filter);  // takes the floating ref
  }

  Session session;
  session.loop = g_main_loop_new(nullptr, FALSE);
  const gulong handler = g_signal_connect(dialog, "response", G_CALLBACK(OnResponse), &session);
  GMainContext* context = g_main_context_default();
  open = true;
  cancel = [dialog, &session, context] {
    if (!g_main_context_is_owner(context) || session.done) return false;
    // hide aborts the interaction without emitting "response".
    gtk_native_dialog_hide(GTK_NATIVE_DIALOG(dialog));
    session.response = GTK_RESPONSE_CANCEL;
    session.done = true;
    g_main_loop_quit(session.loop);
    return true;
  };
  gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
  // Nested loop like gtk_native_dialog_run, but Close() can end it without a
  // response (run would keep waiting for one).
  if (!session.done) g_main_loop_run(session.loop);
  cancel = {};
  open = false;

  if (session.response == GTK_RESPONSE_ACCEPT) {
    GSList* files = gtk_file_chooser_get_filenames(chooser);
    for (GSList* item = files; item; item = item->next) {
      paths.emplace_back(static_cast<char*>(item->data));
      g_free(item->data);
    }
    g_slist_free(files);
  }
  g_signal_handler_disconnect(dialog, handler);
  gtk_native_dialog_destroy(GTK_NATIVE_DIALOG(dialog));
  g_object_unref(dialog);
  g_main_loop_unref(session.loop);
  result = paths.empty() ? FileDialogResult::Cancelled : FileDialogResult::Accepted;
  return true;
}
}  // namespace nativeapi
