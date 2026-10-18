import 'dart:async' show Completer, StreamSubscription, Timer, unawaited;
import 'dart:convert' show ascii;
import 'dart:io' show Platform;
import 'dart:math' show max, min;
import 'dart:ui' as ui;

import 'package:PiliPlus/common/assets.dart';
import 'package:PiliPlus/harmony_adapt/harmony_channel.dart';
import 'package:PiliPlus/http/browser_ua.dart';
import 'package:PiliPlus/http/constants.dart';
import 'package:PiliPlus/http/loading_state.dart';
import 'package:PiliPlus/http/video.dart';
import 'package:PiliPlus/media_kit_adapt/media_kit_adapt.dart';
import 'package:PiliPlus/models/common/account_type.dart';
import 'package:PiliPlus/models/common/audio_normalization.dart';
import 'package:PiliPlus/models/common/dm_chart_source.dart';
import 'package:PiliPlus/models/common/super_resolution_type.dart';
import 'package:PiliPlus/models/common/video/video_quality.dart';
import 'package:PiliPlus/models/common/video/video_type.dart';
import 'package:PiliPlus/models/user/danmaku_rule.dart';
import 'package:PiliPlus/models/video/play/url.dart';
import 'package:PiliPlus/models_new/video/video_shot/data.dart';
import 'package:PiliPlus/pages/danmaku/danmaku_model.dart';
import 'package:PiliPlus/pages/setting/models/play_settings.dart'
    show kMaxVolume;
import 'package:PiliPlus/pages/sponsor_block/block_mixin.dart';
import 'package:PiliPlus/plugin/pl_player/models/auto_audio_only_state.dart';
import 'package:PiliPlus/plugin/pl_player/models/data_source.dart';
import 'package:PiliPlus/plugin/pl_player/models/data_status.dart';
import 'package:PiliPlus/plugin/pl_player/models/double_tap_type.dart';
import 'package:PiliPlus/plugin/pl_player/models/duration.dart';
import 'package:PiliPlus/plugin/pl_player/models/fullscreen_mode.dart';
import 'package:PiliPlus/plugin/pl_player/models/heart_beat_type.dart';
import 'package:PiliPlus/plugin/pl_player/models/ohos_hdr_output.dart';
import 'package:PiliPlus/plugin/pl_player/models/play_repeat.dart';
import 'package:PiliPlus/plugin/pl_player/models/play_status.dart';
import 'package:PiliPlus/plugin/pl_player/models/speed_lock_hint.dart';
import 'package:PiliPlus/plugin/pl_player/models/video_fit_type.dart';
import 'package:PiliPlus/plugin/pl_player/utils/danmaku_options.dart';
import 'package:PiliPlus/plugin/pl_player/utils/fullscreen.dart';
import 'package:PiliPlus/plugin/pl_player/utils/player_rebuild_queue.dart';
import 'package:PiliPlus/services/live_pip_overlay_service.dart';
import 'package:PiliPlus/services/pip_overlay_service.dart';
import 'package:PiliPlus/services/service_locator.dart';
import 'package:PiliPlus/utils/accounts.dart';
import 'package:PiliPlus/utils/android/android_helper.dart';
import 'package:PiliPlus/utils/android/bindings.g.dart';
import 'package:PiliPlus/utils/asset_utils.dart';
import 'package:PiliPlus/utils/device_utils.dart';
import 'package:PiliPlus/utils/duration_utils.dart';
import 'package:PiliPlus/utils/extension/box_ext.dart';
import 'package:PiliPlus/utils/extension/num_ext.dart';
import 'package:PiliPlus/utils/extension/string_ext.dart';
import 'package:PiliPlus/utils/feed_back.dart';
import 'package:PiliPlus/utils/image_utils.dart';
import 'package:PiliPlus/utils/ios/pip_helper.dart';
import 'package:PiliPlus/utils/page_utils.dart';
import 'package:PiliPlus/utils/path_utils.dart';
import 'package:PiliPlus/utils/platform_utils.dart';
import 'package:PiliPlus/utils/storage.dart';
import 'package:PiliPlus/utils/storage_key.dart';
import 'package:PiliPlus/utils/storage_pref.dart';
import 'package:PiliPlus/utils/utils.dart';
import 'package:PiliPlus/utils/video_utils.dart';
import 'package:archive/archive.dart' show getCrc32;
import 'package:canvas_danmaku/canvas_danmaku.dart';
import 'package:easy_debounce/easy_throttle.dart';
import 'package:floating/floating.dart';
import 'package:flutter/foundation.dart'
    show ValueNotifier, clampDouble, kDebugMode;
import 'package:flutter/services.dart'
    show DeviceOrientation, HapticFeedback;
import 'package:flutter_smart_dialog/flutter_smart_dialog.dart';
import 'package:flutter_volume_controller/flutter_volume_controller.dart';
import 'package:get/get.dart';
import 'package:hive_ce/hive.dart';
import 'package:material_ui/material_ui.dart';
import 'package:media_kit/media_kit.dart';
import 'package:media_kit_video/media_kit_video.dart';
import 'package:native_device_orientation/native_device_orientation.dart';
import 'package:os_type/os_type.dart';
import 'package:path/path.dart' as path;
import 'package:screen_brightness_platform_interface/screen_brightness_platform_interface.dart';
import 'package:wakelock_plus/wakelock_plus.dart';
import 'package:window_manager/window_manager.dart';

typedef PlayCallback = Future<void>? Function();
typedef PlayerInitCallback = Future<void> Function();
typedef PlayOwner = ({String tag, Type type});

class PlPlayerController with BlockConfigMixin, AudioNormalizationMixin {
  Player? _videoPlayerController;

  /// 播放器控制器。用 Rx 承载而不是普通字段：它的创建/释放都发生在异步链路
  /// 里（换源、初始化失败、退后台清内存），与页面「可以开始播放」的时机并不同
  /// 步。页面侧靠 `videoController == null` 决定是否挂载 PLVideoPlayer，普通字
  /// 段的变化读不到，守卫就会在控制器为空时把播放器视图挂上去，命中
  /// `_PLVideoPlayerState.initState` 里的空断言而整页报错。
  final Rxn<VideoController> _videoControllerNotifier = Rxn<VideoController>();

  /// setDataSource 与渲染路径重建共用的串行队列，见 [PlayerRebuildQueue]。
  late final PlayerRebuildQueue _playerQueue = PlayerRebuildQueue(
    hasPlayer: () => _videoPlayerController != null,
    isStale: () => _appliedOutput != _ohosOutput,
    rebuild: _rebuildForRenderPath,
    onError: _onRenderPathRebuildFailed,
  );

  /// 渲染路径重建**完成**后自增（新播放器已经 open 好）。
  ///
  /// 与 [videoController] 的变化不同：后者在 VideoController 刚被替换时就变了，
  /// 供 UI 尽快改挂新 controller，那时 `player.open()` 还没调用；这时候去设字幕
  /// 轨之类的东西会被随后的 loadfile 丢掉。页面级、需要播放器真正就绪的重挂
  /// 逻辑要订阅这一个。
  ///
  /// 只在 [_rebuildForRenderPath] 里自增——正常的 setDataSource 流程有 onInit 收口，
  /// 不需要也不应该重复触发。
  final RxInt playerRebuilt = 0.obs;

  static PlPlayerController? _instance;

  /// 真正的销毁已经开始（区别于 _playerCount 递减的早退路径）。
  /// dispose() 的收尾是异步的（等在途数据源与底层播放器释放），在尾部把
  /// _instance 置空之前，新页面可能已经通过 getInstance()/ensureInstance()
  /// 领走仍在拆解中的实例——随后尾部会清空它的播放器引用、平台视图通知器
  /// 与全局 service handler，新页面表现为黑屏、控件失效。标记后这些入口
  /// 立即丢弃该实例，让调用方拿到全新的实例。
  bool _disposeRequested = false;

  PlayerStatus playerStatus = .paused;

  final Rx<DataStatus> dataStatus = Rx(.none);

  Duration? seekToPos;
  bool hasToasted = false;
  final RxBool isSeeking = false.obs;

  final RxInt position = RxInt(0);
  final RxInt seekPosition = RxInt(0);
  int get progress => isSeeking.value ? seekPosition.value : position.value;

  int get positionInMilliseconds =>
      videoPlayerController?.state.position.inMilliseconds ?? 0;

  final RxInt buffered = RxInt(0);

  final RxInt duration = RxInt(0);

  int durationInMilliseconds = 0;

  void updateDuration(Duration value) {
    duration.value = value.inSeconds;
    durationInMilliseconds = value.inMilliseconds;
  }

  int _playerCount = 0;

  late double lastPlaybackSpeed = 1.0;
  final RxDouble _playbackSpeed = Pref.playSpeedDefault.obs;
  late final RxDouble _longPressSpeed = Pref.longPressSpeedDefault.obs;

  final RxDouble volume = RxDouble(
    PlatformUtils.isDesktop ? Pref.desktopVolume : 1.0,
  );

  /// 实际音量
  final actualVolume = 0.0.obs;
  final setSystemBrightness = Pref.setSystemBrightness;

  /// 系统音量（仅在应用内音量模式下用于追踪当前系统音量）
  final RxDouble systemVolume = RxDouble(1.0);

  /// duck 相关状态
  bool _isDucked = false;
  double _preDuckVolume = 1.0;
  final RxDouble brightness = (-1.0).obs;

  final RxBool showControls = false.obs;

  final RxBool showBrightnessStatus = false.obs;

  final RxBool longPressStatus = false.obs;

  /// 长按倍速锁定（仅当次播放生效，不持久化）
  bool speedLocked = false;

  /// 长按倍速锁定提示（六态，驱动播放器顶部 toast）
  final Rx<SpeedLockHint> speedLockHint = SpeedLockHint.none.obs;

  /// 长按中滑动越线后的预备态：松手才提交锁定/解锁
  bool _speedLockArmed = false;
  Timer? _speedLockHintTimer;

  final RxBool controlsLock = false.obs;

  final RxBool isFullScreen = false.obs;
  void Function(bool isFullScreen)? onFullScreenChanged;
  // 系统原生 PiP 状态
  final RxBool isNativePip = false.obs;
  bool isLive = false;

  bool _isVertical = false;

  final Rx<VideoFitType> videoFit = Rx(.contain);

  late final RxBool continuePlayInBackground =
      Pref.continuePlayInBackground.obs;

  bool _autoPlay = false;

  // 记录历史记录
  int? _aid;
  String? _bvid;
  int? cid;
  int? roomId;

  /// 当前数据源由哪个页面控制器建立。播放器是单例，而视频页 / 直播页可以叠加
  /// （从视频页再点开一个视频），链路变化这类被动事件必须据此判断自己是否仍是
  /// 播放器的持有者，否则后台的页面会把播放器抢回自己的源。
  Object? sourceOwner;
  int? _epid;
  int? _seasonId;
  int? _pgcType;
  VideoType _videoType = VideoType.ugc;
  int _heartDuration = 0;
  int? width;
  int? height;

  late final tryLook = !Accounts.get(AccountType.video).isLogin && Pref.p1080;

  late DataSource dataSource;

  Timer? _timer;
  StreamSubscription? _subForSeek;

  // 鸿蒙：mpv 的 ohaudio 音频输出感知不到系统音频打断（如其他 app 抢占焦点），
  // 被打断后 mpv 仍自认为在播放，进度停滞、UI 按钮状态错误且无法点击恢复。
  // 用位置停滞检测兜底：播放中且非缓冲时位置连续数秒不前进，视为被系统打断。
  Timer? _stallWatchdog;
  int? _stallLastPosition;
  int _stallTicks = 0;
  bool _audioInterrupted = false;

  Box setting = GStorage.setting;

  // final Durations durations;

  String get bvid => _bvid!;

  bool isCurrentVideoSource({
    required String bvid,
    required int cid,
  }) =>
      dataStatus.value == DataStatus.loaded && _bvid == bvid && this.cid == cid;

  /// 当前播放视频的标识、进度与播放状态快照，用于鸿蒙跨设备接续；
  /// 直播（由 LiveRoomController 提供快照）或无视频时为 null
  Map<String, dynamic>? get playSnapshot {
    if (isLive || _bvid == null || cid == null) {
      return null;
    }
    return {
      'type': 'video',
      'aid': _aid,
      'bvid': _bvid,
      'cid': cid,
      'epid': _epid,
      'seasonId': _seasonId,
      'pgcType': _pgcType,
      'videoType': _videoType.name,
      'progress': positionInMilliseconds,
      'playing': playerStatus.isPlaying,
      'onlyPlayAudio': onlyPlayAudio.value,
    };
  }

  /// 视频播放速度
  double get playbackSpeed => _playbackSpeed.value;

  // 长按倍速
  double get longPressSpeed => _longPressSpeed.value;

  /// [videoPlayerController] instance of Player
  Player? get videoPlayerController => _videoPlayerController;

  /// 底层 Player 是否已不可用。
  ///
  /// media_kit 的 NativePlayer 一旦 dispose，任何 API（play/pause/seek/
  /// setSubtitleTrack…）都会抛 `[Player] has been disposed`。页面退出、
  /// 换源期间异步链路仍可能继续跑，调用前先用它兜底。
  bool get playerDisposed {
    final platform = _videoPlayerController?.platform;
    return platform == null || (platform is NativePlayer && platform.disposed);
  }

  /// [videoController] instance of Player
  VideoController? get videoController => _videoControllerNotifier.value;

  bool isMuted = false;

  /// 听视频
  late final RxBool onlyPlayAudio = false.obs;

  /// 后台自动只听音频状态机
  AutoAudioOnlyState _autoAudioState = AutoAudioOnlyState.idle;
  Timer? _autoAudioOnlyTimer;
  bool _audioReopened = false;
  AppLifecycleState? _lastAppLifecycleState;
  bool _autoAudioRestoreInProgress = false;
  int _autoAudioRestoreGeneration = 0;
  Timer? _autoAudioRestoreTimer;
  StreamSubscription? _autoAudioRestoreSubscription;
  Completer<bool>? _autoAudioRestoreCompleter;
  final RxBool suppressBufferingIndicator = false.obs;

  /// 自动恢复需要重初始化的回调（视频→playerInit，直播→queryLiveUrl）
  PlayerInitCallback? onNeedsPlayerInit;

  /// 镜像
  late final RxBool flipX = false.obs;

  late final RxBool flipY = false.obs;

  final RxBool isBuffering = true.obs;

  /// 全屏方向
  // ignore: unnecessary_getters_setters
  bool get isVertical => _isVertical;

  set isVertical(bool value) {
    _isVertical = value;
  }

  /// 弹幕开关
  late final RxBool enableShowDanmaku = Pref.enableShowDanmaku.obs;
  late final RxBool enableShowLiveDanmaku = Pref.enableShowLiveDanmaku.obs;
  RxBool get enableShowDanmakuAdaptive =>
      isLive ? enableShowLiveDanmaku : enableShowDanmaku;

  late final bool autoPiP = Pref.autoPiP;
  bool get isPipMode =>
      isNativePip.value ||
      (Platform.isAndroid && AndroidHelper.isPipMode) ||
      ((Platform.isAndroid || OS.isHarmony) && Floating().isPipMode) ||
      (PlatformUtils.isDesktop && isDesktopPip);

  /// [isPipMode] 的响应式镜像（仅鸿蒙由 onPipModeChanged 驱动）。页面布局
  /// 依赖 isPipMode 时应同时监听它触发重建，否则 PiP 结束时若无视口变化
  /// 页面会滞留在画中画布局。
  final RxBool pipModeRx = false.obs;
  late bool isDesktopPip = false;
  late Rect _lastWindowBounds;

  late final showWindowTitleBar = Pref.showWindowTitleBar;
  late final RxBool isAlwaysOnTop = false.obs;
  Future<void> setAlwaysOnTop(bool value) {
    isAlwaysOnTop.value = value;
    return windowManager.setAlwaysOnTop(value);
  }

  Future<void> exitDesktopPip() {
    isDesktopPip = false;
    return Future.wait([
      if (showWindowTitleBar)
        windowManager.setTitleBarStyle(TitleBarStyle.normal),
      windowManager.setMinimumSize(const Size(400, 700)),
      windowManager.setBounds(_lastWindowBounds),
      setAlwaysOnTop(false),
      windowManager.setAspectRatio(0),
    ]);
  }

  Future<void> enterDesktopPip() async {
    if (isFullScreen.value) return;

    isDesktopPip = true;

    _lastWindowBounds = await windowManager.getBounds();

    if (showWindowTitleBar) {
      windowManager.setTitleBarStyle(TitleBarStyle.hidden);
    }

    const shortSide = 280.0;
    const minShortSide = 160.0;
    final Size size;
    final Size minimumSize;
    final state = videoPlayerController!.state;
    int width = state.width ?? this.width ?? 16;
    int height = state.height ?? this.height ?? 9;
    if (width == 0) {
      width = this.width ?? 16;
    }
    if (height == 0) {
      height = this.height ?? 9;
    }
    if (height > width) {
      size = Size(shortSide, shortSide * height / width);
      minimumSize = Size(minShortSide, minShortSide * height / width);
    } else {
      size = Size(shortSide * width / height, shortSide);
      minimumSize = Size(minShortSide * width / height, minShortSide);
    }

    await windowManager.setMinimumSize(minimumSize);
    setAlwaysOnTop(true);
    windowManager
      ..setSize(size)
      ..setAspectRatio(width / height);
  }

  void toggleDesktopPip() {
    if (isDesktopPip) {
      exitDesktopPip();
    } else {
      enterDesktopPip();
    }
  }

  late bool _isAutoEnterPip = false;
  bool get isAutoEnterPip => _isAutoEnterPip;

  static bool get _isCurrVideoPage {
    final routing = Get.routing;
    if (routing.route is! GetPageRoute) {
      return false;
    }
    return _isVideoPage(routing.current);
  }

  static bool _isVideoPage(String routeName) {
    return routeName == '/videoV' || routeName == '/liveRoom';
  }

  bool get _isInInAppPip {
    return PipOverlayService.isInPipMode || LivePipOverlayService.isInPipMode;
  }

  Future<PiPStatus> enterPip({bool autoEnter = false}) {
    // 鸿蒙「横屏小窗」（系统小窗内切全屏，见 HarmonyChannel.isMiniWindowLandscape）
    // 下手动进画中画只有黑屏：此时 floating 插件走 freeform 分支，先
    // moveAbilityToBackground 再 startPiP，PiP 窗口的内容节点（customUIController，
    // 由应用侧 ArkUI 承载）在应用已退到后台之后才创建——实测日志里主窗口
    // visibility 1→0 比 "PiP XComponent loaded" 还早，且首帧尺寸也是错的
    // （808x454vp，135ms 后才纠正为 413x232vp）。
    // 小窗内不切全屏时同样走 freeform 分支但画面正常，故拦截条件取横屏小窗而非小窗。
    // 这里直接拒绝，由调用方提示"当前处于系统小窗，无法进入画中画"。
    // 退后台自动进入（isAuto，走系统 auto-start，次序正常）不受影响。
    if (!autoEnter && OS.isHarmony && HarmonyChannel.isMiniWindowLandscape) {
      return Future.value(PiPStatus.unavailable);
    }
    if (videoPlayerController != null) {
      final state = videoPlayerController!.state;
      if (Platform.isIOS) {
        if (videoController?.id.value case final textureId?) {
          IOSPipHelper.enter(
            textureId,
            width: state.width == 0 ? width : state.width,
            height: state.height == 0 ? height : state.height,
            autoEnter: autoEnter,
            state: _iosPipState(state),
          );
          return Future.value(PiPStatus.enabled);
        }
        return Future.value(PiPStatus.unavailable);
      }
      return PageUtils.enterPip(
        isAuto: autoEnter,
        width: state.width == 0 ? width : state.width,
        height: state.height == 0 ? height : state.height,
      );
    }
    return Future.value(PiPStatus.unavailable);
  }

  // void _disableAutoEnterPipIfNeeded() {
  //   // 对齐上游逻辑，如果是从视频页返回到非视频页，则切断 Auto-Enter PiP
  //   if (!_isPreviousVideoPage) {
  //     _disableAutoEnterPip();
  //   }
  // }

  void disableAutoEnterPip() => _disableAutoEnterPip();

  void _disableAutoEnterPip() {
    if (_isAutoEnterPip) {
      if (OS.isHarmony) {
        Floating().setAutoPip(false);
      } else if (Platform.isIOS) {
        IOSPipHelper.disableAutoEnter();
      } else {
        PiliAndroidHelper.disableAutoEnterPip();
      }
    }
  }

  Map<String, Object> _iosPipState(PlayerState state, [Duration? position]) {
    return {
      'isPlaying': playerStatus.isPlaying,
      'isBuffering': isBuffering.value,
      'isLive': isLive,
      'position': (position ?? state.position).inMilliseconds,
      'duration': durationInMilliseconds,
      'speed': state.rate,
    };
  }

  void _updateIOSPip([Duration? position]) {
    if (Platform.isIOS && IOSPipHelper.needsUpdate) {
      if (_videoPlayerController case final player?) {
        IOSPipHelper.update(_iosPipState(player.state, position));
      }
    }
  }

  // 弹幕相关配置
  late final enableTapDm = PlatformUtils.isMobile && Pref.enableTapDm;
  late RuleFilter filters = Pref.danmakuFilterRule;
  // 关联弹幕控制器
  DanmakuController<DanmakuExtra>? danmakuController;

  /// 弹幕遮挡区（弹幕层坐标系）。PLVideoPlayer 的几何驱动写入，PlDanmaku 消费；
  /// 两端都以此对象为交接点，画布重建（切 P）时 PlDanmaku 从这里补读。
  final ValueNotifier<Path?> danmakuMaskPath = ValueNotifier(null);
  bool showDanmaku = true;
  Set<int> dmState = <int>{};
  late final mergeDanmaku = Pref.mergeDanmaku;
  late final String midHash = getCrc32(
    ascii.encode(Accounts.main.mid.toString()),
    0,
  ).toRadixString(16);
  late final RxDouble danmakuOpacity = Pref.danmakuOpacity.obs;

  late List<double> speedList = Pref.speedList;
  late bool enableAutoLongPressSpeed = Pref.enableAutoLongPressSpeed;
  late double longPressSpeedFactor = Pref.longPressSpeedFactor;
  late final showControlDuration = Pref.enableLongShowControl
      ? const Duration(seconds: 30)
      : const Duration(seconds: 3);
  // 字幕
  late double subtitleFontScale = Pref.subtitleFontScale;
  late double subtitleFontScaleFS = Pref.subtitleFontScaleFS;
  late int subtitlePaddingH = Pref.subtitlePaddingH;
  late int subtitlePaddingB = Pref.subtitlePaddingB;
  late double subtitleBgOpacity = Pref.subtitleBgOpacity;
  final bool showVipDanmaku = Pref.showVipDanmaku; // loop unswitching
  late double subtitleStrokeWidth = Pref.subtitleStrokeWidth;
  late int subtitleFontWeight = Pref.subtitleFontWeight;
  // 副字幕(位置边距与主字幕共用)
  late double subtitleSecondaryFontScale = Pref.subtitleSecondaryFontScale;
  late double subtitleSecondaryFontScaleFS = Pref.subtitleSecondaryFontScaleFS;
  late double subtitleSecondaryBgOpacity = Pref.subtitleSecondaryBgOpacity;
  late double subtitleSecondaryStrokeWidth = Pref.subtitleSecondaryStrokeWidth;
  late int subtitleSecondaryFontWeight = Pref.subtitleSecondaryFontWeight;
  late double subtitleSecondarySpacing = Pref.subtitleSecondarySpacing;

  // settings
  late final showFSActionItem = Pref.showFSActionItem;
  late final enableShrinkVideoSize = Pref.enableShrinkVideoSize;
  late final enablePinchRotate = Pref.enablePinchRotate;
  late final darkVideoPage = Pref.darkVideoPage;
  late final enableSlideVolumeBrightness = Pref.enableSlideVolumeBrightness;
  late final enableSlideFS = Pref.enableSlideFS;
  late final enableDragSubtitle = Pref.enableDragSubtitle;
  late final fastForBackwardDuration = Duration(
    seconds: Pref.fastForBackwardDuration,
  );

  late final horizontalSeasonPanel = Pref.horizontalSeasonPanel;
  late final preInitPlayer = Pref.preInitPlayer;
  late final showRelatedVideo = Pref.showRelatedVideo;
  late final showVideoReply = Pref.showVideoReply;
  late final showBangumiReply = Pref.showBangumiReply;
  late final reverseFromFirst = Pref.reverseFromFirst;
  late final horizontalPreview = Pref.horizontalPreview;
  DmChartSource get dmChartSource => Pref.dmChartSource;
  bool get showDmChart => dmChartSource.isEnabled;
  late final showViewPoints = Pref.showViewPoints;
  late final showFsScreenshotBtn = Pref.showFsScreenshotBtn;
  late final showFsLockBtn = Pref.showFsLockBtn;
  late final showFsLockBtnRight = Pref.showFsLockBtnRight;
  late final lockFsRotation = Pref.lockFsRotation;
  late final keyboardControl = Pref.keyboardControl;
  late final uiScale = Pref.uiScale;

  late final bool autoEnterFullScreen = Pref.autoEnterFullScreen;
  late final bool autoExitFullscreen = Pref.autoExitFullscreen;
  late final bool autoPlayEnable = Pref.autoPlayEnable;
  late final bool enableVerticalExpand = Pref.enableVerticalExpand;
  late final bool pipNoDanmaku = Pref.pipNoDanmaku;

  late final bool tempPlayerConf = Pref.tempPlayerConf;

  static const int videoPictureParameterMin = -100;
  static const int videoPictureParameterMax = 100;
  static const int audioDelayMin = -1000;
  static const int audioDelayMax = 1000;
  static const int audioDelayStep = 10;
  static const Duration _videoPictureSaveDelay = Duration(milliseconds: 300);

  late final RxInt videoBrightness =
      (tempPlayerConf ? 0 : Pref.videoBrightness).obs;
  late final RxInt videoContrast =
      (tempPlayerConf ? 0 : Pref.videoContrast).obs;
  late final RxInt videoSaturation =
      (tempPlayerConf ? 0 : Pref.videoSaturation).obs;
  late final RxInt videoGamma = (tempPlayerConf ? 0 : Pref.videoGamma).obs;
  late final RxInt videoHue = (tempPlayerConf ? 0 : Pref.videoHue).obs;
  late final RxInt audioDelayMs = (tempPlayerConf ? 0 : Pref.audioDelayMs).obs;
  Timer? _videoPictureSaveTimer;

  late int? cacheVideoQa = PlatformUtils.isMobile ? null : Pref.defaultVideoQa;
  late int cacheAudioQa = Pref.defaultAudioQa;
  bool enableHeart = true;
  late final String? hwdec = Pref.enableHA ? Pref.hardwareDecoding : null;

  late final bool enableHDR = Pref.enableHDR;

  /// 当前播放源的画质，用于判断是否为 HDR 片源。由 [setDataSource] 传入。
  VideoQuality? _hdrQuality;

  /// 鸿蒙上现在需要的视频输出，规则见 [OhosHdrOutput.output]；其他平台为 null。
  ///
  /// 平台视图没有单独的开关：关掉它等于关掉 HDR（元数据会在 Flutter 纹理那一步
  /// 丢光），所以它不是一个用户能做的取舍，跟随「启用 HDR 视频」即可。
  ///
  /// 只有**系统画中画**要退回 SDR，规则见 [OhosHdrOutput.needsSdrOutput]：
  /// [isNativePip] 由 floating 插件的 `onPipModeChanged` 经 `syncNativePip` 同步，
  /// 那时小窗由插件另起一块 XComponent 承载整个 Flutter 界面，平台视图跟不过去，
  /// 留在平台视图上小窗里只有空白。
  ///
  /// **应用内小窗不在此列**：它在应用自己的 Overlay 里，仍在同一个 Flutter 视图
  /// 内，平台视图照常合成，所以那里继续走 HDR；进出应用内小窗也不再重建播放器
  /// （[notifyInAppPipChanged] 因此变成空转，输出根本没变）。
  /// 不要用 [isPipMode] 或 [_isInInAppPip] 去驱动这条规则：前者含应用内小窗以外
  /// 的多种情况，后者恰好把该保留 HDR 的应用内小窗当成要降级的那一种。
  OhosOutput? get _ohosOutput => OS.isHarmony
      ? OhosHdrOutput.output(
          hdrEnabled: enableHDR,
          quality: _hdrQuality,
          displayHasNoHdr: HarmonyChannel.displayHasNoHdr,
          displayMaySupportVivid: HarmonyChannel.displayMaySupportHdrVivid,
          pipMode: OhosHdrOutput.needsSdrOutput(
            nativePiP: isNativePip.value,
            inAppPiP: _isInInAppPip,
          ),
          peakNits: Pref.hdrPeakNits,
          matchVideo: Pref.hdrMatchVideoPeak,
        )
      : null;

  /// 当前播放器创建时用的输出。
  ///
  /// 这些只在创建 Player 时写一次，与 [_ohosOutput] 不一致就得重建播放器：在
  /// SDR 与 HDR 片源之间切换、进出画中画、面板能力在首个视频打开后才查到，或是
  /// 改了峰值亮度设置。几档 HDR 之间切换得到的是同一个输出，不会重建。
  OhosOutput? _appliedOutput;

  late final progressType = Pref.btmProgressBehavior;
  late final enableQuickDouble = Pref.enableQuickDouble;
  late final fullScreenGestureReverse = Pref.fullScreenGestureReverse;

  late final isRelative = Pref.useRelativeSlide;
  late final offset = isRelative
      ? Pref.sliderDuration / 100
      : Pref.sliderDuration * 1000;

  num get sliderScale => isRelative ? durationInMilliseconds * offset : offset;

  // 播放顺序相关
  late PlayRepeat playRepeat = Pref.playRepeat;

  TextStyle _buildSubtitleStyle({
    required double fontScale,
    required double fontScaleFS,
    required int fontWeight,
    required double bgOpacity,
  }) => TextStyle(
    height: 1.5,
    fontSize: 16 * (isFullScreen.value ? fontScaleFS : fontScale),
    letterSpacing: 0.1,
    wordSpacing: 0.1,
    color: Colors.white,
    fontWeight: FontWeight.values[fontWeight],
    backgroundColor: bgOpacity == 0
        ? null
        : Colors.black.withValues(alpha: bgOpacity),
  );

  TextStyle get subTitleStyle => _buildSubtitleStyle(
    fontScale: subtitleFontScale,
    fontScaleFS: subtitleFontScaleFS,
    fontWeight: subtitleFontWeight,
    bgOpacity: subtitleBgOpacity,
  );

  TextStyle get subTitleSecondaryStyle => _buildSubtitleStyle(
    fontScale: subtitleSecondaryFontScale,
    fontScaleFS: subtitleSecondaryFontScaleFS,
    fontWeight: subtitleSecondaryFontWeight,
    bgOpacity: subtitleSecondaryBgOpacity,
  );

  late final Rx<SubtitleViewConfiguration> subtitleConfig = getSubConfig.obs;
  String? _activeVideoContextKey;

  SubtitleViewConfiguration get getSubConfig {
    final subTitleStyle = this.subTitleStyle;
    final secondaryStyle = subTitleSecondaryStyle;

    // 无背景时用描边保证可读性,主副字幕各自独立
    TextStyle? strokeOf(TextStyle base, double bgOpacity, double strokeWidth) =>
        bgOpacity == 0
        ? base.copyWith(
            color: null,
            background: null,
            backgroundColor: null,
            foreground: Paint()
              ..color = Colors.black
              ..style = PaintingStyle.stroke
              ..strokeWidth = strokeWidth,
          )
        : null;

    return SubtitleViewConfiguration(
      style: subTitleStyle,
      strokeStyle: strokeOf(
        subTitleStyle,
        subtitleBgOpacity,
        subtitleStrokeWidth,
      ),
      secondaryStyle: secondaryStyle,
      secondaryStrokeStyle: strokeOf(
        secondaryStyle,
        subtitleSecondaryBgOpacity,
        subtitleSecondaryStrokeWidth,
      ),
      spacing: subtitleSecondarySpacing,
      padding: EdgeInsets.only(
        left: subtitlePaddingH.toDouble(),
        right: subtitlePaddingH.toDouble(),
        bottom: subtitlePaddingB.toDouble(),
      ),
      textScaler: TextScaler.noScaling,
    );
  }

  void updateSubtitleStyle() {
    subtitleConfig.value = getSubConfig;
  }

  void onUpdatePadding(EdgeInsets padding) {
    subtitlePaddingB = padding.bottom.round().clamp(0, 200);
    putSubtitleSettings();
  }

  static PlPlayerController? get instance => _instance;

  static bool instanceExists() {
    return _instance != null;
  }

  static void setPlayCallBack(
    PlayCallback? playCallBack, {
    PlayOwner? playOwner,
  }) {
    _playCallBack = playCallBack;
    _playOwner = playOwner;
  }

  static PlayOwner? _playOwner;
  static PlayOwner? get playOwner => _playOwner;
  static PlayCallback? _playCallBack;

  static Future<void>? playIfExists() {
    if (_instance != null && !(_instance!.playerStatus.isPlaying)) {
      return _instance!.play();
    }
    return _playCallBack?.call();
  }

  // try to get PlayerStatus
  static PlayerStatus? getPlayerStatusIfExists() {
    return _instance?.playerStatus;
  }

  static Future<void>? pauseIfExists({
    bool notify = true,
    bool isInterrupt = false,
  }) {
    if (_instance?.playerStatus.isPlaying ?? false) {
      return _instance?.pause(notify: notify, isInterrupt: isInterrupt);
    }
    return null;
  }

  static Future<void>? seekToIfExists(
    Duration position, {
    bool isSeek = true,
  }) {
    return _instance?.seekTo(position, isSeek: isSeek);
  }

  static double? getVolumeIfExists() {
    return _instance?.volume.value;
  }

  static Future<void>? setVolumeIfExists(
    double volumeNew, {
    bool showIndicator = true,
  }) {
    return _instance?.setVolume(volumeNew, showIndicator: showIndicator);
  }

  Box video = GStorage.video;

  bool visible = true;

  DeviceOrientation? _orientation;
  StreamSubscription<OrientationParams>? _orientationListener;
  // 对齐上游 ：监听旋转状态，Android/鸿蒙由checkIsAutoRotate原生读取系统旋转设置
  bool get checkIsAutoRotate =>
      (Platform.isAndroid || OS.isHarmony) && mode != .gravity;

  /// 传感器报竖屏后，延迟确认再自动退出全屏的定时器。
  ///
  /// 鸿蒙侧的方向分类按 pitch/roll 判定（native_device_orientation fork 的
  /// SensorOrientationListener），在侧躺、斜举、接近平放这些边界姿态下会偶发
  /// 单帧误报竖屏。收到即退出全屏的话，紧接着下一帧回到横屏又会被 landscape
  /// 分支自动拉回全屏，表现为「全屏自己退出又进入」。这里等设备确实稳定在
  /// 竖屏之后再退，单帧误报会被后续的横屏事件取消掉。
  Timer? _autoExitFsTimer;

  /// 自动退出全屏前的确认时长。取值需大于误报的持续时间（几十~几百毫秒），
  /// 同时又不至于让「真的转回竖屏」的退出手感明显变迟钝。
  static const _autoExitFsDelay = Duration(milliseconds: 500);

  void _cancelAutoExitFs() {
    _autoExitFsTimer?.cancel();
    _autoExitFsTimer = null;
  }

  /// 延迟确认后自动退出全屏。确认时重新校验全部前置条件，期间发生的任何变化
  /// （转回横屏、手动进/退全屏、锁定控件、换成竖屏视频）都会让本次退出作废。
  void _scheduleAutoExitFullScreen(DeviceOrientation orientation) {
    _cancelAutoExitFs();
    _autoExitFsTimer = Timer(_autoExitFsDelay, () {
      _autoExitFsTimer = null;
      if ((OS.isHarmony && HarmonyChannel.isWindowMode) ||
          _orientation != orientation ||
          !isFullScreen.value ||
          isManualFS ||
          horizontalScreen ||
          _isVertical ||
          controlsLock.value) {
        return;
      }
      // 自动退出必须显式传 isManualFS: false，否则会按默认值记成手动退出：
      // 既翻错 isManualFS 的语义，也会白白开启 600ms 的自动进全屏抑制窗。
      triggerFullScreen(
        status: false,
        orientation: orientation,
        isManualFS: false,
      );
    });
  }

  void _stopOrientationListener() {
    _orientationListener?.cancel();
    _orientationListener = null;
    _cancelAutoExitFs();
  }

  void _onOrientationChanged(OrientationParams param) {
    final deviceOrientation = param.orientation;
    if (deviceOrientation == null) return;
    _orientation = deviceOrientation;
    if (OS.isHarmony && HarmonyChannel.isWindowMode) {
      _cancelAutoExitFs();
      return;
    }
    if (Platform.isIOS && !visible) return;
    final isFullScreen = this.isFullScreen.value;
    if (checkIsAutoRotate &&
        param.isAutoRotate != true &&
        (!isFullScreen ||
            _isVertical ||
            deviceOrientation == .portraitUp ||
            deviceOrientation == .portraitDown)) {
      return;
    }
    switch (deviceOrientation) {
      case .portraitUp:
        if (!_isVertical && controlsLock.value) return;
        if (!horizontalScreen && !_isVertical && isFullScreen) {
          if (!isManualFS) {
            // 自动进的全屏才跟随设备转回竖屏而退出；延迟确认，避免单帧误报
            _scheduleAutoExitFullScreen(deviceOrientation);
          }
        } else {
          _cancelAutoExitFs();
          portraitUpMode();
        }
      case .portraitDown:
        if (!horizontalScreen) return;
        if (!_isVertical && controlsLock.value) return;
        _cancelAutoExitFs();
        portraitDownMode();
      case .landscapeLeft:
        _cancelAutoExitFs();
        if (controlsLock.value && (lockFsRotation || _isVertical)) return;
        if (!horizontalScreen && !isFullScreen) {
          triggerFullScreen(orientation: deviceOrientation, isManualFS: false);
        } else {
          landscapeLeftMode();
        }
      case .landscapeRight:
        _cancelAutoExitFs();
        if (controlsLock.value && (lockFsRotation || _isVertical)) return;
        if (!horizontalScreen && !isFullScreen) {
          triggerFullScreen(orientation: deviceOrientation, isManualFS: false);
        } else {
          landscapeRightMode();
        }
    }
  }

  // 添加一个私有构造函数
  PlPlayerController._() {
    if (PlatformUtils.isMobile) {
      _orientationListener = NativeDeviceOrientationCommunicator()
          .onOrientationChanged(
            checkIsAutoRotate: checkIsAutoRotate,
            angleDegrees: Pref.angleDegrees,
          )
          .listen(_onOrientationChanged);
    }

    if (!Accounts.heartbeat.isLogin || Pref.historyPause) {
      enableHeart = false;
    }

    if (Platform.isAndroid) {
      // onPipChanged 回调是本 fork 的系统 PiP 状态同步通道（上游没有），
      // 与下方的 autoPiP 注册共用同一个 MethodChannel handler
      Utils.channel.setMethodCallHandler((call) async {
        if (call.method == 'onPipChanged') {
          final bool isInPip = call.arguments as bool;
          isNativePip.value = isInPip;
          PipOverlayService.isNativePip = isInPip;
          LivePipOverlayService.isNativePip = isInPip;
          handleAutoAudioOnlyPipChanged(isInPip);
        }
      });

      if (autoPiP) {
        if (DeviceUtils.sdkInt < 31) {
          final func = Runnable.implement($Runnable(run: _onUserLeaveHint));
          AndroidHelper$ToDart.onUserLeaveHint = func;
          func.release();
        } else {
          _isAutoEnterPip = true;
        }
      }
    } else if (autoPiP &&
        (OS.isHarmony || (Platform.isIOS && IOSPipHelper.isAvailable))) {
      _isAutoEnterPip = true;
    }
  }

  void _onUserLeaveHint() {
    if (_isInInAppPip) {
      enterPip();
      return;
    }
    if (playerStatus.isPlaying && _isCurrVideoPage) {
      enterPip();
    }
  }

  // 获取实例 传参
  static PlPlayerController getInstance({bool isLive = false}) {
    _dropDisposedInstance();
    // 如果实例尚未创建，则创建一个新实例
    return (_instance ??= PlPlayerController._())
      ..isLive = isLive
      .._playerCount += 1;
  }

  static PlPlayerController ensureInstance({bool isLive = false}) {
    _dropDisposedInstance();
    return (_instance ??= PlPlayerController._())..isLive = isLive;
  }

  /// dispose() 是异步收尾，尾部才把 _instance 置空；其间任何获取实例的调用
  /// （新视频页构造、playerInit 重取等）都会领走仍在拆解中的僵尸实例，收尾
  /// 随后清空其播放器/通知器/静态引用 → 新页面黑屏、控件失效。
  /// 发现标记即丢弃引用，让调用方拿到全新实例。
  static void _dropDisposedInstance() {
    if (_instance?._disposeRequested ?? false) {
      _instance = null;
    }
  }

  static bool _isAnimPgcType(int? pgcType) => pgcType == 1 || pgcType == 4;

  void resetTempSettings({int? nextPgcType}) {
    if (!tempPlayerConf) {
      return;
    }

    if (kDebugMode) {
      debugPrint(
        '[PlPlayer] resetTempSettings (currentContext: $_activeVideoContextKey, nextPgcType: $nextPgcType)',
      );
    }

    enableShowDanmaku.value = Pref.enableShowDanmaku;
    enableShowLiveDanmaku.value = Pref.enableShowLiveDanmaku;
    videoPlayerServiceHandler?.enableBackgroundPlay = Pref.enableBackgroundPlay;
    continuePlayInBackground.value = Pref.continuePlayInBackground;
    playRepeat = Pref.playRepeat;
    cacheVideoQa = PlatformUtils.isMobile ? null : Pref.defaultVideoQa;
    cacheAudioQa = Pref.defaultAudioQa;
    if (_playbackSpeed.value != playSpeedDefault) {
      unawaited(setPlaybackSpeed(playSpeedDefault));
    }

    final defaultSuperResolutionType = _isAnimPgcType(nextPgcType)
        ? Pref.superResolutionType
        : SuperResolutionType.disable;
    superResolutionType.value = defaultSuperResolutionType;
    if (_videoPlayerController != null) {
      unawaited(setShader(defaultSuperResolutionType));
    }

    resetAllVideoPictureParameters(persist: false);
  }

  int _normalizeVideoPictureParameter(num value) => value
      .round()
      .clamp(
        videoPictureParameterMin,
        videoPictureParameterMax,
      )
      .toInt();

  RxInt? _videoPictureParameterState(String property) => switch (property) {
    'brightness' => videoBrightness,
    'contrast' => videoContrast,
    'saturation' => videoSaturation,
    'gamma' => videoGamma,
    'hue' => videoHue,
    _ => null,
  };

  void setVideoPictureParameter(String property, num value) {
    final state = _videoPictureParameterState(property);
    if (state == null) return;

    final normalized = _normalizeVideoPictureParameter(value);
    state.value = normalized;
    _applyVideoPictureProperty(property, normalized);
    _scheduleVideoPictureSettingsSave();
  }

  void resetVideoPictureParameter(String property) {
    setVideoPictureParameter(property, 0);
  }

  void setAudioDelay(int value) {
    audioDelayMs.value = value.clamp(audioDelayMin, audioDelayMax).toInt();
    _applyAudioDelay();
    _scheduleVideoPictureSettingsSave();
  }

  void resetAudioDelay() => setAudioDelay(0);

  void _applyAudioDelay() {
    final player = _videoPlayerController;
    if (player == null || onlyPlayAudio.value) return;
    try {
      player.setProperty('audio-delay', (audioDelayMs.value / 1000).toString());
    } catch (_) {}
  }

  void resetAllVideoPictureParameters({bool persist = true}) {
    videoBrightness.value = 0;
    videoContrast.value = 0;
    videoSaturation.value = 0;
    videoGamma.value = 0;
    videoHue.value = 0;
    audioDelayMs.value = 0;
    applyVideoPictureParameters();
    if (persist) {
      _scheduleVideoPictureSettingsSave();
    }
  }

  void _applyVideoPictureProperty(String property, int value) {
    final player = _videoPlayerController;
    if (player == null || onlyPlayAudio.value) return;
    try {
      player.setProperty(property, value.toString());
    } catch (_) {}
  }

  void applyVideoPictureParameters([Player? target]) {
    final player = target ?? _videoPlayerController;
    if (player == null || onlyPlayAudio.value) return;
    try {
      player
        ..setProperty('brightness', videoBrightness.value.toString())
        ..setProperty('contrast', videoContrast.value.toString())
        ..setProperty('saturation', videoSaturation.value.toString())
        ..setProperty('gamma', videoGamma.value.toString())
        ..setProperty('hue', videoHue.value.toString())
        ..setProperty('audio-delay', (audioDelayMs.value / 1000).toString());
    } catch (_) {}
  }

  void _scheduleVideoPictureSettingsSave() {
    if (tempPlayerConf) return;
    _videoPictureSaveTimer?.cancel();
    _videoPictureSaveTimer = Timer(
      _videoPictureSaveDelay,
      persistVideoPictureSettings,
    );
  }

  void persistVideoPictureSettings() {
    _videoPictureSaveTimer?.cancel();
    _videoPictureSaveTimer = null;
    if (tempPlayerConf) return;
    setting.putAllNE({
      SettingBoxKey.videoBrightness: videoBrightness.value,
      SettingBoxKey.videoContrast: videoContrast.value,
      SettingBoxKey.videoSaturation: videoSaturation.value,
      SettingBoxKey.videoGamma: videoGamma.value,
      SettingBoxKey.videoHue: videoHue.value,
      SettingBoxKey.audioDelayMs: audioDelayMs.value,
    });
  }

  bool _processing = false;
  bool get processing => _processing;

  // offline
  bool get isFileSource => dataSource is FileSource;

  // 初始化资源
  //
  // 播放器是全局单例，多个视频页共享同一实例。setDataSource 内部会异步
  // 创建 media_kit Player（_initPlayer）。若并发调用（如 preInitPlayer
  // 页面初始化进行中，另一个 autoPlay 页面又进入——autoPlay 分支不检查
  // processing），会同时创建多个 Player，后创建的覆盖先创建的，先创建的
  // 失去引用后永不释放，造成原生播放器与流监听泄漏。这里用 Future 队列
  // 将初始化串行化，保证同一时刻只有一个 _setDataSource 在执行。
  Future<void> setDataSource(
    DataSource dataSource, {
    bool isLive = false,
    bool autoplay = true,
    // 初始化播放位置
    Duration? seekTo,
    // 初始化播放速度
    double speed = 1.0,
    int? width,
    int? height,
    Duration? duration,
    // 方向
    bool? isVertical,
    // 记录历史记录
    int? aid,
    String? bvid,
    int? cid,
    int? roomId,
    int? epid,
    int? seasonId,
    int? pgcType,
    VideoType? videoType,
    VoidCallback? onInit,
    Volume? volume,
    bool autoFullScreenFlag = false,
    // 当前片源画质，用于判断是否需要按 HDR 输出
    VideoQuality? quality,
  }) {
    return _playerQueue.run(
      () => _setDataSource(
        dataSource,
        isLive: isLive,
        autoplay: autoplay,
        seekTo: seekTo,
        speed: speed,
        width: width,
        height: height,
        duration: duration,
        isVertical: isVertical,
        aid: aid,
        bvid: bvid,
        cid: cid,
        epid: epid,
        seasonId: seasonId,
        pgcType: pgcType,
        videoType: videoType,
        onInit: onInit,
        volume: volume,
        autoFullScreenFlag: autoFullScreenFlag,
        quality: quality,
      ),
    );
  }

  Future<void> _setDataSource(
    DataSource dataSource, {
    bool isLive = false,
    bool autoplay = true,
    // 初始化播放位置
    Duration? seekTo,
    // 初始化播放速度
    double speed = 1.0,
    int? width,
    int? height,
    Duration? duration,
    // 方向
    bool? isVertical,
    // 记录历史记录
    int? aid,
    String? bvid,
    int? cid,
    int? epid,
    int? seasonId,
    int? pgcType,
    VideoType? videoType,
    VoidCallback? onInit,
    Volume? volume,
    bool autoFullScreenFlag = false,
    // 当前片源画质，用于判断是否需要按 HDR 输出
    VideoQuality? quality,
  }) async {
    try {
      _processing = true;
      // 换视频/换P：解除倍速锁定并恢复锁定前速度。
      // 必须先于临时配置重置执行，否则 lastPlaybackSpeed 会被先行覆盖
      await releaseSpeedLock();
      final nextVideoContextKey = PipOverlayService.buildVideoContextKey(
        videoType: videoType ?? VideoType.ugc,
        bvid: bvid,
        cid: cid,
        epId: epid,
        seasonId: seasonId,
      );
      final shouldResetTempSettings =
          tempPlayerConf &&
          _activeVideoContextKey != null &&
          nextVideoContextKey != null &&
          nextVideoContextKey != _activeVideoContextKey;
      if (shouldResetTempSettings) {
        resetTempSettings(nextPgcType: pgcType);
      }
      _activeVideoContextKey = nextVideoContextKey;

      // 换视频上下文时取消自动只听定时器，状态保持
      _cancelAutoAudioOnlyTimer();
      _interruptAutoAudioRestore();
      if (_autoAudioState == AutoAudioOnlyState.autoAudioOnly) {
        _audioReopened = true;
      }
      this.isLive = isLive;
      _videoType = videoType ?? VideoType.ugc;
      this.width = width;
      this.height = height;
      this.dataSource = dataSource;
      _autoPlay = autoplay;
      // 初始化数据加载状态
      dataStatus.value = DataStatus.loading;
      // 初始化全屏方向
      _isVertical = isVertical ?? false;
      _aid = aid;
      _bvid = bvid;
      this.cid = cid;
      this.roomId = roomId;
      _epid = epid;
      _seasonId = seasonId;
      _pgcType = pgcType;
      // 必须在创建 VideoController 之前赋值：输出方式（平台视图 / HDR 信令）
      // 在播放器初始化时就要确定。
      _hdrQuality = quality;
      if (!isLive && bvid != null && cid != null) {
        HarmonyChannel.holdContinuation(this);
      } else {
        HarmonyChannel.releaseContinuation(this);
      }

      if (showSeekPreview) {
        _clearPreview();
      }
      hideDesktopProgressPreview();
      cancelLongPressTimer();
      if (_videoPlayerController != null &&
          _videoPlayerController!.state.playing) {
        await pause(notify: false);
      }

      if (_playerCount == 0) {
        if (kDebugMode) {
          debugPrint(
            '[PlPlayer] setDataSource aborted: _playerCount == 0 before init',
          );
        }
        return;
      }
      // 配置Player 音轨、字幕等等
      await _createVideoController(dataSource, seekTo, volume);

      if (_playerCount == 0) {
        if (kDebugMode) {
          debugPrint(
            '[PlPlayer] setDataSource aborted: _playerCount == 0 after createVideoController',
          );
        }
        _removeListeners();
        await _disposePlayerSafely(_videoPlayerController);
        _videoPlayerController = null;
        _videoControllerNotifier.value = null;
        return;
      }

      updateDuration(duration ?? _videoPlayerController!.state.duration);
      position.value = buffered.value = seekTo?.inSeconds ?? 0;

      dataStatus.value = .loaded;
      if (_canUseSeekPreview) {
        unawaited(ensureVideoShot());
      }

      if (autoFullScreenFlag && autoEnterFullScreen) {
        triggerFullScreen(status: true);
      }

      await _initializePlayer();
      onInit?.call();
    } catch (err, stackTrace) {
      dataStatus.value = DataStatus.error;
      if (kDebugMode) {
        debugPrint(stackTrace.toString());
        debugPrint('plPlayer err:  $err');
      }
    } finally {
      _processing = false;
    }
  }

  String? shadersDirPath;
  Future<String> get copyShadersToExternalDirectory async {
    if (shadersDirPath != null) {
      return shadersDirPath!;
    }

    return shadersDirPath = await AssetUtils.getOrCopy(
      'assets/shaders',
      Assets.mpvAnime4KShaders.followedBy(Assets.mpvAnime4KShadersLite),
      path.join(appSupportDirPath, 'anime_shaders'),
    );
  }

  late final isAnim = _pgcType == 1 || _pgcType == 4;
  late final Rx<SuperResolutionType> superResolutionType =
      (isAnim ? Pref.superResolutionType : SuperResolutionType.disable).obs;
  Future<void> setShader([SuperResolutionType? type, NativePlayer? pp]) async {
    if (type == null) {
      type = superResolutionType.value;
    } else {
      superResolutionType.value = type;
      if (isAnim && !tempPlayerConf) {
        setting.put(SettingBoxKey.superResolutionType, type.index);
      }
    }
    pp ??= _videoPlayerController!.platform!.maybeAsNativePlayer;
    await pp.waitForPlayerInitialization;
    await pp.waitForVideoControllerInitializationIfAttached;
    switch (type) {
      case SuperResolutionType.disable:
        return pp.command(const ['change-list', 'glsl-shaders', 'clr', '']);
      case SuperResolutionType.efficiency:
        return pp.command([
          'change-list',
          'glsl-shaders',
          'set',
          PathUtils.buildShadersAbsolutePath(
            await copyShadersToExternalDirectory,
            Assets.mpvAnime4KShadersLite,
          ),
        ]);
      case SuperResolutionType.quality:
        return pp.command([
          'change-list',
          'glsl-shaders',
          'set',
          PathUtils.buildShadersAbsolutePath(
            await copyShadersToExternalDirectory,
            Assets.mpvAnime4KShaders,
          ),
        ]);
    }
  }

  Future<Player> _initPlayer(OhosOutput? output) async {
    assert(_videoPlayerController == null);
    if (PlatformUtils.isMobile && Pref.enableAppVolume) {
      // 移动平台应用内音量模式：初始化系统音量
      systemVolume.value = (await FlutterVolumeController.getVolume()) ?? 1.0;
      // 从持久化存储读取应用内音量
      volume.value = Pref.appVolume;
    }
    final opt = {
      'video-sync': Pref.videoSync,
      if (Platform.isAndroid) 'ao': Pref.audioOutput,
      'stream-lavf-o': 'reconnect=1',
      'volume':
          (PlatformUtils.isMobile
                  ? (Pref.enableAppVolume
                        ? volume.value * 100
                        : Pref.playerVolume)
                  : volume.value * 100)
              .toString(),
      'volume-max': kMaxVolume.toString(),
      // 按设定峰值色调映射时测量片源的实际峰值，盖掉 media-kit 默认的 no
      if (OhosHdrOutput.measuresPeak(output)) 'hdr-compute-peak': 'yes',
    };
    final autosync = Pref.autosync;
    if (autosync != '0') {
      opt['autosync'] = autosync;
    }

    final player = Player(
      configuration: const PlayerConfiguration(
        logLevel: kDebugMode ? .warn : .error,
      ),
    );

    final pp = player.platform!.maybeAsNativePlayer;
    for (var o in opt.entries) {
      pp.setProperty(o.key, o.value);
    }

    assert(videoController == null);

    _videoControllerNotifier.value = VideoController(
      player,
      configuration: VideoControllerConfiguration(
        enableHardwareAcceleration: hwdec != null,
        androidAttachSurfaceAfterVideoParameters: false,
        hwdec: hwdec,
        usePlatformView: output?.platformView ?? false,
        ohosHdrMode: output?.hdrMode,
        ohosHdrTargetPeak: output?.targetPeak,
      ),
    );
    // await player.setAudioTrack(.auto());

    if (Platform.isAndroid) {
      // Avoid sizing the surface using mpv's idle window between media loads.
      player.setProperty('force-window', 'no');
    }

    _startListeners(player.platform!.maybeAsNativePlayer);

    return player;
  }

  Map<String, String>? _buffer;
  Map<String, String> get buffer =>
      _buffer ??= Pref.initBuffer(_playbackSpeed.value);
  Map<String, String>? _liveBuffer;
  Map<String, String> get liveBuffer => _liveBuffer ??= Pref.initLiveBuffer();

  // 配置播放器
  Future<void> _createVideoController(
    DataSource dataSource,
    Duration? seekTo,
    Volume? volume, {
    // 渲染路径重建是同一个片源接着播，屏上的弹幕和心跳进度都要留着
    bool isRebuild = false,
  }) async {
    isBuffering.value = false;
    _heartDuration = 0;
    position.value = seekTo?.inSeconds ?? 0;
    buffered.value = 0;
    danmakuController?.clear();
    if (!isRebuild) {
      _heartDuration = 0;
      danmakuController?.clear();
    }

    var player = _videoPlayerController;

    // 输出在 VideoController 创建时就固定了，改不了，与现在需要的不一致就必须
    // 重建播放器，见 [_appliedOutput]。
    if (player != null && _appliedOutput != _ohosOutput) {
      _removeListeners();
      await _disposePlayerSafely(player);
      player = null;
      _videoPlayerController = null;
      _videoControllerNotifier.value = null;
    }

    if (player == null) {
      final output = _ohosOutput;
      _appliedOutput = output;
      player = await _initPlayer(output);
      if (_playerCount == 0) {
        _removeListeners();
        await _disposePlayerSafely(player);
        player = null;
        _videoControllerNotifier.value = null;
        return;
      }
      _videoPlayerController = player;
      if (isAnim && superResolutionType.value != .disable) {
        await setShader();
      }
    }

    final Map<String, String> extras = {};

    if (dataSource is FileSource) {
      extras['cache'] = 'no';
    } else {
      if (isLive) {
        extras.addAll(liveBuffer);
      } else {
        extras.addAll(buffer);
      }
    }

    String video = dataSource.videoSource;
    if (dataSource.audioSource case final audio? when (audio.isNotEmpty)) {
      if (onlyPlayAudio.value) {
        video = audio;
      } else {
        var audioUri = Platform.isWindows
            ? audio.replaceAll(';', '\\;')
            : audio.replaceAll(':', '\\:');
        extras['audio-files'] = audioUri;
        player.platform!.maybeAsNativePlayer.setProperty(
          'audio-files',
          audioUri,
        );
      }
      audioFilterExtras(volume, map: extras);
    } else {
      // 修复 Bug：从视频页返回直播间后，声音变成刚才视频的声音。
      // 原因：当前版本改用 NativePlayer.setProperty 直接设置 audio-files，
      // 该属性会持久化在复用的 Player 实例上；而 2.0.1 及之前版本是通过
      // Media.extras 传入 audio-files，每次 player.open 都会随新 Media 重置。
      // 当切换到无单独音频源的内容（如直播、关闭听视频）时，必须主动清空
      // audio-files，否则旧视频的外部音轨会继续播放，导致画面与声音不一致。
      player.platform!.maybeAsNativePlayer.setProperty('audio-files', '');
    }

    assert(!isLive || seekTo == null);
    await player.open(
      Media(
        video,
        httpHeaders: {
          'user-agent': BrowserUa.pc,
          'referer': HttpString.baseUrl,
        },
        start: seekTo,
        extras: extras.isEmpty ? null : extras,
      ),
      play: false,
    );
    applyVideoPictureParameters(player);
  }

  Future<bool?> refreshPlayer() async {
    if (dataSource is FileSource) {
      return null;
    }
    if (_videoPlayerController == null) {
      // SmartDialog.showToast('视频播放器为空，请重新进入本页面');
      return false;
    }
    if (dataSource.videoSource.isNullOrEmpty) {
      SmartDialog.showToast('视频源为空，请重新进入本页面');
      return false;
    }
    String? audioUri;
    if (!isLive) {
      if (dataSource.audioSource.isNullOrEmpty) {
        SmartDialog.showToast('音频源为空');
      } else {
        audioUri = Platform.isWindows
            ? dataSource.audioSource!.replaceAll(';', '\\;')
            : dataSource.audioSource!.replaceAll(':', '\\:');
        await (_videoPlayerController!.platform!).maybeAsNativePlayer
            .setProperty('audio-files', audioUri);
      }
    } else {
      // 与 _createVideoController 保持一致：直播等无外部音频源的场景下，
      // 刷新播放源前必须清空残留的 audio-files，防止旧视频音频继续播放。
      await (_videoPlayerController!.platform!).maybeAsNativePlayer.setProperty(
        'audio-files',
        '',
      );
    }
    await _videoPlayerController!.open(
      Media(
        dataSource.videoSource,
        start: isLive ? null : Duration(milliseconds: positionInMilliseconds),
        extras: audioUri == null ? null : {'audio-files': '"$audioUri"'},
      ),
      play: true,
    );
    return true;
    // seekTo(currentPos);
  }

  // 开始播放
  Future<void> _initializePlayer() async {
    if (_instance == null) return;
    // 设置倍速
    if (_videoPlayerController != null) {
      final speed = isLive ? 1.0 : playbackSpeed;
      if (_videoPlayerController!.state.rate != speed) {
        await setPlaybackSpeed(speed);
      }
    }
    _initVideoFit();

    // 自动播放
    if (_autoPlay) {
      playIfExists();
    }
  }

  List<StreamSubscription>? _subscriptions;
  final Set<ValueChanged<Duration>> _positionListeners = {};
  final Set<ValueChanged<PlayerStatus>> _statusListeners = {};

  Timer? _wakeLockTimer;

  void _startWakeLockTimer() {
    _wakeLockTimer?.cancel();
    _wakeLockTimer = Timer(
      const Duration(milliseconds: 500),
      _stopWakeLock,
    );
  }

  void _stopWakeLockTimer() {
    _wakeLockTimer?.cancel();
    _wakeLockTimer = null;
  }

  void _stopWakeLock() {
    WakelockPlus.disable();
    _updatePlaybackState(debugLabel: 'onVideoPaused');
  }

  void _updatePlaybackState({Duration? position, String? debugLabel}) {
    videoPlayerServiceHandler?.onUpdateState(
      playerStatus,
      isBuffering.value,
      isLive,
      position: position ?? _videoPlayerController!.state.position,
      speed: playbackSpeed,
      debugLabel: debugLabel,
    );
    _updateIOSPip(position);
  }

  /// 播放事件监听
  void _startListeners(NativePlayer player) {
    assert(_subscriptions == null);
    if (OS.isHarmony) {
      _startStallWatchdog();
      Floating().onPipAction = _onPipAction;
      // isPipMode 是普通 bool，build 读它不产生依赖；PiP 结束时若窗口尺寸
      // 恰好没变（如画中画期间从应用栏以小窗打开 app），没有任何重建时机，
      // 页面会冻结在画中画布局。用回调驱动 pipModeRx，页面据此重建。
      // 这里同时是鸿蒙版的 onPipChanged 状态同步（Android 走 Utils.channel，
      // 见构造函数）：系统画中画开始/结束必须同步 isNativePip，否则应用内
      // 小窗不会切到全屏视频分支，画中画窗口里小窗会和页面 UI 叠加。
      // 仅在状态真实变化时驱动自动纯音频状态机，避免监听重建时重复触发。
      void syncNativePip(bool v) {
        final changed = isNativePip.value != v;
        pipModeRx.value = v;
        isNativePip.value = v;
        PipOverlayService.isNativePip = v;
        LivePipOverlayService.isNativePip = v;
        if (changed) {
          handleAutoAudioOnlyPipChanged(v);
        }
      }

      Floating().onPipModeChanged = (v) {
        syncNativePip(v);
        // 系统画中画是另一块 XComponent，平台视图不会跟过去（小窗里一片
        // 空白），要退回 SDR 纹理路径，回到应用时再切回来。渲染路径在
        // VideoController 创建时固定，只能重建播放器。
        // 应用内小窗不在这里：它不改变输出，也不必重建，见 [_ohosOutput]。
        unawaited(_playerQueue.requestRebuild());
      };
      syncNativePip(Floating().isPipMode);
    }
    final stream = player.stream;
    _subscriptions = [
      // 播放中改了「匹配视频亮度」或峰值亮度也要生效：与当前播放器的输出
      // 不一致时重建播放器，进度和播放状态都保留。
      if (OS.isHarmony)
        for (final key in const [
          SettingBoxKey.hdrMatchVideoPeak,
          SettingBoxKey.hdrPeakNits,
        ])
          GStorage.setting
              .watch(key: key)
              .listen((_) => unawaited(_playerQueue.requestRebuild())),

      /// playing
      stream.playing.listen((bool playing) {
        if (playing) {
          playerStatus = .playing;
          _stopWakeLockTimer();
          _updatePlaybackState();
          WakelockPlus.enable();

          if (_isAutoEnterPip &&
              _autoAudioState != AutoAudioOnlyState.autoAudioOnly) {
            if (_isCurrVideoPage || _isInInAppPip) {
              enterPip(autoEnter: true);
            } else {
              _disableAutoEnterPip();
            }
          }
        } else {
          playerStatus = .paused;
          _startWakeLockTimer();
          // 鸿蒙：退后台引发的暂停不取消自动画中画，否则该取消操作会与系统
          // 自动小窗的启动赛跑，导致自动小窗时灵时不灵（Android 的 PiP
          // auto-enter 与离开手势原子执行，无此问题）。仅在应用处于前台
          // （用户主动暂停）时才取消。
          if (!OS.isHarmony ||
              WidgetsBinding.instance.lifecycleState ==
                  AppLifecycleState.resumed) {
            _disableAutoEnterPip();
          }
          _updateIOSPip();
          // 鸿蒙：进入画中画的退后台过程中，系统会直接把 mpv 置为暂停
          //（应用层无任何 pause 调用，插桩证实；auto-start 武装与否均如此）。
          // 画中画窗口可见即应继续播放，且普通 play() 即可恢复（等效于
          // 用户按面板播放键），这里自动补上。_pauseRequestedByApp 排除
          // 用户/业务暂停，频率闸防止与系统拉锯。
          if (OS.isHarmony &&
              !_pauseRequestedByApp &&
              isPipMode &&
              !(videoPlayerController?.state.completed ?? false) &&
              _pipAutoResumeAllowed()) {
            play();
          }
        }
        if (OS.isHarmony) {
          // 同步鸿蒙小窗控制面板的播放/暂停图标
          Floating().updatePipControlStatus(playing: playing);
        }

        for (final element in _statusListeners) {
          element(playing ? .playing : .paused);
        }

        final seconds = videoPlayerController!.state.position.inSeconds;
        if (seconds != 0) {
          makeHeartBeat(seconds, type: .status);
        }
      }),

      ///completed
      stream.completed.listen((bool completed) {
        if (completed) {
          playerStatus = .completed;
          _startWakeLockTimer();

          for (final element in _statusListeners) {
            element(.completed);
          }

          makeHeartBeat(-1, type: .completed);
        }
      }),

      /// position
      stream.position.listen((Duration position) {
        final posInSeconds = position.inSeconds;

        if (posInSeconds != this.position.value) {
          if (posInSeconds == 0 && playerStatus.isPlaying) {
            _updatePlaybackState(position: position);
          }

          this.position.value = posInSeconds;

          makeHeartBeat(posInSeconds);
        }

        for (final element in _positionListeners) {
          element(position);
        }
      }),
      stream.duration.listen((Duration duration) {
        updateDuration(duration);
        _updateIOSPip();
      }),
      stream.buffer.listen((Duration buffer) {
        buffered.value = buffer.inSeconds;
      }),
      stream.buffering.listen((bool buffering) {
        isBuffering.value = buffering;
        if (!playerStatus.isCompleted) {
          _stopWakeLockTimer();
          _updatePlaybackState();
        }
      }),
      if (kDebugMode)
        stream.log.listen(((PlayerLog log) {
          if (log.level == 'error' || log.level == 'fatal') {
            Utils.reportError('${log.level}: ${log.prefix}: ${log.text}', null);
          } else {
            debugPrint(log.toString());
          }
        })),
      stream.error.listen((String event) {
        if (dataSource is FileSource &&
            event.startsWith("Failed to open file")) {
          return;
        }
        if (isLive) {
          if (event.startsWith('tcp: ffurl_read returned ') ||
              event.startsWith("Failed to open https://") ||
              event.startsWith("Can not open external file https://")) {
            Timer(const Duration(milliseconds: 3000), refreshPlayer);
          }
          return;
        }
        if (event.startsWith("Failed to open https://") ||
            event.startsWith("Can not open external file https://") ||
            //tcp: ffurl_read returned 0xdfb9b0bb
            //tcp: ffurl_read returned 0xffffff99
            event.startsWith('tcp: ffurl_read returned ')) {
          EasyThrottle.throttle(
            'controllerStream.error.listen',
            const Duration(milliseconds: 10000),
            () => Timer(const Duration(milliseconds: 3000), () {
              // if (kDebugMode) {
              //   debugPrint("isBuffering.value: ${isBuffering.value}");
              // }
              // if (kDebugMode) {
              //   debugPrint("_buffered.value: ${_buffered.value}");
              // }
              if (isBuffering.value && buffered.value == 0) {
                final customHost = VideoUtils.customCDNUrl;
                SmartDialog.showToast(
                  customHost == null
                      ? '视频链接打开失败，重试中'
                      : '视频链接打开失败，重试中\n当前自定义CDN节点：$customHost，持续失败可尝试更换或清除',
                  displayTime: customHost == null
                      ? const Duration(milliseconds: 500)
                      : const Duration(milliseconds: 3000),
                );
                refreshPlayer();
              }
            }),
          );
        } else if (event.contains('Invalid NAL unit size') ||
            event.contains('Error splitting the input into NAL') ||
            event.contains('Stream ends prematurely')) {
          EasyThrottle.throttle(
            'controllerStream.nal.error',
            const Duration(milliseconds: 5000),
            refreshPlayer,
          );
          Utils.reportError(event);
        } else if (event.startsWith('Could not open codec')) {
          SmartDialog.showToast('无法加载解码器, $event，可能会切换至软解');
        } else if (!onlyPlayAudio.value) {
          if (event.startsWith("error running") ||
              event.startsWith("Failed to open .") ||
              event.startsWith("Cannot open") ||
              event.startsWith("Can not open")) {
            return;
          }
          Utils.reportError(event);
          // SmartDialog.showToast('视频加载错误, $event');
        }
      }),
    ];
  }

  /// 移除事件监听
  void _removeListeners() {
    _stallWatchdog?.cancel();
    _stallWatchdog = null;
    if (Floating().onPipAction == _onPipAction) {
      Floating().onPipAction = null;
    }
    _subscriptions?.forEach((e) => e.cancel());
    _subscriptions?.clear();
    _subscriptions = null;
  }

  /// 鸿蒙小窗控制面板按钮回调（floating 插件转发 controlPanelActionEvent）
  void _onPipAction(String event, int? status) {
    switch (event) {
      case 'playbackStateChanged':
        // status: 1=请求播放，0=请求暂停；缺省时按当前状态取反
        final wantPlay = status == null ? !playerStatus.isPlaying : status == 1;
        if (wantPlay) {
          play();
        } else {
          pause();
        }
      case 'fastForward':
        onForward(fastForBackwardDuration);
      case 'fastBackward':
        onBackward(fastForBackwardDuration);
    }
  }

  /// 鸿蒙音频打断兜底检测（见 _stallWatchdog 字段注释）
  void _startStallWatchdog() {
    _stallWatchdog?.cancel();
    _stallLastPosition = null;
    _stallTicks = 0;
    _stallWatchdog = Timer.periodic(const Duration(milliseconds: 1200), (_) {
      if (!playerStatus.isPlaying ||
          isBuffering.value ||
          positionInMilliseconds == 0) {
        _stallTicks = 0;
        _stallLastPosition = null;
        return;
      }
      if (positionInMilliseconds == _stallLastPosition) {
        _stallTicks++;
        // 连续约 2.4s 声称播放中却毫无进展：音频输出已被系统暂停。
        // 同步为暂停态，让按钮显示"播放"，恢复逻辑见 play()。
        if (_stallTicks >= 2) {
          _stallTicks = 0;
          _stallLastPosition = null;
          _audioInterrupted = true;
          pause(isInterrupt: true);
        }
      } else {
        _stallTicks = 0;
        _stallLastPosition = positionInMilliseconds;
      }
    });
  }

  void _cancelSubForSeek() {
    if (_subForSeek != null) {
      _subForSeek!.cancel();
      _subForSeek = null;
    }
  }

  Future<void> seek(Duration position, {bool isSeek = false}) async {
    if (isSeek) {
      /// 拖动进度条调节时，不等待第一帧，防止抖动
      try {
        await _videoPlayerController?.stream.buffer.first;
      } catch (_) {
        // 播放器在等待期间被销毁/换源：流已关闭（Bad state: No element）
      }
      if (_playerCount == 0) return;
    }
    danmakuController?.clear();
    try {
      await _videoPlayerController?.seek(position);
      _updateIOSPip(position);
    } catch (e) {
      if (kDebugMode) debugPrint('seek failed: $e');
    }
  }

  /// 跳转至指定位置
  Future<void> seekTo(Duration position, {bool isSeek = true}) async {
    if (_playerCount == 0) {
      return;
    }
    if (position < Duration.zero) {
      position = Duration.zero;
    }
    _heartDuration = position.inSeconds;

    if (duration.value != 0) {
      seek(position, isSeek: isSeek);
    } else {
      // if (kDebugMode) debugPrint('seek duration else');
      _subForSeek?.cancel();
      _subForSeek = duration.listen((_) {
        seek(position, isSeek: isSeek);
        _cancelSubForSeek();
      });
    }
  }

  /// 设置倍速
  Future<void> setPlaybackSpeed(double speed) async {
    lastPlaybackSpeed = playbackSpeed;

    if (speed == _videoPlayerController?.state.rate) return;

    await _setRate(speed);
    if (!isLive) _playbackSpeed.value = speed;
    _updatePlaybackState();
    if (danmakuController != null) {
      try {
        danmakuController?.updateOption(
          danmakuController!.option.copyWith(
            duration: DanmakuOptions.danmakuDuration / speed,
            staticDuration: DanmakuOptions.danmakuStaticDuration / speed,
          ),
        );
      } catch (_) {}
    }
  }

  // 还原默认速度
  double playSpeedDefault = Pref.playSpeedDefault;
  Future<void> setDefaultSpeed() async {
    await _setRate(playSpeedDefault);
    _playbackSpeed.value = playSpeedDefault;
  }

  bool _showControlsOnNextPlay = false;

  void markManualEpisodeChange() {
    if (Pref.showControlsOnManualEpisodeChange) {
      _showControlsOnNextPlay = true;
    }
  }

  bool _consumeShowControlsOnNextPlay() {
    final showControls = _showControlsOnNextPlay;
    _showControlsOnNextPlay = false;
    return showControls;
  }

  /// 底层播放：申请音频焦点 + 启动播放，不含 UI 副作用
  Future<void> _rawPlay() async {
    await _videoPlayerController?.play();
    audioSessionHandler?.setActive(true);
    playerStatus = PlayerStatus.playing;
  }

  Future<void> _setRate(double speed) async {
    if (playerDisposed) return;
    try {
      await _videoPlayerController?.setRate(speed);
    } catch (e) {
      if (kDebugMode) debugPrint('setRate failed: $e');
    }
  }

  /// 播放视频
  Future<void> play({bool repeat = false, bool hideControls = true}) async {
    if (_playerCount == 0) return;
    _pauseRequestedByApp = false;
    // 播放时自动隐藏控制条
    final showControlsOnNextPlay = _consumeShowControlsOnNextPlay();
    controls = !hideControls || showControlsOnNextPlay;
    // repeat为true，将从头播放
    if (repeat) {
      await seekTo(Duration.zero, isSeek: false);
    }

    // 鸿蒙：被系统打断后 mpv 的音频渲染器已被暂停且 mpv 自身无感知，
    // 单纯解除暂停不会恢复。先重新激活音频会话抢回焦点（暂停其他 app 的
    // 音频），再用 ao-reload 重建音频输出，让新渲染器以新焦点启动。
    if (_audioInterrupted) {
      _audioInterrupted = false;
      try {
        await audioSessionHandler?.setActive(true);
      } catch (_) {}
      try {
        await _videoPlayerController?.platform?.maybeAsNativePlayer.command(
          const ['ao-reload'],
        );
      } catch (_) {}
    }

    await _rawPlay();
  }

  /// 暂停播放
  /// 应用层是否主动发起了当前这次暂停。用于区分"系统在进入画中画的退后台
  /// 过程中直接把 mpv 置为暂停"（Dart 层无任何 pause 调用，需要自动恢复）
  /// 与用户/业务发起的正常暂停。
  bool _pauseRequestedByApp = false;
  int _pipAutoResumeCount = 0;
  DateTime? _pipAutoResumeWindowStart;

  /// 生命周期回调的补偿入口：处于画中画且播放被系统静默暂停（而非应用
  /// 主动暂停）时自动续播。覆盖"系统暂停发生在 isPipMode 置位之前"的
  /// 时序——彼时 stream.playing 监听里的自动续播不满足条件，只能靠随后
  /// 到达的生命周期事件（floating 插件会在 PiP 启动后补发 inactive）触发。
  void autoResumeInPipIfNeeded() {
    if (OS.isHarmony &&
        !_pauseRequestedByApp &&
        isPipMode &&
        !playerStatus.isPlaying &&
        !(videoPlayerController?.state.completed ?? false) &&
        _pipAutoResumeAllowed()) {
      play();
    }
  }

  /// 限制画中画内自动续播的频率，防止与系统的强制暂停陷入拉锯。
  bool _pipAutoResumeAllowed() {
    final now = DateTime.now();
    if (_pipAutoResumeWindowStart == null ||
        now.difference(_pipAutoResumeWindowStart!) >
            const Duration(seconds: 10)) {
      _pipAutoResumeWindowStart = now;
      _pipAutoResumeCount = 0;
    }
    return ++_pipAutoResumeCount <= 3;
  }

  Future<void> pause({bool notify = true, bool isInterrupt = false}) async {
    _pauseRequestedByApp = true;
    // 播放器可能在异步链路中被销毁（media_kit 会抛 [Player] has been disposed）
    try {
      await _videoPlayerController?.pause();
    } catch (e) {
      if (kDebugMode) debugPrint('pause failed: $e');
    }
    playerStatus = .paused;

    // 主动暂停时让出音频焦点
    if (!isInterrupt) {
      audioSessionHandler?.setActive(false);
    }
  }

  bool tripling = false;

  /// 隐藏控制条
  void hideTaskControls() {
    _timer?.cancel();
    _timer = Timer(showControlDuration, () {
      if (!isSeeking.value && !tripling) {
        controls = false;
      }
      _timer = null;
    });
  }

  void onSeekStart(int seekFrom) {
    seekPosition.value = seekFrom;
    isSeeking.value = true;
  }

  void onSeekEnd() {
    if (showSeekPreview) {
      showPreview.value = false;
    }
    hasToasted = false;
    isSeeking.value = false;
    if (PlatformUtils.isDesktop) {
      requireDesktopProgressHoverReenter = _desktopProgressPointerInside;
      hideDesktopProgressPreview();
    }
    hideTaskControls();
  }

  final RxBool volumeIndicator = false.obs;
  Timer? volumeTimer;
  bool volumeInterceptEventStream = false;

  double get maxVolume => PlatformUtils.isDesktop
      ? Pref.maxVolume
      : (Pref.enableAppVolume && Pref.enableVolumeBoost ? 2.0 : 1.0);

  // 音量增强二次确认：是否已解锁突破 100%（松手后重置）
  bool volumeBoostUnlocked = false;

  // 手势滑动时的音量上限：未解锁时最大 1.0，解锁后最大 2.0
  double get gestureVolumeMax {
    if (Pref.enableAppVolume && Pref.enableVolumeBoost) {
      return volumeBoostUnlocked ? 2.0 : 1.0;
    }
    return maxVolume;
  }

  // 松手时重置解锁状态
  void onVolumeGestureEnd() {
    if (Pref.enableAppVolume && Pref.enableVolumeBoost) {
      volumeBoostUnlocked = false;
    }
  }

  Future<void> setVolume(double volume, {bool showIndicator = true}) async {
    if (this.volume.value != volume) {
      this.volume.value = volume;
      try {
        if (PlatformUtils.isDesktop) {
          await _videoPlayerController!.setVolume(volume * 100);
          actualVolume.value = volume;
        } else {
          // 移动平台：根据设置选择音量控制方式
          if (Pref.enableAppVolume) {
            // 应用内音量模式：使用 media_kit 控制应用内音量
            _videoPlayerController?.setVolume(volume * 100);
            actualVolume.value = volume;
          } else {
            // 默认模式：控制系统音量
            FlutterVolumeController.updateShowSystemUI(false);
            await FlutterVolumeController.setVolume(volume);
            actualVolume.value =
                await FlutterVolumeController.getVolume() ?? actualVolume.value;
          }
        }
      } catch (err) {
        if (kDebugMode) debugPrint(err.toString());
      }
    }
    if (showIndicator) {
      volumeIndicator.value = true;
    }
    volumeInterceptEventStream = true;
    volumeTimer?.cancel();
    volumeTimer = Timer(const Duration(milliseconds: 200), () {
      volumeIndicator.value = false;
      volumeInterceptEventStream = false;
      if (PlatformUtils.isDesktop) {
        setting.put(SettingBoxKey.desktopVolume, volume.toPrecision(3));
      } else if (Pref.enableAppVolume) {
        // 移动平台应用内音量模式：保存音量到持久化存储
        // duck 期间不保存，避免保存临时的减半音量
        if (!_isDucked) {
          Pref.appVolume = volume;
        }
      }
    });
  }

  /// 处理应用内音量设置变更
  Future<void> onAppVolumeSettingChanged() async {
    if (!PlatformUtils.isMobile) return;

    // 如果播放器未初始化，直接返回（设置已保存，下次播放器初始化时生效）
    if (_videoPlayerController == null) return;

    if (Pref.enableAppVolume) {
      // 切换到应用内音量模式
      // 获取当前系统音量
      final currentSystemVolume =
          (await FlutterVolumeController.getVolume()) ?? 1.0;
      systemVolume.value = currentSystemVolume;

      final appVolume = (Pref.playerVolume / 100)
          .clamp(0.0, maxVolume)
          .toDouble();
      volume.value = appVolume;
      Pref.appVolume = appVolume;
      volumeBoostUnlocked = false;

      // 关闭上游固定增益，避免和应用内音量叠加
      _videoPlayerController?.setVolume(appVolume * 100);

      // 显示提示
      SmartDialog.showToast('已切换到应用内音量模式');
    } else {
      // 切换到同步系统音量模式
      // 恢复上游播放器音量设置，并按增益折算系统音量
      final currentSystemVolume =
          (await FlutterVolumeController.getVolume()) ?? 1.0;
      final playerGain = max(Pref.playerVolume / 100, 0.01);
      final newSystemVolume = (currentSystemVolume * volume.value / playerGain)
          .clamp(0.0, 1.0)
          .toDouble();

      await FlutterVolumeController.updateShowSystemUI(false);
      await FlutterVolumeController.setVolume(newSystemVolume);
      await _videoPlayerController?.setVolume(Pref.playerVolume);
      volumeBoostUnlocked = false;

      // 更新状态
      systemVolume.value = newSystemVolume;
      volume.value = newSystemVolume;

      SmartDialog.showToast('已切换到同步系统音量模式');
    }
  }

  /// duck 事件处理（由 audio_session.dart 调用）
  Future<void> handleDuck(bool begin) async {
    if (!PlatformUtils.isMobile) return;

    if (Pref.enableAppVolume) {
      // 应用内音量模式：使用临时音量，不影响持久化值
      if (begin) {
        _isDucked = true;
        _preDuckVolume = volume.value;
        volume.value = volume.value * 0.5;
        _videoPlayerController?.setVolume(volume.value * 100);
      } else {
        _isDucked = false;
        volume.value = _preDuckVolume;
        _videoPlayerController?.setVolume(volume.value * 100);
      }
    }
    // 同步模式：使用原有逻辑，直接调用 setVolume 即可
  }

  /// Toggle Change the videofit accordingly
  void toggleVideoFit(VideoFitType value) {
    _prefFit = videoFit.value = value;
    video.put(VideoBoxKey.cacheVideoFit, value.index);
  }

  /// 读取fit
  var _prefFit = VideoFitType.values[Pref.cacheVideoFit];
  void _initVideoFit() {
    if (_prefFit == .fill && _isVertical) {
      videoFit.value = .contain;
    } else {
      videoFit.value = _prefFit;
    }
  }

  /// 设置后台播放
  void setBackgroundPlay(bool val) {
    videoPlayerServiceHandler?.enableBackgroundPlay = val;
    if (!tempPlayerConf) {
      setting.put(SettingBoxKey.enableBackgroundPlay, val);
    }
  }

  set controls(bool visible) {
    if (!visible && PlatformUtils.isDesktop) {
      hideDesktopProgressPreview();
    }
    showControls.value = visible;
    _timer?.cancel();
    if (visible) {
      hideTaskControls();
    }
  }

  Timer? longPressTimer;

  /// 长按倍速的上下界，避免自定义系数把速率放到 mpv 无法处理的区间
  static const double _minLongPressSpeed = 0.25;
  static const double _maxLongPressSpeed = 10.0;

  /// 进入长按前的播放速度，松手后恢复到它
  double? _longPressRestoreSpeed;

  /// 「x 倍速中」提示所显示的速度。只在进入长按时写入，松手后保持不变，
  /// 这样提示条 150ms 淡出期间不会闪回已恢复的原速。
  late final RxDouble longPressDisplaySpeed = longPressSpeed.obs;

  void cancelLongPressTimer() {
    longPressTimer?.cancel();
    longPressTimer = null;
  }

  /// 设置长按倍速状态 live模式下禁用
  Future<void> setLongPressStatus(bool val, {bool isCancel = false}) async {
    if (isLive) {
      return;
    }
    if (controlsLock.value) {
      return;
    }
    if (longPressStatus.value == val) {
      return;
    }
    if (val) {
      if (playerStatus.isPlaying) {
        _speedLockArmed = false;
        _cancelSpeedLockHintTimer();
        longPressStatus.value = val;
        HapticFeedback.lightImpact();
        if (speedLocked) {
          // 锁定态：速度已固化为锁定值，不得再调速——setPlaybackSpeed 会
          // 覆盖 lastPlaybackSpeed（锁定前速度），动态倍速下还会二次翻倍
          speedLockHint.value = SpeedLockHint.swipeDownToUnlock;
        } else {
          _longPressRestoreSpeed = playbackSpeed;
          final targetSpeed = enableAutoLongPressSpeed
              ? (playbackSpeed * longPressSpeedFactor).clamp(
                  _minLongPressSpeed,
                  _maxLongPressSpeed,
                )
              : longPressSpeed;
          longPressDisplaySpeed.value = targetSpeed;
          await setPlaybackSpeed(targetSpeed);
          if (longPressStatus.value) {
            speedLockHint.value = SpeedLockHint.swipeUpToLock;
          }
        }
      }
    } else {
      // 预备态在松手时提交；系统打断（cancel）一律不提交
      final bool commit = _speedLockArmed && !isCancel;
      _speedLockArmed = false;
      longPressStatus.value = val;
      if (speedLocked) {
        if (commit) {
          speedLocked = false;
          HapticFeedback.mediumImpact();
          await setPlaybackSpeed(lastPlaybackSpeed);
          _showTransientSpeedLockHint(SpeedLockHint.unlockedConfirm);
        } else {
          speedLockHint.value = SpeedLockHint.none;
        }
      } else {
        if (commit) {
          speedLocked = true;
          HapticFeedback.mediumImpact();
          _showTransientSpeedLockHint(SpeedLockHint.lockedConfirm);
        } else {
          speedLockHint.value = SpeedLockHint.none;
          await setPlaybackSpeed(_longPressRestoreSpeed ?? lastPlaybackSpeed);
        }
      }
      _longPressRestoreSpeed = null;
    }
  }

  // 长按倍速锁定：预备阈值 = 播放器实高 × 比例，clamp 到人体工学区间。
  // 比例由实测锚点反推（横屏半屏 48/231≈0.21、横屏全屏 96/411≈0.23），
  // 下限保小窗/小播放器手感，上限≈2.7cm 为拇指第一指节舒适推程封顶
  static const double _speedLockThresholdRatio = 0.22;
  static const double _speedLockThresholdMin = 48;
  static const double _speedLockThresholdMax = 112;
  static const double _speedLockHysteresis = 10;

  /// 长按中手指移动，dy 为相对长按起点的纵向偏移（上滑为负），
  /// [playerHeight] 为播放器组件实高（决定阈值）。
  /// 越线进入预备态、滑回取消预备，均可反复，提交只发生在松手
  void onLongPressMove(double dy, double playerHeight) {
    if (!longPressStatus.value) {
      return;
    }
    final double threshold = clampDouble(
      playerHeight * _speedLockThresholdRatio,
      _speedLockThresholdMin,
      _speedLockThresholdMax,
    );
    if (speedLocked) {
      // 下滑预备退出
      if (!_speedLockArmed && dy >= threshold) {
        _speedLockArmed = true;
        HapticFeedback.lightImpact();
        speedLockHint.value = SpeedLockHint.releaseToUnlock;
      } else if (_speedLockArmed && dy <= threshold - _speedLockHysteresis) {
        _speedLockArmed = false;
        HapticFeedback.lightImpact();
        speedLockHint.value = SpeedLockHint.swipeDownToUnlock;
      }
    } else {
      // 上滑预备锁定
      if (!_speedLockArmed && dy <= -threshold) {
        _speedLockArmed = true;
        HapticFeedback.lightImpact();
        speedLockHint.value = SpeedLockHint.releaseToLock;
      } else if (_speedLockArmed && dy >= -(threshold - _speedLockHysteresis)) {
        _speedLockArmed = false;
        HapticFeedback.lightImpact();
        speedLockHint.value = SpeedLockHint.swipeUpToLock;
      }
    }
  }

  /// 手动调速（倍速菜单/快捷键）：锁定态下先解除锁定再应用所选速度
  Future<void> setManualPlaybackSpeed(double speed) async {
    if (speedLocked) {
      await releaseSpeedLock(restoreSpeed: false);
    }
    await setPlaybackSpeed(speed);
  }

  /// 解除倍速锁定；[restoreSpeed] 为 true 时恢复锁定前速度
  Future<void> releaseSpeedLock({bool restoreSpeed = true}) async {
    if (!speedLocked) {
      return;
    }
    speedLocked = false;
    _speedLockArmed = false;
    _cancelSpeedLockHintTimer();
    speedLockHint.value = SpeedLockHint.none;
    if (restoreSpeed) {
      await setPlaybackSpeed(lastPlaybackSpeed);
    }
  }

  void _showTransientSpeedLockHint(SpeedLockHint hint) {
    speedLockHint.value = hint;
    _speedLockHintTimer?.cancel();
    _speedLockHintTimer = Timer(const Duration(seconds: 2), () {
      if (speedLockHint.value == hint) {
        speedLockHint.value = SpeedLockHint.none;
      }
    });
  }

  void _cancelSpeedLockHintTimer() {
    _speedLockHintTimer?.cancel();
    _speedLockHintTimer = null;
  }

  bool get isCompleted =>
      videoPlayerController!.state.completed ||
      durationInMilliseconds - positionInMilliseconds <= 50;

  // 双击播放、暂停
  Future<void> onDoubleTapCenter() async {
    if (!isLive && isCompleted) {
      await videoPlayerController!.seek(Duration.zero);
      await _rawPlay();
    } else if (videoPlayerController!.state.playing) {
      await pause();
    } else {
      await _rawPlay();
    }
  }

  final RxBool mountSeekBackwardButton = false.obs;
  final RxBool mountSeekForwardButton = false.obs;

  void onDoubleTapSeekBackward() {
    mountSeekBackwardButton.value = true;
  }

  void onDoubleTapSeekForward() {
    mountSeekForwardButton.value = true;
  }

  void onForward(Duration duration) {
    onForwardBackward(videoPlayerController!.state.position + duration);
  }

  void onBackward(Duration duration) {
    onForwardBackward(videoPlayerController!.state.position - duration);
  }

  void onForwardBackward(Duration duration) {
    seekTo(
      duration.clamp(Duration.zero, videoPlayerController!.state.duration),
      isSeek: false,
    ).whenComplete(play);
  }

  void doubleTapFuc(DoubleTapType type) {
    if (!enableQuickDouble) {
      onDoubleTapCenter();
      return;
    }
    switch (type) {
      case DoubleTapType.left:
        // 双击左边区域 👈
        onDoubleTapSeekBackward();
        break;
      case DoubleTapType.center:
        onDoubleTapCenter();
        break;
      case DoubleTapType.right:
        // 双击右边区域 👈
        onDoubleTapSeekForward();
        break;
    }
  }

  /// 关闭控制栏
  void onLockControl(bool val) {
    feedBack();
    controlsLock.value = val;
    if (val) {
      hideDesktopProgressPreview();
    }
    if (!val && showControls.value) {
      showControls.refresh();
    }
    controls = !val;
  }

  void _setFullScreen(bool val) {
    isFullScreen.value = val;
    updateSubtitleStyle();
    onFullScreenChanged?.call(val);
    if (!OS.isHarmony) return;

    if (!val) {
      HarmonyChannel.onLandscapeOrMiniWindowChange(false, null); // 非横屏
    } else if (!isVertical) {
      HarmonyChannel.onLandscapeOrMiniWindowChange(true, null); // 横屏
    }
  }

  double screenRatio = 0.0;
  bool isManualFS = true;

  /// 最近一次退出全屏的时间。鸿蒙部分机型开启旋转锁定后，会被 childWhenDisabled 的窗口变
  /// 横屏自动进全屏立即拉回，表现为退不出全屏。用该时间戳抑制退出后短暂窗口内的自动进全屏。
  ///
  /// 手动与自动退出一视同仁：窗口从横屏转回竖屏总有延迟，这段时间里窗口宽高比
  /// 仍是横屏，不抑制就会被立刻拉回全屏。传感器驱动的进全屏（landscape 分支）
  /// 不看这个时间戳，所以退出后立刻转回横屏依然能正常进全屏。
  DateTime _lastFsExitAt = DateTime.fromMillisecondsSinceEpoch(0);
  bool get suppressAutoFullScreen =>
      DateTime.now().difference(_lastFsExitAt) <
      const Duration(milliseconds: 600);
  // 每次读取而不缓存：播放器是跨页面存活的单例，缓存会让在设置页改完
  // 「默认全屏方向」后本次会话仍用旧值，表现为「改了没反应」
  FullScreenMode get mode => Pref.fullScreenMode;
  late final horizontalScreen = Pref.horizontalScreen;
  late final removeSafeArea = Pref.removeSafeArea;

  Future<void>? changeOrientation({
    required bool isVertical,
    DeviceOrientation? orientation,
  }) {
    if (orientation == null && (mode == .none || mode == .gravity)) {
      return null;
    }
    if (orientation == null &&
        (mode == .vertical ||
            (mode == .auto && isVertical) ||
            (mode == .ratio && (isVertical || screenRatio < kScreenRatio)))) {
      return portraitUpMode();
    } else {
      // https://github.com/flutter/flutter/issues/73651
      // https://github.com/flutter/flutter/issues/183708
      if (Platform.isAndroid || OS.isHarmony) {
        if ((orientation ?? _orientation) == .landscapeRight) {
          return landscapeRightMode();
        } else {
          return landscapeLeftMode();
        }
      } else {
        if (orientation == .landscapeLeft) {
          return landscapeLeftMode();
        } else {
          return landscapeRightMode();
        }
      }
    }
  }

  // 全屏
  bool _fsProcessing = false;
  Future<void> triggerFullScreen({
    bool status = true,
    bool inAppFullScreen = false,
    DeviceOrientation? orientation,
    bool isManualFS = true,
  }) async {
    if (isDesktopPip) return;
    if (isFullScreen.value == status) return;

    if (_fsProcessing) return;
    _fsProcessing = true;
    // 任何一次真正的全屏切换都作废挂起的自动退出确认，避免刚切完又被延迟退出。
    // 由 _scheduleAutoExitFullScreen 的回调调用时定时器已置空，这里是空操作。
    _cancelAutoExitFs();
    this.isManualFS = isManualFS;
    try {
      if (status) {
        if (PlatformUtils.isMobile) {
          hideSystemBar();
          await changeOrientation(
            isVertical: isVertical,
            orientation: orientation,
          );
          if (OS.isHarmony && !HarmonyChannel.isWindowMode) {
            // 手动进全屏的目标方向：横屏时等视口真正变横屏再置全屏布局，
            // 避免全屏先在竖屏/中间尺寸渲染，导致比例错误和动画跳变。
            final targetLandscape =
                orientation != null ||
                switch (mode) {
                  .vertical => false,
                  .horizontal => true,
                  .auto => !isVertical,
                  .ratio => !isVertical && screenRatio >= kScreenRatio,
                  _ => false,
                };
            if (targetLandscape) {
              await _waitForLandscapeViewport();
            }
          }
        } else {
          enterDesktopFullScreen(inAppFullScreen: inAppFullScreen);
        }
      } else {
        if (PlatformUtils.isMobile) {
          if (!removeSafeArea) {
            showSystemBar();
          }
          if (orientation == null && mode == .none) {
            return;
          }
          _lastFsExitAt = DateTime.now();
          // 鸿蒙mate80开启旋转锁定时，原生setPreferredOrientation可能长时间
          // 不返回。加超时保证退出
          await resetScreenRotation()?.timeout(
            const Duration(milliseconds: 500),
            onTimeout: () {},
          );
          // 退出全屏时，延迟等待方向旋转后改变组件
          if (OS.isHarmony && !HarmonyChannel.isWindowMode) {
            await Future<void>.delayed(const Duration(milliseconds: 32));
          }
        } else {
          exitDesktopFullScreen();
        }
      }
    } finally {
      _setFullScreen(status);
      _fsProcessing = false;
    }
  }

  /// 输出（渲染路径或 HDR 参数，见 [_appliedOutput]）变化时按当前输出重建。
  ///
  /// 与 setDataSource 排在同一条队列里，是否真要重建到了队头才判断，
  /// 见 [PlayerRebuildQueue]。
  Future<void> _syncRenderPath() async {
    if (!OS.isHarmony) return;
    await _playerQueue.requestRebuild();
  }

  /// 应用内小窗进出时由 [PipOverlayService] / [LivePipOverlayService] 调用，
  /// 语义对齐系统画中画的 `onPipModeChanged`（见 [_startListeners]）。
  ///
  /// 应用内小窗**不影响**输出（见 [_ohosOutput]）：它是应用自己 Overlay 里的一
  /// 层，仍在同一个 Flutter 视图内，平台视图照常合成，画面与小窗外的播放走同一
  /// 条 HDR 平台视图路径。所以这里实际上是个空操作——[PlayerRebuildQueue] 到队头
  /// 发现 isStale 为假，直接返回，不会重建播放器，进出小窗也就不再打断播放。
  ///
  /// 留着这个入口是为了让两个小窗服务的语义与系统画中画对齐；真需要重建的只有
  /// 系统画中画（走 `onPipModeChanged` -> `syncNativePip`）。
  void notifyInAppPipChanged() => unawaited(_syncRenderPath());

  /// 按当前渲染路径重建播放器，保留进度、播放状态与倍速。
  /// 只由 [_playerQueue] 调用，失败向上抛，由它记下待重试。
  Future<void> _rebuildForRenderPath() async {
    // 上一次重建失败后，重试可能落在页面已经销毁之后
    if (_playerCount == 0) return;
    final wasPlaying = playerStatus.isPlaying;
    // 不能只信 state.position：open() 会把它清零，重建恰好落在「新播放器
    // 刚建好、mpv 还没上报 time-pos」的窗口里时读到的就是 0，一重建就从头播，
    // 还会把 0 当成观看进度写回历史。position（秒）由监听器维护，可以兜底。
    final live = positionInMilliseconds;
    final pos = live > 0
        ? Duration(milliseconds: live)
        : Duration(seconds: position.value);
    await _createVideoController(dataSource, pos, null, isRebuild: true);
    if (_videoPlayerController == null) return;
    // 重建出来的是一个全新的 Player，倍速回到了默认值 1.0，而 UI 上的
    // _playbackSpeed 没变——不重新应用就会「显示 2.0x，实际 1.0x」。
    // 注意不要直接调 _initializePlayer()：它带 _autoPlay 分支，会把用户
    // 手动暂停的视频重新播起来。
    if (!isLive && _videoPlayerController!.state.rate != _playbackSpeed.value) {
      await setPlaybackSpeed(_playbackSpeed.value);
    }
    if (wasPlaying) {
      await play();
    }
    // 到这里新播放器已经 open 完毕，页面才能安全地重挂字幕轨、
    // SponsorBlock 这些绑在 Player 上的东西。见 [playerRebuilt]。
    playerRebuilt.value++;
  }

  void _onRenderPathRebuildFailed(Object error, StackTrace stackTrace) {
    if (kDebugMode) {
      debugPrint(stackTrace.toString());
      debugPrint('_rebuildForRenderPath failed: $error');
    }
    // 旧播放器多半已经 dispose，画面是黑的；切换清晰度会重建播放器并重试。
    SmartDialog.showToast('切换播放画面失败，切换清晰度可重试');
  }

  /// 等待平台视口旋转为横屏（宽>高），带超时兜底。
  Future<void> _waitForLandscapeViewport({
    Duration timeout = const Duration(milliseconds: 600),
  }) async {
    final views = WidgetsBinding.instance.platformDispatcher.views;
    if (views.isEmpty) return;
    final view = views.first;
    final stopwatch = Stopwatch()..start();
    while (stopwatch.elapsedMilliseconds < timeout.inMilliseconds) {
      final size = view.physicalSize;
      if (size.width > size.height) return;
      await Future<void>.delayed(const Duration(milliseconds: 16));
    }
  }

  void addPositionListener(ValueChanged<Duration> listener) {
    if (_playerCount == 0) return;
    _positionListeners.add(listener);
  }

  void removePositionListener(ValueChanged<Duration> listener) =>
      _positionListeners.remove(listener);

  void addStatusLister(ValueChanged<PlayerStatus> listener) {
    if (_playerCount == 0) return;
    _statusListeners.add(listener);
  }

  void removeStatusLister(ValueChanged<PlayerStatus> listener) =>
      _statusListeners.remove(listener);

  // 记录播放记录
  Future<void>? makeHeartBeat(
    int progress, {
    HeartBeatType type = .playing,
    bool isManual = false,
    dynamic aid,
    dynamic bvid,
    dynamic cid,
    dynamic epid,
    dynamic seasonId,
    dynamic pgcType,
    VideoType? videoType,
  }) {
    if (!isManual && dataStatus.value != DataStatus.loaded) {
      return null;
    }
    if (isLive ||
        !enableHeart ||
        progress == 0 ||
        (playerStatus.isPaused && !isManual)) {
      return null;
    }

    Future<void> send() {
      return VideoHttp.heartBeat(
        aid: aid ?? _aid,
        bvid: bvid ?? _bvid,
        cid: cid ?? this.cid,
        progress: progress,
        epid: epid ?? _epid,
        seasonId: seasonId ?? _seasonId,
        subType: pgcType ?? _pgcType,
        videoType: videoType ?? _videoType,
      );
    }

    switch (type) {
      case .playing:
        if (progress - _heartDuration >= 5) {
          _heartDuration = progress;
          return send();
        }
      case .status:
        if (progress - _heartDuration >= 2) {
          _heartDuration = progress;
          return send();
        }
      case .completed:
        if (playerStatus.isCompleted &&
            (durationInMilliseconds - positionInMilliseconds) <= 1000) {
          progress = -1;
        }
        return send();
    }
    return null;
  }

  void setPlayRepeat(PlayRepeat type) {
    playRepeat = type;
    final handler = videoPlayerServiceHandler;
    handler?.updateRepeatMode(type);
    // 视频页循环模式也支持从播控中心/实况窗切换
    handler?.onRepeatModeChanged = (repeat) async {
      setPlayRepeat(repeat);
    };
    if (!tempPlayerConf) video.put(VideoBoxKey.playRepeat, type.index);
  }

  void putSubtitleSettings() {
    setting.putAllNE({
      SettingBoxKey.subtitleFontScale: subtitleFontScale,
      SettingBoxKey.subtitleFontScaleFS: subtitleFontScaleFS,
      SettingBoxKey.subtitlePaddingH: subtitlePaddingH,
      SettingBoxKey.subtitlePaddingB: subtitlePaddingB,
      SettingBoxKey.subtitleBgOpacity: subtitleBgOpacity,
      SettingBoxKey.subtitleStrokeWidth: subtitleStrokeWidth,
      SettingBoxKey.subtitleFontWeight: subtitleFontWeight,
      SettingBoxKey.subtitleSecondaryFontScale: subtitleSecondaryFontScale,
      SettingBoxKey.subtitleSecondaryFontScaleFS: subtitleSecondaryFontScaleFS,
      SettingBoxKey.subtitleSecondaryBgOpacity: subtitleSecondaryBgOpacity,
      SettingBoxKey.subtitleSecondaryStrokeWidth: subtitleSecondaryStrokeWidth,
      SettingBoxKey.subtitleSecondaryFontWeight: subtitleSecondaryFontWeight,
      SettingBoxKey.subtitleSecondarySpacing: subtitleSecondarySpacing,
    });
  }

  // isCloseAll 由外部页面直接置位（新 ohos 提交的写法），故为公开字段
  bool isCloseAll = false;

  /// 播放器销毁时恢复应用级方向：开着横屏适配跟随系统，否则锁回竖屏
  Future<void>? resetScreenRotation() {
    if (horizontalScreen) {
      return fullMode();
    } else {
      // 鸿蒙：部分机型开启系统旋转锁定时 setPreferredOrientations 无法把窗口
      // 从横屏转回竖屏，用不受锁控制的原生接口强制转回，避免退出全屏卡横屏。
      if (OS.isHarmony) {
        return harmonyForcePortrait();
      }
      return portraitUpMode();
    }
  }

  void onCloseAll() {
    isCloseAll = true;
    if (PlatformUtils.isDesktop) {
      exitDesktopFullScreen();
    } else {
      //规避鸿蒙悬浮窗全屏下，返回主页，方向不更新
      triggerFullScreen(status: false);
    }
    // dispose 已改为异步（退后台清内存），这里不阻塞路由返回
    unawaited(dispose());
    Get.until((route) => route.isFirst);
  }

  // ──────────────────────────────────────────────
  // 后台自动只听音频 — 状态机
  // ──────────────────────────────────────────────

  /// 手动听视频/仅播放音频标记，由三点菜单或直播按钮调用
  void markManualOnlyPlayAudio(bool enabled) {
    _interruptAutoAudioRestore();
    if (enabled) {
      _cancelAutoAudioOnlyTimer();
      _autoAudioState = AutoAudioOnlyState.manualAudioOnly;
    } else {
      if (_autoAudioState == AutoAudioOnlyState.manualAudioOnly ||
          _autoAudioState == AutoAudioOnlyState.autoAudioOnly) {
        _autoAudioState = AutoAudioOnlyState.idle;
      }
    }
  }

  /// 设置项变更时调用
  void onAutoAudioOnlySettingChanged() {
    if (!Pref.autoAudioOnlyInBackground || !continuePlayInBackground.value) {
      if (_autoAudioState == AutoAudioOnlyState.arm ||
          _autoAudioState == AutoAudioOnlyState.pipHold) {
        _cancelAutoAudioOnlyTimer();
        _autoAudioState = AutoAudioOnlyState.idle;
      } else if (_autoAudioState == AutoAudioOnlyState.autoAudioOnly) {
        _cancelAutoAudioOnlyTimer();
        unawaited(restoreFromAutoAudioOnly());
      }
    }
  }

  void _cancelAutoAudioOnlyTimer() {
    _autoAudioOnlyTimer?.cancel();
    _autoAudioOnlyTimer = null;
  }

  /// 自动只听准入检查
  bool _canEnterAutoAudioOnly() {
    if (!PlatformUtils.isMobile) return false;
    if (!Pref.autoAudioOnlyInBackground) return false;
    if (!continuePlayInBackground.value) return false;
    if (_videoPlayerController == null) return false;
    if (!playerStatus.isPlaying) return false;
    if (isPipMode) return false;
    if (_autoAudioState == AutoAudioOnlyState.manualAudioOnly) return false;
    if (_autoAudioState == AutoAudioOnlyState.autoAudioOnly) return false;
    if (Get.currentRoute == '/audio') return false;
    // 点播有独立音轨才可切；直播没有音轨限制
    if (!isLive && !_hasIndependentAudioTrack) return false;
    return true;
  }

  bool get _hasIndependentAudioTrack {
    if (dataSource is FileSource) {
      return !(dataSource as FileSource).isMp4;
    }
    return dataSource.audioSource?.isNotEmpty == true;
  }

  /// 后台 10 秒到点，切到只听
  Future<void> applyAutoAudioOnly() async {
    if (!_canEnterAutoAudioOnly()) {
      _autoAudioState = AutoAudioOnlyState.idle;
      return;
    }

    onlyPlayAudio.value = true;

    if (isLive) {
      // 直播：重拉 only_audio=1
      _autoAudioState = AutoAudioOnlyState.autoAudioOnly;
      await onNeedsPlayerInit?.call();
    } else {
      // 点播：vid=no
      try {
        _videoPlayerController?.setProperty(
          'file-local-options/vid',
          'no',
        );
      } catch (_) {
        onlyPlayAudio.value = false;
        _autoAudioState = AutoAudioOnlyState.idle;
        return;
      }
      _audioReopened = false;
      _autoAudioState = AutoAudioOnlyState.autoAudioOnly;
    }
  }

  /// 回到前台，恢复画面
  Future<void> restoreFromAutoAudioOnly() async {
    if (_autoAudioState != AutoAudioOnlyState.autoAudioOnly) return;
    if (_autoAudioRestoreInProgress) return;

    _cancelAutoAudioOnlyTimer();
    _autoAudioRestoreInProgress = true;
    final restoreGeneration = ++_autoAudioRestoreGeneration;
    suppressBufferingIndicator.value = !isLive && !_audioReopened;

    try {
      // 直播，以及自动只听期间已经重开过纯音频 Media 的点播，
      // 都没有可供 vid=auto 复用的视频输入，直接走页面级恢复。
      if (isLive || _audioReopened) {
        await _fullRestore();
        return;
      }

      final player = _videoPlayerController;
      if (player == null || player.state.playlist.medias.isEmpty) {
        await _fullRestore();
        return;
      }

      onlyPlayAudio.value = false;

      // 第一层：在当前 EDL Media 上重新选择视频轨，不重开 Media。
      try {
        player.setProperty('file-local-options/vid', 'auto');
      } catch (_) {
        await _fullRestore();
        return;
      }
      applyVideoPictureParameters();

      final restoredByTrackSwitch = await _waitForVideoOutput(
        restoreGeneration,
        checkCurrentState: false,
      );
      if (!_isAutoAudioRestoreCurrent(restoreGeneration)) {
        return;
      }

      if (restoredByTrackSwitch) {
        _completeAutoAudioRestore();
        return;
      }

      if (!_canUseAutoAudioRestoreFallback(player)) {
        _completeAutoAudioRestore();
        return;
      }

      // 第二层：当前 EDL 仍可复用时，只重开这一个 Media。
      if (await _reopenCurrentMedia(restoreGeneration)) {
        _completeAutoAudioRestore();
        return;
      }

      if (_isAutoAudioRestoreCurrent(restoreGeneration) &&
          _canUseAutoAudioRestoreFallback(player)) {
        // 第三层：当前 Media 也无法恢复时，交给页面重新初始化资源。
        await _fullRestore();
      } else if (_isAutoAudioRestoreCurrent(restoreGeneration)) {
        _completeAutoAudioRestore();
      }
    } finally {
      if (restoreGeneration == _autoAudioRestoreGeneration) {
        _autoAudioRestoreInProgress = false;
        suppressBufferingIndicator.value = false;
      }
    }
  }

  /// 等待当前 Media 恢复真实视频输出。
  Future<bool> _waitForVideoOutput(
    int restoreGeneration, {
    bool checkCurrentState = true,
  }) {
    final player = _videoPlayerController;
    if (player == null) return Future.value(false);
    if (checkCurrentState && _hasVideoOutput(player)) {
      return Future.value(true);
    }

    final completer = _autoAudioRestoreCompleter = Completer<bool>();
    final subscription = player.stream.videoParams.listen((_) {
      if (!_isAutoAudioRestoreCurrent(restoreGeneration)) {
        _finishAutoAudioRestoreWait(false);
      } else if (_hasVideoOutput(player)) {
        _finishAutoAudioRestoreWait(true);
      }
    });
    _autoAudioRestoreSubscription = subscription;
    _autoAudioRestoreTimer = Timer(
      const Duration(seconds: 4),
      () => _finishAutoAudioRestoreWait(false),
    );
    return completer.future;
  }

  bool _hasVideoOutput(Player player) {
    final state = player.state;
    if (state.track.video.id == 'no') return false;
    final params = state.videoParams;
    final width = params.dw ?? params.w ?? 0;
    final height = params.dh ?? params.h ?? 0;
    return width > 0 && height > 0;
  }

  bool _canUseAutoAudioRestoreFallback(Player player) =>
      playerStatus.isPlaying || player.state.buffering;

  bool _isAutoAudioRestoreCurrent(int restoreGeneration) =>
      restoreGeneration == _autoAudioRestoreGeneration &&
      _autoAudioState == AutoAudioOnlyState.autoAudioOnly &&
      _lastAppLifecycleState != AppLifecycleState.hidden &&
      _lastAppLifecycleState != AppLifecycleState.paused &&
      _lastAppLifecycleState != AppLifecycleState.detached &&
      !isPipMode;

  void _completeAutoAudioRestore() {
    _autoAudioState = AutoAudioOnlyState.idle;
    _audioReopened = false;
  }

  void _finishAutoAudioRestoreWait(bool result) {
    final completer = _autoAudioRestoreCompleter;
    if (completer == null || completer.isCompleted) return;
    _autoAudioRestoreTimer?.cancel();
    _autoAudioRestoreTimer = null;
    unawaited(_autoAudioRestoreSubscription?.cancel());
    _autoAudioRestoreSubscription = null;
    _autoAudioRestoreCompleter = null;
    completer.complete(result);
  }

  void _interruptAutoAudioRestore() {
    if (!_autoAudioRestoreInProgress && _autoAudioRestoreCompleter == null) {
      return;
    }
    _autoAudioRestoreGeneration++;
    _autoAudioRestoreInProgress = false;
    suppressBufferingIndicator.value = false;
    _finishAutoAudioRestoreWait(false);
  }

  /// 第二层：重开当前 Media，避免立即进入页面级 playerInit 的加载状态。
  Future<bool> _reopenCurrentMedia(int restoreGeneration) async {
    final player = _videoPlayerController;
    if (player == null || player.state.playlist.medias.isEmpty) return false;
    if (!_isAutoAudioRestoreCurrent(restoreGeneration)) return false;

    try {
      final shouldResumePlayback = player.state.playing;
      await player.open(
        player.state.playlist.medias.last.copyWith(
          start: player.state.position,
        ),
        play: shouldResumePlayback,
      );
      applyVideoPictureParameters(player);
    } catch (_) {
      return false;
    }

    if (!_isAutoAudioRestoreCurrent(restoreGeneration)) return false;
    return _waitForVideoOutput(restoreGeneration);
  }

  /// 第三层：点播 playerInit，直播 queryLiveUrl。
  Future<void> _fullRestore() async {
    suppressBufferingIndicator.value = false;
    onlyPlayAudio.value = false;
    _completeAutoAudioRestore();
    await onNeedsPlayerInit?.call();
  }

  /// 生命周期变化 — 由 view 转发
  void handleAutoAudioOnlyLifecycle(AppLifecycleState state) {
    _lastAppLifecycleState = state;

    switch (state) {
      case AppLifecycleState.hidden:
      case AppLifecycleState.paused:
        _cancelAutoAudioOnlyTimer();
        if (_autoAudioRestoreInProgress) {
          _interruptAutoAudioRestore();
        }
        // 已在只听/手动听视频时，保持状态不退出
        if (_autoAudioState == AutoAudioOnlyState.autoAudioOnly ||
            _autoAudioState == AutoAudioOnlyState.manualAudioOnly) {
          break;
        }
        if (isPipMode) {
          _autoAudioState = AutoAudioOnlyState.pipHold;
        } else if (_canEnterAutoAudioOnly()) {
          _autoAudioOnlyTimer = Timer(
            const Duration(seconds: 10),
            applyAutoAudioOnly,
          );
          _autoAudioState = AutoAudioOnlyState.arm;
        }
        break;
      case AppLifecycleState.resumed:
        _cancelAutoAudioOnlyTimer();
        if (_autoAudioState == AutoAudioOnlyState.autoAudioOnly) {
          if (!_autoAudioRestoreInProgress) {
            unawaited(restoreFromAutoAudioOnly());
          }
        } else if (_autoAudioState == AutoAudioOnlyState.arm ||
            _autoAudioState == AutoAudioOnlyState.pipHold) {
          _autoAudioState = AutoAudioOnlyState.idle;
        }
        // manualAudioOnly / pipHold / arm / idle：保持状态不重置
        break;
      case AppLifecycleState.inactive:
        // 过渡状态，忽略
        break;
      case AppLifecycleState.detached:
        _cancelAutoAudioOnlyTimer();
        _interruptAutoAudioRestore();
        break;
    }
  }

  /// 系统 PiP 变化时由 onPipChanged 调用
  void handleAutoAudioOnlyPipChanged(bool isInPip) {
    if (isInPip) {
      _cancelAutoAudioOnlyTimer();
      if (_autoAudioRestoreInProgress) {
        _interruptAutoAudioRestore();
      }
      if (_autoAudioState == AutoAudioOnlyState.arm) {
        _autoAudioState = AutoAudioOnlyState.pipHold;
      }
    } else {
      // PiP 关闭
      _cancelAutoAudioOnlyTimer();
      if (_lastAppLifecycleState == AppLifecycleState.resumed) {
        if (_autoAudioState == AutoAudioOnlyState.autoAudioOnly) {
          unawaited(restoreFromAutoAudioOnly());
        } else if (_autoAudioState == AutoAudioOnlyState.arm ||
            _autoAudioState == AutoAudioOnlyState.pipHold) {
          _autoAudioState = AutoAudioOnlyState.idle;
        }
        // manualAudioOnly 保持不动
      } else {
        // 仍在后台：仅当不在只听/手动听时重新 arm
        if (_autoAudioState == AutoAudioOnlyState.autoAudioOnly ||
            _autoAudioState == AutoAudioOnlyState.manualAudioOnly) {
          return;
        }
        _autoAudioState = AutoAudioOnlyState.arm;
        _autoAudioOnlyTimer = Timer(
          const Duration(seconds: 10),
          applyAutoAudioOnly,
        );
      }
    }
  }

  Future<void> dispose() async {
    if (_disposeRequested) {
      // 已在销毁流程中（或已销毁），幂等返回
      return;
    }
    _cancelAutoAudioOnlyTimer();
    _interruptAutoAudioRestore();
    _autoAudioState = AutoAudioOnlyState.idle;
    _audioReopened = false;
    _lastAppLifecycleState = null;
    // 每次减1，最后销毁
    resetScreenRotation();
    cancelLongPressTimer();
    _cancelSpeedLockHintTimer();
    speedLocked = false;
    _speedLockArmed = false;
    speedLockHint.value = SpeedLockHint.none;
    _cancelSubForSeek();
    if (!isCloseAll && _playerCount > 1) {
      _playerCount -= 1;
      _heartDuration = 0;
      return;
    }

    // 走到此处才是真正的销毁（而非计数递减）。立即标记：异步收尾期间新页面
    // 通过 getInstance()/ensureInstance() 获取实例时会丢弃本实例并新建
    // （见 _dropDisposedInstance），避免把僵尸实例交给新页面。
    _disposeRequested = true;
    _playerCount = 0;
    if (removeSafeArea) {
      showSystemBar();
    }
    danmakuController = null;
    danmakuMaskPath.value = null;
    _stopOrientationListener();
    _disableAutoEnterPip();
    setPlayCallBack(null);
    dmState.clear();
    hideDesktopProgressPreview();
    if (showSeekPreview) {
      _clearPreview();
    }
    if (Platform.isAndroid) {
      AndroidHelper$ToDart.onUserLeaveHint = null;
    } else if (Platform.isIOS) {
      IOSPipHelper.dispose();
    }
    _timer?.cancel();
    persistVideoPictureSettings();
    // _position.close();
    // _playerEventSubs?.cancel();
    // _sliderPosition.close();
    // _sliderTempPosition.close();
    // _isSliderMoving.close();
    // _duration.close();
    // _buffered.close();
    // _showControls.close();
    // _controlsLock.close();

    // playerStatus.close();
    // dataStatus.close();

    if (PlatformUtils.isDesktop && isAlwaysOnTop.value) {
      windowManager.setAlwaysOnTop(false);
    }

    _removeListeners();
    _positionListeners.clear();
    _statusListeners.clear();
    _stopWakeLockTimer();
    WakelockPlus.disable();
    if (kDebugMode) {
      debugPrint('dispose player');
    }
    // 正在创建数据源/VideoController 或重建渲染路径时先等它收尾
    final pending = _playerQueue.tail;
    if (pending != null) {
      try {
        await pending.timeout(const Duration(seconds: 3));
      } catch (_) {}
    }
    await _disposePlayerSafely(_videoPlayerController);
    _videoPlayerController = null;
    _videoControllerNotifier.value = null;
    _activeVideoContextKey = null;
    danmakuMaskPath.dispose();
    // 收尾期间新页面可能已通过 _dropDisposedInstance 领走/重建了单例：
    // 此时不能把新实例的静态引用清掉，也不能 clear 新页面正在使用的
    // 全局 service handler（否则新页面黑屏、控件失效）
    if (identical(_instance, this)) {
      _instance = null;
    }
    if (_instance == null) {
      videoPlayerServiceHandler?.clear();
    }
    HarmonyChannel.releaseContinuation(this);
  }

  /// 销毁底层 Player 前给 OhosVideoController 的在途回调留收尾时间。
  ///
  /// 该控制器的 videoParams 回调链会异步回到 Player（SetSurfaceSize 走一次
  /// 平台通道，随后 setProperty），而它的退订挂在 `Player.platform.release` 上、
  /// 由 `Player.dispose()` 内部才执行；销毁瞬间若有回调在途，就会命中
  /// "[Player] has been disposed"（原生侧还可能出现悬空 handle）。
  Future<void> _disposePlayerSafely(Player? player) async {
    if (player == null) return;
    if (videoController != null) {
      await Future.delayed(const Duration(milliseconds: 150));
    }
    await player.dispose();
  }

  static Future<void> updatePlayCount() async {
    if (_instance?._playerCount == 1) {
      await _instance?.dispose();
    } else {
      _instance?._playerCount -= 1;
    }
  }

  void setContinuePlayInBackground() {
    continuePlayInBackground.value = !continuePlayInBackground.value;
    if (!tempPlayerConf) {
      setting.put(
        SettingBoxKey.continuePlayInBackground,
        continuePlayInBackground.value,
      );
    }
    // 关掉后台播放时，停止自动只听状态机
    if (!continuePlayInBackground.value) {
      onAutoAudioOnlySettingChanged();
    }
  }

  void setOnlyPlayAudio() {
    onlyPlayAudio.value = !onlyPlayAudio.value;
    videoPlayerController?.setVideoTrack(
      onlyPlayAudio.value ? VideoTrack.no() : VideoTrack.auto(),
    );
  }

  late final Map<String, ui.Image?> previewCache = {};
  LoadingState<VideoShotData>? videoShot;
  late final RxBool showPreview = false.obs;
  late final showSeekPreview = Pref.showSeekPreview;
  late final previewIndex = RxnInt();
  final RxBool showDesktopProgressFeedback = false.obs;
  final Rx<double?> desktopProgressHoverValue = Rx<double?>(null);
  final Rx<double?> desktopProgressPreviewValue = Rx<double?>(null);
  final RxInt progressPreviewSeconds = 0.obs;
  final RxInt desktopProgressPreviewLayoutVersion = 0.obs;
  bool _desktopProgressPointerInside = false;
  bool requireDesktopProgressHoverReenter = false;

  bool get _canUseSeekPreview =>
      !isFileSource && showSeekPreview && cid != null;

  void refreshDesktopProgressPreviewLayout() {
    desktopProgressPreviewLayoutVersion.value++;
  }

  double _progressValueFromDuration(Duration value) {
    if (durationInMilliseconds <= 0) {
      return 0.0;
    }
    return (value.inMilliseconds / durationInMilliseconds).clamp(0.0, 1.0);
  }

  void _setDesktopProgressPreviewState(Duration value) {
    final progressValue = _progressValueFromDuration(value);
    progressPreviewSeconds.value = value.inSeconds;
    desktopProgressHoverValue.value = progressValue;
    desktopProgressPreviewValue.value = progressValue;
  }

  void updateDesktopProgressPreviewFromDrag(Duration value) {
    if (!PlatformUtils.isDesktop) {
      return;
    }
    _setDesktopProgressPreviewState(value);
    showDesktopProgressFeedback.value = true;
    if (!isFileSource && showSeekPreview) {
      updatePreviewIndex(value.inSeconds);
    }
  }

  void onDesktopProgressDragStart(Duration value) {
    _desktopProgressPointerInside = true;
    requireDesktopProgressHoverReenter = false;
    updateDesktopProgressPreviewFromDrag(value);
  }

  void onDesktopProgressHoverStart(Duration value) {
    _desktopProgressPointerInside = true;
    if (controlsLock.value || isSeeking.value) {
      return;
    }
    if (requireDesktopProgressHoverReenter) {
      return;
    }
    _setDesktopProgressPreviewState(value);
    showDesktopProgressFeedback.value = true;
    if (!isFileSource && showSeekPreview) {
      updatePreviewIndex(value.inSeconds);
    }
  }

  void onDesktopProgressHoverUpdate(Duration value) {
    _desktopProgressPointerInside = true;
    if (controlsLock.value || isSeeking.value) {
      return;
    }
    if (requireDesktopProgressHoverReenter) {
      return;
    }
    _setDesktopProgressPreviewState(value);
    showDesktopProgressFeedback.value = true;
    if (!isFileSource && showSeekPreview) {
      updatePreviewIndex(value.inSeconds);
    }
  }

  void onDesktopProgressHoverEnd() {
    _desktopProgressPointerInside = false;
    requireDesktopProgressHoverReenter = false;
    if (!isSeeking.value) {
      hideDesktopProgressPreview();
    }
  }

  void hideDesktopProgressPreview() {
    showDesktopProgressFeedback.value = false;
    desktopProgressHoverValue.value = null;
    desktopProgressPreviewValue.value = null;
    if (showSeekPreview) {
      showPreview.value = false;
    }
  }

  void _setPreviewIndexFromShot(int seconds) {
    final shot = videoShot;
    if (shot case Success(:final response)) {
      previewIndex.value = max(
        0,
        (response.index.where((item) => item <= seconds).length - 2),
      );
    }
  }

  Future<void> ensureVideoShot() async {
    if (!_canUseSeekPreview) {
      return;
    }
    if (videoShot is Loading) {
      return;
    }
    if (videoShot case Success()) {
      return;
    }

    final requestBvid = bvid;
    final requestCid = cid!;
    videoShot = LoadingState.loading();
    final response = await VideoHttp.videoshot(
      bvid: requestBvid,
      cid: requestCid,
    );
    if (requestBvid != _bvid || requestCid != cid) {
      return;
    }

    videoShot = response;
    if (!showPreview.value) {
      return;
    }
    if (response case Success()) {
      _setPreviewIndexFromShot(progressPreviewSeconds.value);
    } else {
      showPreview.value = false;
      previewIndex.value = null;
    }
  }

  void updatePreviewIndex(int seconds) {
    if (!_canUseSeekPreview) {
      return;
    }
    showPreview.value = true;
    if (videoShot == null) {
      previewIndex.value = null;
      unawaited(ensureVideoShot());
      return;
    }
    if (videoShot is Loading) {
      previewIndex.value = null;
      return;
    }
    if (videoShot case Success()) {
      _setPreviewIndexFromShot(seconds);
    } else {
      showPreview.value = false;
      previewIndex.value = null;
    }
  }

  void _clearPreview() {
    showPreview.value = false;
    previewIndex.value = null;
    videoShot = null;
    for (final i in previewCache.values) {
      i?.dispose();
    }
    previewCache.clear();
  }

  /// 上游的 getVideoShot 与之并存的 ensureVideoShot（本 fork）：
  /// ensureVideoShot 会做 _canUseSeekPreview 守卫 + 迟到响应丢弃，
  /// 保留本 fork 版本，删除上游的简单版避免命名冲突。
  int _screenshotState = 0;
  Future<void> takeScreenshot() async {
    if (_screenshotState > 0) return;
    _screenshotState = 1;
    SmartDialog.showToast('截图中');
    // 鸿蒙 media_kit fork 的 screenshot 返回 Uint8List（上游 fork 返回 ui.Image）
    final bytes = await videoPlayerController?.screenshot(format: 'image/png');
    if (bytes != null) {
      SmartDialog.showToast('点击弹窗保存截图');
      await showDialog<bool>(
        context: Get.context!,
        builder: (context) => GestureDetector(
          onTap: () async {
            Get.back(result: false);
            final time = DurationUtils.formatDuration(
              positionInMilliseconds / 1000,
            ).replaceAll(':', '-');
            ImageUtils.saveByteImg(
              bytes: bytes,
              fileName: 'screenshot_${cid}_$time',
            );
          },
          child: Align(
            alignment: Alignment.centerRight,
            child: Padding(
              padding: const EdgeInsets.only(right: 12),
              child: ConstrainedBox(
                constraints: BoxConstraints(
                  maxWidth: min(MediaQuery.widthOf(context) / 3, 350),
                ),
                child: DecoratedBox(
                  decoration: BoxDecoration(
                    border: Border.all(
                      width: 5,
                      color: ColorScheme.of(context).surface,
                    ),
                  ),
                  child: Padding(
                    padding: const EdgeInsets.all(5),
                    child: Image.memory(bytes),
                  ),
                ),
              ),
            ),
          ),
        ),
      );
    } else {
      SmartDialog.showToast('截图失败');
    }
    _screenshotState = 0;
  }

  void onPopInvokedWithResult(
    bool didPop,
    Object? result, {
    bool pauseOnPop = true,
  }) {
    if (didPop) {
      if (pauseOnPop && playerStatus.isPlaying) {
        pause();
      }

      setPlayCallBack(null);

      if ((Platform.isAndroid || OS.isHarmony) && _playerCount <= 1) {
        // 只有真的离开播放才切断系统自动画中画。pauseOnPop 为 false 表示
        // 本次 pop 是在进应用内小窗或归位到下层视频页，播放并未结束，这
        // 一刀关掉 auto-start 后没有任何地方补得回来（补开依赖 playing
        // 事件，而小窗期间播放是连续的）：小窗期间退后台不再转系统画中画，
        // 且本次视频的后台画中画会一直失效到下一个视频。
        if (pauseOnPop) {
          _disableAutoEnterPip();
        }
        if (!setSystemBrightness) {
          ScreenBrightnessPlatform.instance.resetApplicationScreenBrightness();
        }
      }

      return;
    }

    if (controlsLock.value) {
      onLockControl(false);
      return;
    }
    if (isDesktopPip) {
      exitDesktopPip();
      return;
    }
    if (isFullScreen.value) {
      triggerFullScreen(status: false);
      return;
    }
    Get.back();
  }
}
