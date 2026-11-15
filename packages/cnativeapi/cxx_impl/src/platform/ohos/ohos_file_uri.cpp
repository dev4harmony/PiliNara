#include "ohos_file_uri.h"
#include "../../app_info.h"

#include <filemanagement/file_uri/oh_file_uri.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>

namespace nativeapi::ohos {
bool LocalPathToUri(const std::string& path, std::string& uri, bool& directory) {
  struct stat info{};
  if (path.empty() || path.front() != '/' || path.find('\0') != path.npos ||
      path.size() > std::numeric_limits<unsigned int>::max() || stat(path.c_str(), &info) != 0 ||
      access(path.c_str(), R_OK) != 0 || (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)))
    return false;
  char* raw = nullptr;
  const auto status = OH_FileUri_GetUriFromPath(path.c_str(), path.size(), &raw);
  std::unique_ptr<char, decltype(&std::free)> owned(raw, std::free);
  if (status != ERR_OK || !raw)
    return false;
  uri = raw;
  directory = S_ISDIR(info.st_mode);
  return true;
}
bool UriToLocalPath(const std::string& uri, std::string& path) {
  if (uri.rfind("file://", 0) != 0 || uri.size() > std::numeric_limits<unsigned int>::max() ||
      uri.find('\0') != uri.npos || !OH_FileUri_IsValidUri(uri.c_str(), uri.size()))
    return false;
  char* raw = nullptr;
  const auto status = OH_FileUri_GetPathFromUri(uri.c_str(), uri.size(), &raw);
  std::unique_ptr<char, decltype(&std::free)> owned(raw, std::free);
  if (status != ERR_OK || !raw || raw[0] != '/')
    return false;
  const auto slash = uri.find('/', 7);
  const auto authority = uri.substr(7, slash == uri.npos ? uri.npos : slash - 7);
  const std::string decoded(raw);
  struct stat info{};
  if (stat(decoded.c_str(), &info) != 0 || access(decoded.c_str(), R_OK) != 0 ||
      (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)))
    return false;
  // A foreign app's private sandbox path names a different file in our sandbox.
  // Never turn that foreign URI into a misleading local path. Shared mount paths
  // remain usable only if this process can actually read them.
  if (!authority.empty() && authority != AppInfo::GetInstance().GetIdentifier()) {
    // Normalize before testing the sandbox root: dot segments or symlinks must
    // not let a foreign authority alias a readable file in this application's
    // sandbox. Keep the caller-visible path unchanged after validation.
    std::unique_ptr<char, decltype(&std::free)> resolved(realpath(decoded.c_str(), nullptr),
                                                       std::free);
    const auto normalized = std::filesystem::path(decoded).lexically_normal().string();
    if (normalized == "/data/storage" || normalized.rfind("/data/storage/", 0) == 0 ||
        !resolved || std::string(resolved.get()) == "/data/storage" ||
        std::string(resolved.get()).rfind("/data/storage/", 0) == 0)
      return false;
  }
  path = decoded;
  return true;
}
}  // namespace nativeapi::ohos
