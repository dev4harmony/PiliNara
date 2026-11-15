#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "foundation/event.h"
#include "foundation/event_emitter.h"
#include "foundation/geometry.h"
#include "window.h"

namespace nativeapi {

class Image;

/**
 * @brief What happens to dragged data when it is dropped.
 *
 * A DragSource offers one operation and a DropTarget accepts one; a drop only
 * happens when the two agree. Some SDKs cannot constrain the source offer;
 * check DragSource::IsOperationSelectionSupported() for that capability.
 */
enum class DragOperation {
  /** Nothing: the drag was cancelled, or the target refused it. */
  None,
  /** The target copies the data; the source keeps it. */
  Copy,
  /** The target takes the data; the source is expected to delete its copy. */
  Move,
  /** The target creates a link (alias, shortcut) to the data. */
  Link
};

/**
 * @brief Base class for events emitted by a DragSource.
 *
 * Reported positions use the screen coordinate space of Window::GetPosition()
 * (top-left origin, logical pixels). Sources that cannot report coordinates
 * use the origin fallback; check DragSource::IsPositionSupported() first.
 */
class DragSourceEvent : public Event {
 public:
  DragSourceEvent(WindowId window_id, Point position)
      : window_id_(window_id), position_(position) {}
  virtual ~DragSourceEvent() = default;

  /**
   * @brief The window the drag was started from.
   */
  WindowId GetWindowId() const { return window_id_; }

  /**
   * @brief The global cursor position in screen coordinates.
   *
   * Returns the origin when the native source cannot report an end position.
   * Check DragSource::IsPositionSupported() before interpreting this value.
   *
   * @note Platform availability:
   * - macOS: ✅ Fully supported - Native drag-ended screen point
   * - Windows: ✅ Fully supported - Native cursor point when the drag ends
   * - Linux: ✅ Fully supported - Native cursor point when the drag ends
   * - Android: ❌ Not applicable - No native drag source events
   * - iOS: ❌ Not applicable - No native drag source events
   * - OpenHarmony: ❌ Not applicable - Origin fallback; end coordinates unavailable
   */
  Point GetPosition() const { return position_; }

  std::string GetTypeName() const override { return "DragSourceEvent"; }

 private:
  WindowId window_id_;
  Point position_;
};

/**
 * @brief The drag is over: the data was dropped, or the drag was cancelled.
 */
class DragSourceEndedEvent : public DragSourceEvent {
 public:
  DragSourceEndedEvent(WindowId window_id, Point position, DragOperation operation)
      : DragSourceEvent(window_id, position), operation_(operation) {}

  /**
   * @brief The operation the target performed, or DragOperation::None when
   * nothing was dropped (the user cancelled, or no target accepted the data).
   */
  DragOperation GetOperation() const { return operation_; }

  std::string GetTypeName() const override { return "DragSourceEndedEvent"; }

 private:
  DragOperation operation_;
};

/**
 * @brief Drags files and text out of a window, into other windows and other
 * applications (a file manager, an editor, a mail composer).
 *
 * Describe the data with SetFilePaths() and / or SetText(), then call
 * StartDragging() while the primary pointer is held down — typically from
 * a mouse-down or drag-start handler. The platform's drag loop takes over the
 * mouse from there; a DragSourceEndedEvent reports how the drag ended.
 *
 * @code
 * auto source = std::make_shared<DragSource>();
 * source->SetFilePaths({"/Users/me/report.pdf"});
 * source->AddListener<DragSourceEndedEvent>([](const DragSourceEndedEvent& event) {
 *   if (event.GetOperation() == DragOperation::None) {
 *     // Nothing was dropped.
 *   }
 * });
 * // In a mouse-down / drag-start handler:
 * source->StartDragging(window);
 * @endcode
 *
 * Events are emitted on the main thread. The data may be changed between
 * drags; changing it during a drag has no effect on that drag.
 *
 * Platform notes:
 * - macOS (AppKit dragging session), Windows (OLE DoDragDrop) and Linux
 *   (GTK 3) are supported.
 * - OpenHarmony supports SDK-default public native ArkUI drag actions through an
 *   initialized ArkTS UI host, including ArkTS-only content. Start from an
 *   active primary pointer event. Explicit Move/Link preferences are refused;
 *   the SDK can still select Move at the receiver because a source-offer mask
 *   is unavailable. Ended events report the actual Copy/Move/None outcome. End notifications
 *   carry result/operation but no coordinates: IsPositionSupported() is false
 *   and event positions use the origin as an unavailable-value fallback.
 * - Android and iOS return false from IsSupported() and StartDragging().
 * - The platform drag loop consumes the mouse-up that ends the drag. So that
 *   the window's content does not keep treating the button as held, the window
 *   the drag started from receives a synthesized mouse-up when the drag ends
 *   (macOS, Windows).
 */
class DragSource : public EventEmitter<DragSourceEvent> {
 public:
  /**
   * @brief Checks if dragging data out of a window is available on this
   * platform.
   *
   * @note Platform availability:
   * - macOS: ✅ Fully supported - AppKit source sessions
   * - Windows: ✅ Fully supported - OLE source sessions
   * - Linux: ✅ Fully supported - GTK source sessions
   * - Android: ❌ Not applicable - Returns false
   * - iOS: ❌ Not applicable - Returns false
   * - OpenHarmony: ⚠️ Partial support - Initialized ArkTS UI host; SDK-default drag actions
   */
  static bool IsSupported();

  /**
   * @brief Checks whether native drag-ended events report a screen position.
   *
   * When false, DragSourceEvent::GetPosition() returns the origin as a fallback.
   * This query describes native source notifications, not user-constructed events.
   *
   * @note Platform availability:
   * - macOS: ✅ Fully supported - Returns true
   * - Windows: ✅ Fully supported - Returns true
   * - Linux: ✅ Fully supported - Returns true
   * - Android: ❌ Not applicable - Returns false
   * - iOS: ❌ Not applicable - Returns false
   * - OpenHarmony: ❌ Not applicable - Public drag-action notifications omit coordinates
   */
  static bool IsPositionSupported();

  /**
   * @brief Checks whether a source can constrain the operation offered to targets.
   *
   * When false, the SDK receiver chooses the operation and the ended event reports
   * its actual choice. Only the default Copy preference can start an OHOS source;
   * that SDK cannot enforce a Copy-only offer and may report a Move completion.
   * The backend never deletes the source files itself.
   *
   * @note Platform availability:
   * - macOS: ✅ Fully supported - Returns true
   * - Windows: ✅ Fully supported - Returns true
   * - Linux: ✅ Fully supported - Returns true
   * - Android: ❌ Not applicable - Returns false
   * - iOS: ❌ Not applicable - Returns false
   * - OpenHarmony: ❌ Not applicable - No public source-offer mask operation
   */
  static bool IsOperationSelectionSupported();

  DragSource();
  virtual ~DragSource();

  DragSource(const DragSource&) = delete;
  DragSource& operator=(const DragSource&) = delete;
  DragSource(DragSource&&) = delete;
  DragSource& operator=(DragSource&&) = delete;

  /**
   * @brief Sets the files to drag.
   *
   * @param file_paths Absolute paths of existing files or directories. An
   *        empty list drags no files.
   */
  void SetFilePaths(const std::vector<std::string>& file_paths);

  /**
   * @brief The files the next drag carries.
   */
  std::vector<std::string> GetFilePaths() const;

  /**
   * @brief Sets plain text to drag, alongside or instead of files.
   *
   * @param text UTF-8 text, or std::nullopt to drag no text.
   */
  void SetText(const std::optional<std::string>& text);

  /**
   * @brief The text the next drag carries, if any.
   */
  std::optional<std::string> GetText() const;

  /**
   * @brief Sets the image shown under the cursor while dragging.
   *
   * @param image The drag image, or nullptr for the platform default (the file
   *        icons on macOS; the system drag cursor elsewhere).
   *
   * @note Platform availability:
   * - macOS: ✅ Fully supported - The image replaces the file icons
   * - Windows: ✅ Fully supported - Shown through the shell drag image helper
   * - Linux: ✅ Fully supported - Set as the drag icon
   * - Android: ❌ Not applicable - Always ignored
   * - iOS: ❌ Not applicable - Always ignored
   * - OpenHarmony: ✅ Fully supported - PixelMap preview; nullptr uses a compact default tile
   */
  void SetImage(std::shared_ptr<Image> image);

  /**
   * @brief The drag image, or nullptr for the platform default.
   *
   * @see SetImage() for platform availability.
   */
  std::shared_ptr<Image> GetImage() const;

  /**
   * @brief Sets the requested operation offered to drop targets.
   *
   * @param operation The operation to offer; defaults to DragOperation::Copy.
   *        DragOperation::Move tells file managers they may move the files
   *        away, so only offer it when that is intended.
   *
   * @note Platform availability:
   * - macOS: ✅ Fully supported - Constrains the native source operation mask
   * - Windows: ✅ Fully supported - Constrains the native source operation mask
   * - Linux: ✅ Fully supported - Constrains the native source operation mask
   * - Android: ❌ Not applicable - No native drag source
   * - iOS: ❌ Not applicable - No native drag source
   * - OpenHarmony: ⚠️ Partial support - Only default Copy may start; the SDK
   *   receiver can choose Copy or Move; see IsOperationSelectionSupported()
   */
  void SetDragOperation(DragOperation operation);

  /**
   * @brief The requested operation offered to drop targets.
   *
   * @see SetDragOperation() for platform availability.
   */
  DragOperation GetDragOperation() const;

  /**
   * @brief Starts dragging the data out of a window.
   *
   * @param window The window the drag starts from. The drag image appears
   *        under the cursor.
   * @return false if the platform does not support dragging, the window is
   *         gone, there is no data to drag (no files and no text), the
   *         operation is DragOperation::None, the primary pointer is not
   *         held down, a drag is already in progress, or the platform SDK
   *         rejects the data or request.
   *
   * The call returns right away; the drag continues in the platform's drag
   * loop, and DragSourceEndedEvent is emitted when it is over.
   *
   * @note Platform availability:
   * - macOS: ✅ Fully supported - Starts an NSDraggingSession from the content view
   * - Windows: ✅ Fully supported - Runs DoDragDrop on the next turn of the message loop
   * - Linux: ✅ Fully supported - Starts a GTK drag on the window
   * - Android: ❌ Not applicable - Returns false
   * - iOS: ❌ Not applicable - Returns false
   * - OpenHarmony: ⚠️ Partial support - Initialized ArkTS UI host; SDK receiver chooses operation, no end coordinates
   */
  bool StartDragging(std::shared_ptr<Window> window);

  /**
   * @brief Checks if a drag started by this source is in progress.
   */
  bool IsDragging() const;

 private:
  class Impl;
  std::unique_ptr<Impl> pimpl_;
};

}  // namespace nativeapi
