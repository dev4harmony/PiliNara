#pragma once
#include <string>
namespace nativeapi::ohos {
// Kept separate from pasteboard headers: the two public NDKs have colliding
// unscoped ERR_OK enum names. URI conversion does not itself grant file access.
bool LocalPathToUri(const std::string& path, std::string& uri, bool& directory);
bool UriToLocalPath(const std::string& uri, std::string& path);
}  // namespace nativeapi::ohos
