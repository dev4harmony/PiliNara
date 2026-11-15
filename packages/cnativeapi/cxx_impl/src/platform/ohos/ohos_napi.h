#pragma once
// Internal checked helpers for public ArkTS APIs. No values may escape their UI scope.
#include <napi/native_api.h>
#include <string>
#include <vector>

namespace nativeapi::ohos::js {
inline void ClearException(napi_env env) {
  bool pending = false;
  if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
    napi_value error;
    napi_get_and_clear_last_exception(env, &error);
  }
}
inline bool Property(napi_env env, napi_value object, const char* name, napi_value& value) {
  if (!object || napi_get_named_property(env, object, name, &value) != napi_ok) {
    ClearException(env);
    value = nullptr;
    return false;
  }
  return true;
}
inline bool Call(napi_env env,
                 napi_value object,
                 const char* name,
                 const std::vector<napi_value>& args,
                 napi_value& result) {
  napi_value method;
  if (!Property(env, object, name, method) ||
      napi_call_function(env, object, method, args.size(), args.data(), &result) != napi_ok) {
    ClearException(env);
    result = nullptr;
    return false;
  }
  return true;
}
inline bool Module(napi_env env, const char* name, napi_value& value) {
  if (napi_load_module(env, name, &value) != napi_ok) {
    ClearException(env);
    value = nullptr;
    return false;
  }
  return true;
}
inline std::string String(napi_env env, napi_value value) {
  size_t length = 0;
  if (!value || napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok)
    return {};
  std::string result(length + 1, '\0');
  if (napi_get_value_string_utf8(env, value, result.data(), result.size(), &length) != napi_ok)
    return {};
  result.resize(length);
  return result;
}
inline std::string StringProperty(napi_env env, napi_value object, const char* name) {
  napi_value value;
  return Property(env, object, name, value) ? String(env, value) : "";
}
inline napi_value Number(napi_env env, double number) {
  napi_value value = nullptr;
  napi_create_double(env, number, &value);
  return value;
}
}  // namespace nativeapi::ohos::js
