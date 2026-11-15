#include <exception>
#include "file_dialog_impl.h"
namespace nativeapi {
FileDialog::FileDialog(FileDialogMode mode) : pimpl_(std::make_unique<Impl>(mode)) {}
FileDialog::~FileDialog() = default;
bool FileDialog::SetParentWindow(std::shared_ptr<Window> window) {
  std::lock_guard<std::mutex> lock(pimpl_->state_mutex);
  if (pimpl_->opening)
    return false;
  pimpl_->parent = std::move(window);
  return true;
}
bool FileDialog::SetFileTypes(const std::vector<std::string>& extensions) {
  std::lock_guard<std::mutex> lock(pimpl_->state_mutex);
  if (pimpl_->opening)
    return false;
  for (const auto& ext : extensions) {
    if (ext == "*" && pimpl_->mode != FileDialogMode::SaveFile)
      continue;
    if (ext.size() < 2 || ext[0] != '.' ||
        ext.find_first_of("*/\\:;?\"<>|\r\n\t") != std::string::npos ||
        ext.find('\0') != std::string::npos)
      return false;
  }
  pimpl_->extensions = extensions;
  return true;
}
bool FileDialog::SetSuggestedFileName(const std::string& name) {
  std::lock_guard<std::mutex> lock(pimpl_->state_mutex);
  if (pimpl_->opening || name.find_first_of("/\\:;?*\"<>|\r\n\t") != std::string::npos ||
      name.find('\0') != std::string::npos)
    return false;
  pimpl_->suggested_name = name;
  return true;
}
DialogModality FileDialog::GetModality() const {
  std::lock_guard<std::mutex> lock(pimpl_->state_mutex);
  return pimpl_->modality;
}
void FileDialog::SetModality(DialogModality modality) {
  std::lock_guard<std::mutex> lock(pimpl_->state_mutex);
  if (!pimpl_->opening)
    pimpl_->modality = modality;
}
bool FileDialog::Open() {
  {
    std::lock_guard<std::mutex> lock(pimpl_->state_mutex);
    if (pimpl_->opening)
      return false;
    pimpl_->result = FileDialogResult::None;
    pimpl_->paths.clear();
    pimpl_->error.clear();
    if (pimpl_->modality != DialogModality::Window || pimpl_->mode < FileDialogMode::OpenFile ||
        pimpl_->mode > FileDialogMode::SelectFolder) {
      pimpl_->result = FileDialogResult::Failed;
      pimpl_->error = "Invalid picker mode or modality";
      return false;
    }
    pimpl_->opening = true;
  }
  bool opened = false;
  try {
    opened = pimpl_->Open();
  } catch (const std::exception& error) {
    pimpl_->result = FileDialogResult::Failed;
    pimpl_->paths.clear();
    pimpl_->error = error.what();
  } catch (...) {
    pimpl_->result = FileDialogResult::Failed;
    pimpl_->paths.clear();
    pimpl_->error = "Unexpected picker failure";
  }
  {
    // Backends publish their result only after the native picker finishes. Until
    // then readers see absence and configuration stays immutable, including when
    // a worker waits while the UI thread pumps the native picker.
    std::lock_guard<std::mutex> lock(pimpl_->state_mutex);
    pimpl_->opening = false;
  }
  return opened;
}
bool FileDialog::Close() {
  return pimpl_->Close();
}
FileDialogResult FileDialog::GetResult() const {
  std::lock_guard<std::mutex> lock(pimpl_->state_mutex);
  return pimpl_->opening ? FileDialogResult::None : pimpl_->result;
}
std::vector<std::string> FileDialog::GetPaths() const {
  std::lock_guard<std::mutex> lock(pimpl_->state_mutex);
  return pimpl_->opening ? std::vector<std::string>{} : pimpl_->paths;
}
std::string FileDialog::GetLastError() const {
  std::lock_guard<std::mutex> lock(pimpl_->state_mutex);
  return pimpl_->opening ? std::string{} : pimpl_->error;
}
}  // namespace nativeapi
