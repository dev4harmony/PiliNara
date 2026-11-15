#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "foundation/event.h"
#include "foundation/event_emitter.h"

namespace nativeapi {

class Image;

/** @brief Copyable clipboard content. Missing fields advertise no corresponding format. */
struct ClipboardData {
  /** UTF-8 plain text; nullopt differs from an empty string. */
  std::optional<std::string> text;
  /** UTF-8 HTML fragment; plain-text fallback must be supplied explicitly. */
  std::optional<std::string> html;
  /** Raster image, or nullptr when absent. */
  std::shared_ptr<Image> image;
  /** Local absolute paths; an empty list advertises no files. */
  std::vector<std::string> file_paths;
};

/** @brief Base class for clipboard notifications. */
class ClipboardEvent : public Event {
 public:
  std::string GetTypeName() const override { return "ClipboardEvent"; }
};

/** @brief The clipboard may have changed; read it again to obtain current content. */
class ClipboardChangedEvent : public ClipboardEvent {
 public:
  std::string GetTypeName() const override { return "ClipboardChangedEvent"; }
};

/**
 * @brief Accesses the system copy/paste clipboard (not the X11 PRIMARY selection).
 *
 * Desktop calls and listener registration are UI-thread only; mobile backends marshal
 * calls to the host UI thread. Reads complete exactly
 * once, on a later UI-loop turn, and require a running host event loop. A callback's
 * first argument is false on access, conversion or concurrent-change failure; its
 * second argument is then default-initialized. Missing formats are successful reads.
 * Notifications may be coalesced and contain no clipboard content. Subscribing does
 * not emit an initial event. Writes replace every previous format and retain copies.
 * Native commit failures may leave the clipboard empty or partially updated.
 *
 * @note Platform availability:
 * - macOS: ✅ Fully supported - NSPasteboard; changeCount sampled every 100 ms
 * - Windows: ✅ Fully supported - Win32 clipboard and WM_CLIPBOARDUPDATE
 * - Linux: ⚠️ GTK 3 - Access follows compositor policy; persistence is best effort
 * - Android: ⚠️ NativeActivity host initialization required - C++/JNI ClipboardManager;
 *   focused metadata polling every 100 ms, including focus restoration; text/HTML writes
 *   need explicit text, clear requires Android 9+; image/file publication is unsupported
 * - iOS: ⚠️ Foreground only - UIPasteboard notifications and resume checks; reads
 *   follow system paste permission, file URLs remain subject to app sandbox access
 * - OpenHarmony: ⚠️ N-API UI host initialization required - Text/HTML, independent
 *   PixelMap images, readable local file/directory URIs and local change notifications;
 *   reads follow READ_PASTEBOARD permission policy. Foreign sandbox URIs that cannot
 *   resolve to an accessible local path are ignored. File conversion/validation
 *   completes before publication. Before initialization reads fail and writes return false.
 *
 * @code
 * auto& clipboard = Clipboard::GetInstance();
 * ClipboardData data;
 * data.text = "Hello";
 * data.html = "<b>Hello</b>";
 * clipboard.Write(data);
 * clipboard.ReadText([](bool success, std::optional<std::string> text) {
 *   if (success && text) { }
 * });
 * @endcode
 */
class Clipboard : public EventEmitter<ClipboardEvent> {
 public:
  /** @brief Gets the process-wide clipboard service; callers do not own it. */
  static Clipboard& GetInstance();
  /** @brief Checks if the current session has a supported clipboard backend. */
  static bool IsSupported();
  /** @brief Checks if the current backend can report clipboard changes. */
  static bool IsChangeMonitoringSupported();

  virtual ~Clipboard();
  Clipboard(const Clipboard&) = delete;
  Clipboard& operator=(const Clipboard&) = delete;
  Clipboard(Clipboard&&) = delete;
  Clipboard& operator=(Clipboard&&) = delete;

  /** @brief Reads every supported format. @param callback Completion, or empty to do nothing. */
  void Read(std::function<void(bool, ClipboardData)> callback);
  /** @brief Reads plain text; successful absence is nullopt. @param callback Completion. */
  void ReadText(std::function<void(bool, std::optional<std::string>)> callback);
  /** @brief Reads an HTML fragment; successful absence is nullopt. @param callback Completion. */
  void ReadHtml(std::function<void(bool, std::optional<std::string>)> callback);
  /** @brief Reads an independent image; successful absence is nullptr. @param callback Completion.
   */
  void ReadImage(std::function<void(bool, std::shared_ptr<Image>)> callback);
  /** @brief Reads local paths, ignoring remote URIs. @param callback Completion. */
  void ReadFilePaths(std::function<void(bool, std::vector<std::string>)> callback);

  /**
   * @brief Replaces the clipboard with copies of the supplied formats.
   * @param data Content; an entirely absent data package clears the clipboard.
   * @return false for unsupported sessions, invalid UTF-8/NUL, non-absolute paths,
   * invalid images, or native publication failure. No rollback is promised.
   * @see Clipboard for platform availability.
   */
  bool Write(const ClipboardData& data);
  /** @brief Writes only plain text. @param text UTF-8 text. @return false on failure. */
  bool WriteText(const std::string& text);
  /** @brief Writes only HTML. Android requires Write with an explicit plain-text fallback.
   * @param html UTF-8 fragment. @return false on failure or unsupported HTML-only writes. */
  bool WriteHtml(const std::string& html);
  /** @brief Writes only an image. @param image Image, or nullptr to clear. @return false on
   * failure. */
  bool WriteImage(std::shared_ptr<Image> image);
  /** @brief Writes only paths. @param file_paths Absolute paths; empty clears. @return false on
   * failure. */
  bool WriteFilePaths(const std::vector<std::string>& file_paths);
  /** @brief Clears all formats, including content owned by other apps. @return false on failure. */
  bool Clear();
  /** @brief Checks if native change monitoring is currently active. */
  bool IsMonitoring() const;

 protected:
  void StartEventListening() override;
  void StopEventListening() override;

 private:
  Clipboard();
  class Impl;
  std::unique_ptr<Impl> pimpl_;
};

}  // namespace nativeapi
