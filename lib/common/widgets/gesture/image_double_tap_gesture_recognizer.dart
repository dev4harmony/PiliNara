import 'package:flutter/gestures.dart' show DoubleTapGestureRecognizer;

/// 记录最近一次双击完成时刻，供 gallery_viewer 的 `_onTap` 区分
/// "双击后 300ms 内触发的单击"（该单击应被忽略）。
///
/// 上游（Flutter 3.47）通过覆写 `DoubleTapGestureRecognizer.checkUp` 拿到
/// 第二次抬起的 PointerUpEvent 时间戳；本地 OHOS Flutter 3.44.9 的
/// `DoubleTapGestureRecognizer` 没有该保护方法，因此改为在构造时注册
/// `onDoubleTap` 回调记录墙钟时间；配套的 [ImageTapGestureRecognizer]
/// （image_tap_gesture_recognizer.dart）也记录墙钟时间，两者相减同一时钟。
class ImageDoubleTapGestureRecognizer extends DoubleTapGestureRecognizer {
  ImageDoubleTapGestureRecognizer({
    super.debugOwner,
    super.supportedDevices,
    super.allowedButtonsFilter,
  }) {
    onDoubleTap = () {
      timeStamp = DateTime.now();
    };
  }

  DateTime timeStamp = DateTime.fromMillisecondsSinceEpoch(0);
}

/// 与 [ImageDoubleTapGestureRecognizer] 使用同一墙钟，记录最近一次单击抬起。
class ImageTapWallClock {
  ImageTapWallClock._();

  static final ImageTapWallClock instance = ImageTapWallClock._();

  DateTime lastTapUp = DateTime.fromMillisecondsSinceEpoch(0);
}
