import 'package:PiliPlus/common/widgets/gesture/image_double_tap_gesture_recognizer.dart'
    show ImageTapWallClock;
import 'package:flutter/gestures.dart'
    show TapGestureRecognizer, PointerDownEvent, PointerUpEvent;

class ImageTapGestureRecognizer extends TapGestureRecognizer {
  ImageTapGestureRecognizer({
    super.debugOwner,
    super.supportedDevices,
    super.allowedButtonsFilter,
    super.preAcceptSlopTolerance,
    super.postAcceptSlopTolerance,
  });

  Duration timeStamp = .zero;

  @override
  void handleTapUp({
    required PointerDownEvent down,
    required PointerUpEvent up,
  }) {
    timeStamp = up.timeStamp;
    // 上游用双击的 event 时间戳对比；3.44.9 拿不到，改用同一墙钟
    // （见 ImageDoubleTapGestureRecognizer 注释）。
    ImageTapWallClock.instance.lastTapUp = DateTime.now();
    super.handleTapUp(down: down, up: up);
  }
}
