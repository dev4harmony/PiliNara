#pragma once
#include <multimodalinput/oh_input_manager.h>
#include <multimodalinput/oh_key_code.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nativeapi::ohos {
struct HotkeyDeleter {
  void operator()(Input_Hotkey* value) const { OH_Input_DestroyHotkey(&value); }
};
using Hotkey = std::unique_ptr<Input_Hotkey, HotkeyDeleter>;
struct HotkeySpec {
  std::vector<int32_t> modifiers;
  int32_t key = -1;
  std::string Signature() const {
    std::string result = std::to_string(key);
    for (auto modifier : modifiers) result += ":" + std::to_string(modifier);
    return result;
  }
  Hotkey Create() const {
    Hotkey result(OH_Input_CreateHotkey());
    if (result) {
      auto keys = modifiers;
      std::sort(keys.begin(), keys.end());
      OH_Input_SetPreKeys(result.get(), keys.data(), static_cast<int32_t>(keys.size()));
      OH_Input_SetFinalKey(result.get(), key);
      OH_Input_SetRepeat(result.get(), false);
      std::array<int32_t, 4> actual{};
      std::array<int32_t*, 4> pointers{&actual[0], &actual[1], &actual[2], &actual[3]};
      int32_t size = 0, final = -1;
      if (OH_Input_GetPreKeys(result.get(), pointers.data(), &size) != INPUT_SUCCESS ||
          size != static_cast<int32_t>(keys.size()) ||
          !std::equal(keys.begin(), keys.end(), actual.begin()) ||
          OH_Input_GetFinalKey(result.get(), &final) != INPUT_SUCCESS || final != key) return {};

    }
    return result;
  }
};
inline bool ReadHotkey(const Input_Hotkey* native, HotkeySpec& spec) {
  // This API fills caller-owned pointers to individual values, not an allocated
  // flat array. Four is the documented maximum modifier count.
  std::array<int32_t, 4> values{};
  std::array<int32_t*, 4> pointers{&values[0], &values[1], &values[2], &values[3]};
  int32_t count = 0;
  if (OH_Input_GetFinalKey(native, &spec.key) != INPUT_SUCCESS ||
      OH_Input_GetPreKeys(native, pointers.data(), &count) != INPUT_SUCCESS || count < 1 || count > 4) return false;
  spec.modifiers.assign(values.begin(), values.begin() + count);
  for (auto& key : spec.modifiers) {
    if (key == KEYCODE_CTRL_RIGHT) key = KEYCODE_CTRL_LEFT;
    if (key == KEYCODE_ALT_RIGHT) key = KEYCODE_ALT_LEFT;
    if (key == KEYCODE_SHIFT_RIGHT) key = KEYCODE_SHIFT_LEFT;
  }
  std::sort(spec.modifiers.begin(), spec.modifiers.end());
  return true;
}
inline std::vector<Hotkey> HotkeyVariants(const HotkeySpec& spec) {
  std::vector<HotkeySpec> variants{spec};
  for (size_t i = 0; i < spec.modifiers.size(); ++i) {
    const auto count = variants.size();
    for (size_t j = 0; j < count; ++j) {
      auto variant = variants[j];
      auto& code = variant.modifiers[i];
      code = code == KEYCODE_CTRL_LEFT ? KEYCODE_CTRL_RIGHT :
             code == KEYCODE_ALT_LEFT ? KEYCODE_ALT_RIGHT : KEYCODE_SHIFT_RIGHT;
      variants.push_back(std::move(variant));
    }
  }
  std::vector<Hotkey> result;
  for (const auto& variant : variants) {
    auto native = variant.Create();
    if (!native) return {};
    result.push_back(std::move(native));
  }
  return result;
}
inline bool ParseHotkey(const std::string& accelerator, HotkeySpec& result, bool application = false) {
  result = {};
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= accelerator.size()) {
    const auto end = accelerator.find('+', start);
    auto token = accelerator.substr(start, end == std::string::npos ? end : end - start);
    std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) { return std::tolower(c); });
    if (token.empty()) return false;
    parts.push_back(std::move(token));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  if (parts.size() < 2 && !application) return false;  // Global SDK requires modifiers.
  for (size_t i = 0; i + 1 < parts.size(); ++i) {
    const auto& token = parts[i];
    int32_t key;
    if (token == "ctrl" || token == "control" || token == "cmdorctrl" || token == "commandorcontrol") key = KEYCODE_CTRL_LEFT;
    else if (token == "alt" || token == "option") key = KEYCODE_ALT_LEFT;
    else if (token == "shift") key = KEYCODE_SHIFT_LEFT;
    else return false;  // Meta/Fn/lock modifiers are not representable by this SDK.
    if (std::find(result.modifiers.begin(), result.modifiers.end(), key) != result.modifiers.end()) return false;
    result.modifiers.push_back(key);
  }
  std::sort(result.modifiers.begin(), result.modifiers.end());
  const auto& token = parts.back();
  if (token.size() == 1 && token[0] >= 'a' && token[0] <= 'z') result.key = KEYCODE_A + token[0] - 'a';
  else if (token.size() == 1 && token[0] >= '0' && token[0] <= '9') result.key = KEYCODE_0 + token[0] - '0';
  else if (token[0] == 'f' && token.size() <= 3 && token.size() > 1 &&
           std::all_of(token.begin() + 1, token.end(), [](unsigned char c) { return std::isdigit(c); })) {
    const auto number = std::stoi(token.substr(1));
    if (number < 1 || number > 24) return false;
    result.key = number <= 12 ? KEYCODE_F1 + number - 1 : KEYCODE_F13 + number - 13;
  } else if (token.size() == 4 && token.rfind("num", 0) == 0 && token[3] >= '0' && token[3] <= '9') {
    result.key = KEYCODE_NUMPAD_0 + token[3] - '0';
  } else {
    static const std::unordered_map<std::string, int32_t> names = {
      {"space", KEYCODE_SPACE}, {"tab", KEYCODE_TAB}, {"enter", KEYCODE_ENTER}, {"return", KEYCODE_ENTER},
      {"escape", KEYCODE_ESCAPE}, {"esc", KEYCODE_ESCAPE}, {"backspace", KEYCODE_DEL},
      {"delete", KEYCODE_FORWARD_DEL}, {"forwarddelete", KEYCODE_FORWARD_DEL}, {"insert", KEYCODE_INSERT}, {"help", KEYCODE_HELP},
      {"home", KEYCODE_MOVE_HOME}, {"end", KEYCODE_MOVE_END}, {"pageup", KEYCODE_PAGE_UP}, {"pagedown", KEYCODE_PAGE_DOWN},
      {"up", KEYCODE_DPAD_UP}, {"down", KEYCODE_DPAD_DOWN}, {"left", KEYCODE_DPAD_LEFT}, {"right", KEYCODE_DPAD_RIGHT},
      {"plus", KEYCODE_PLUS}, {"equal", KEYCODE_EQUALS}, {"=", KEYCODE_EQUALS}, {"minus", KEYCODE_MINUS}, {"-", KEYCODE_MINUS},
      {"comma", KEYCODE_COMMA}, {",", KEYCODE_COMMA}, {"period", KEYCODE_PERIOD}, {".", KEYCODE_PERIOD},
      {"slash", KEYCODE_SLASH}, {"/", KEYCODE_SLASH}, {"backslash", KEYCODE_BACKSLASH}, {"\\", KEYCODE_BACKSLASH},
      {"semicolon", KEYCODE_SEMICOLON}, {";", KEYCODE_SEMICOLON}, {"quote", KEYCODE_APOSTROPHE}, {"'", KEYCODE_APOSTROPHE},
      {"leftbracket", KEYCODE_LEFT_BRACKET}, {"[", KEYCODE_LEFT_BRACKET}, {"rightbracket", KEYCODE_RIGHT_BRACKET}, {"]", KEYCODE_RIGHT_BRACKET},
      {"grave", KEYCODE_GRAVE}, {"backquote", KEYCODE_GRAVE}, {"`", KEYCODE_GRAVE},
      {"numdec", KEYCODE_NUMPAD_DOT}, {"numadd", KEYCODE_NUMPAD_ADD}, {"numsub", KEYCODE_NUMPAD_SUBTRACT},
      {"nummult", KEYCODE_NUMPAD_MULTIPLY}, {"numdiv", KEYCODE_NUMPAD_DIVIDE}, {"numenter", KEYCODE_NUMPAD_ENTER}};
    const auto found = names.find(token);
    if (found == names.end()) return false;
    result.key = found->second;
  }
  return true;
}
}  // namespace nativeapi::ohos
