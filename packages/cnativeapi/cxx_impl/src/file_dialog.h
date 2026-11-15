#pragma once
#include <memory>
#include <string>
#include <vector>
#include "dialog.h"
namespace nativeapi {
class Window;
enum class FileDialogMode { OpenFile, OpenFiles, SaveFile, SelectFolder };
enum class FileDialogResult { None, Accepted, Cancelled, Failed };
/** System file picker. Open blocks until the native picker is dismissed.
 * Windows and Linux require the UI thread and pump their native message loop.
 * OpenHarmony requires a worker while the ArkTS UI thread keeps processing events.
 *
 * @note Platform availability:
 * - macOS: ❌ Not implemented.
 * - Windows: ✅ WinRT picker with WinUI3 enabled, IFileDialog otherwise.
 * - Linux: ✅ GtkFileChooserNative (portal when GTK selects it), local files only.
 * - Android: ❌ Not implemented.
 * - iOS: ❌ Not implemented.
 * - OpenHarmony: ✅ Public DocumentViewPicker after host/context initialization;
 *   attached visible parent required, current window used when unset. Results are
 *   authorized document URIs; programmatic Close is unavailable in the public SDK.
 */
class FileDialog : public Dialog {
 public:
  explicit FileDialog(FileDialogMode mode);
  ~FileDialog() override;
  FileDialog(const FileDialog&) = delete;
  FileDialog& operator=(const FileDialog&) = delete;
  FileDialog(FileDialog&&) = delete;
  FileDialog& operator=(FileDialog&&) = delete;
  static bool IsSupported();
  /** Configuration setters reject changes while open. */
  bool SetParentWindow(std::shared_ptr<Window> window);
  /** Extensions such as .txt or .png; * allowed for Open, not Save.
   * OpenHarmony cannot represent a literal comma within one extension; Open
   * fails explicitly for that configuration.
   */
  bool SetFileTypes(const std::vector<std::string>& extensions);
  bool SetSuggestedFileName(const std::string& name);
  DialogModality GetModality() const override;
  /** Pickers are window-modal. None/Application are rejected by Open. */
  void SetModality(DialogModality modality) override;
  /** True on acceptance or cancellation; false on failure. Inspect GetResult().
   * WinRT SaveFile may create an empty selected file as part of the system picker.
   * OpenHarmony SaveFile uses the SDK default that pre-creates an empty selected document.
   */
  bool Open() override;
  /** Request native cancellation. OpenHarmony returns false: the SDK exposes no
   * close operation; the user can still cancel in the system picker UI.
   */
  bool Close() override;
  /** None while Open is pending; final native result after Open returns. */
  FileDialogResult GetResult() const;
  /** Empty while pending or after cancellation/failure. OpenHarmony returns the
   * exact authorized document URIs from the system, for use with @ohos.file.fs;
   * do not strip their scheme or treat them as accessible POSIX paths.
   */
  std::vector<std::string> GetPaths() const;
  std::string GetLastError() const;

 private:
  class Impl;
  std::unique_ptr<Impl> pimpl_;
};
}  // namespace nativeapi
