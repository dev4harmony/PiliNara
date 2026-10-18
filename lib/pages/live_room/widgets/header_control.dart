import 'dart:io' show Platform;
import 'dart:math' as math;

import 'package:PiliPlus/common/style.dart';
import 'package:PiliPlus/common/widgets/flutter/draggable_scrollable_sheet.dart';
import 'package:PiliPlus/common/widgets/flutter/popup_menu.dart';
import 'package:PiliPlus/common/widgets/marquee.dart';
import 'package:PiliPlus/pages/live_room/controller.dart';
import 'package:PiliPlus/pages/setting/models/play_settings.dart'
    show showPlayerVolumeDialog;
import 'package:PiliPlus/pages/video/widgets/header_control.dart';
import 'package:PiliPlus/pages/video/widgets/header_mixin.dart';
import 'package:PiliPlus/plugin/pl_player/controller.dart';
import 'package:PiliPlus/plugin/pl_player/widgets/common_btn.dart';
import 'package:PiliPlus/services/shutdown_timer_service.dart'
    show shutdownTimerService;
import 'package:PiliPlus/utils/android/android_helper.dart';
import 'package:PiliPlus/utils/extension/context_ext.dart';
import 'package:PiliPlus/utils/extension/size_ext.dart';
import 'package:PiliPlus/utils/ios/pip_helper.dart';
import 'package:PiliPlus/utils/platform_utils.dart';
import 'package:PiliPlus/utils/storage.dart';
import 'package:PiliPlus/utils/storage_key.dart';
import 'package:collection/collection.dart';
import 'package:floating/floating.dart';
import 'package:flutter_smart_dialog/flutter_smart_dialog.dart';
import 'package:font_awesome_flutter/font_awesome_flutter.dart';
import 'package:get/get.dart';
import 'package:material_design_icons_flutter/material_design_icons_flutter.dart';
import 'package:material_ui/material_ui.dart';
import 'package:os_type/os_type.dart';

class LiveHeaderControl extends StatefulWidget {
  const LiveHeaderControl({
    super.key,
    required this.title,
    required this.upName,
    required this.plPlayerController,
    required this.onSendDanmaku,
    required this.onPlayAudio,
    required this.isPortrait,
    required this.liveController,
    required this.onlineWidget,
  });

  final String? title;
  final String? upName;
  final PlPlayerController plPlayerController;
  final VoidCallback onSendDanmaku;
  final VoidCallback onPlayAudio;
  final bool isPortrait;
  final LiveRoomController liveController;
  final Widget onlineWidget;

  @override
  State<LiveHeaderControl> createState() => _LiveHeaderControlState();
}

class _LiveHeaderControlState extends State<LiveHeaderControl>
    with HeaderMixin, TimeBatteryMixin {
  @override
  late final plPlayerController = widget.plPlayerController;

  @override
  bool get horizontalScreen => true;

  @override
  bool get isFullScreen => plPlayerController.isFullScreen.value;

  @override
  bool get isPortrait => widget.isPortrait;

  @override
  Widget build(BuildContext context) {
    final isFullScreen = this.isFullScreen;
    showCurrTimeIfNeeded(isFullScreen);
    final liveController = widget.liveController;
    final showCastButton =
        PlatformUtils.isDesktop || !context.mediaQuerySize.isPortrait;
    Widget child;
    child = Obx(
      key: titleKey,
      () => MarqueeText(
        liveController.title.value,
        spacing: 30,
        velocity: 30,
        strutStyle: const StrutStyle(fontSize: 15, leading: 0),
        style: const TextStyle(fontSize: 15, height: 1, color: Colors.white),
      ),
    );
    // 全屏时的第二行信息（主播名/人看过/高能观众/开播时长）。
    // 它原先和标题叠在同一列里，而那一列只有右侧那排按钮排完后剩下的宽度，
    // 内容排不下时 Row 溢出又不裁剪，文字会直接画到按钮底下糊成一团。改为
    // 单独放到标题行下面占满整行：按钮与标题同处一行（视觉齐平），第二行
    // 也不再被按钮挤。
    Widget? infoRow;
    if (isFullScreen) {
      infoRow = Row(
        spacing: 10,
        children: [
          if (widget.upName case final upName?)
            Text(
              upName,
              style: const TextStyle(
                fontSize: 12,
                color: Colors.white,
              ),
            ),
          liveController.watchedWidget,
          widget.onlineWidget,
          liveController.timeWidget,
        ],
      );
    }
    child = Expanded(child: child);

    const btnHeight = 30.0;

    final appBar = AppBar(
      backgroundColor: Colors.transparent,
      foregroundColor: Colors.white,
      primary: false,
      automaticallyImplyLeading: false,
      titleSpacing: 14,
      // 全屏时标题行只有一行文字，用默认的 56 会在挖孔和标题之间空出一大
      // 截、两行之间也被撑开；收紧到按钮实际高度（ComBtn 30 + 上下各 2），
      // 标题与第二行的位置和拆行前保持一致。
      toolbarHeight: isFullScreen ? 34 : null,
      title: Row(
        children: [
          if (isFullScreen || plPlayerController.isDesktopPip)
            ComBtn(
              height: 30,
              tooltip: '返回',
              icon: const Icon(FontAwesomeIcons.arrowLeft, size: 15),
              onTap: () {
                if (plPlayerController.isDesktopPip) {
                  plPlayerController.exitDesktopPip();
                } else {
                  plPlayerController.triggerFullScreen(status: false);
                }
              },
            ),
          child,
          ...?timeBatteryWidgets,
          const SizedBox(width: 10),
          if (PlatformUtils.isDesktop && !plPlayerController.isDesktopPip)
            Obx(() {
              final isAlwaysOnTop = plPlayerController.isAlwaysOnTop.value;
              return ComBtn(
                height: 30,
                tooltip: '${isAlwaysOnTop ? '取消' : ''}置顶',
                icon: isAlwaysOnTop
                    ? const Icon(
                        size: 18,
                        Icons.push_pin,
                        color: Colors.white,
                      )
                    : const Icon(
                        size: 18,
                        Icons.push_pin_outlined,
                        color: Colors.white,
                      ),
                onTap: () => plPlayerController.setAlwaysOnTop(!isAlwaysOnTop),
              );
            }),
          if (isFullScreen || PlatformUtils.isDesktop)
            ComBtn(
              height: 30,
              tooltip: '发弹幕',
              icon: const Icon(
                size: 18,
                Icons.comment_outlined,
                color: Colors.white,
              ),
              onTap: widget.onSendDanmaku,
            ),
          if ((Platform.isAndroid && PiliAndroidHelper.isPipAvailable) ||
              OS.isHarmony ||
              (Platform.isIOS && IOSPipHelper.isAvailable) ||
              (PlatformUtils.isDesktop && !isFullScreen))
            ComBtn(
              height: 30,
              tooltip: '画中画',
              onTap: () async {
                if (PlatformUtils.isDesktop) {
                  plPlayerController.toggleDesktopPip();
                  return;
                }
                if (await Floating().isPipAvailable) {
                  final status = await plPlayerController.enterPip();
                  if (OS.isHarmony && status == PiPStatus.unavailable) {
                    // 系统小窗（自由窗口）内画中画无法启动，插件会拒绝
                    SmartDialog.showToast('当前处于系统小窗，无法进入画中画');
                  }
                }
              },
              icon: const Icon(
                size: 18,
                Icons.picture_in_picture_outlined,
                color: Colors.white,
              ),
            ),
          if (showCastButton)
            ComBtn(
              height: 30,
              tooltip: '投屏',
              onTap: liveController.onCast,
              icon: const Icon(
                size: 18,
                Icons.cast,
                color: Colors.white,
              ),
            ),
          Obx(
            () {
              final onlyPlayAudio = plPlayerController.onlyPlayAudio.value;
              return ComBtn(
                height: btnHeight,
                tooltip: '仅播放音频',
                onTap: () {
                  plPlayerController.onlyPlayAudio.toggle();
                  plPlayerController.markManualOnlyPlayAudio(
                    plPlayerController.onlyPlayAudio.value,
                  );
                  widget.onPlayAudio();
                },
                icon: onlyPlayAudio
                    ? const Icon(
                        size: 18,
                        MdiIcons.musicCircle,
                        color: Colors.white,
                      )
                    : const Icon(
                        size: 18,
                        MdiIcons.musicCircleOutline,
                        color: Colors.white,
                      ),
              );
            },
          ),
          if (PlatformUtils.isMobile)
            Obx(() {
              final continuePlayInBackground =
                  plPlayerController.continuePlayInBackground.value;
              return ComBtn(
                height: 30,
                tooltip: '${continuePlayInBackground ? '关闭' : ''}后台播放',
                onTap: plPlayerController.setContinuePlayInBackground,
                icon: continuePlayInBackground
                    ? const Icon(
                        size: 18,
                        Icons.play_circle,
                        color: Colors.white,
                      )
                    : const Icon(
                        size: 18,
                        Icons.play_circle_outline,
                        color: Colors.white,
                      ),
              );
            }),
          ComBtn(
            height: 30,
            tooltip: '定时关闭',
            onTap: () => shutdownTimerService.showScheduleExitDialog(
              context,
              isFullScreen: isFullScreen,
              isLive: true,
            ),
            icon: const Icon(
              size: 18,
              Icons.schedule,
              color: Colors.white,
            ),
          ),
          if (plPlayerController.videoPlayerController case final player?)
            SizedBox.square(
              dimension: 30,
              child: StaticPopupMenuButton(
                iconSize: 18,
                padding: .zero,
                menuPadding: EdgeInsets.zero,
                menuItemOuterPadding: EdgeInsets.zero,
                icon: const Icon(Icons.more_vert, color: Colors.white),
                itemBuilder: (context) => [
                  PopupMenuItem(
                    height: 42,
                    onTap: _showLiveStreamDialog,
                    child: const Row(
                      spacing: 8,
                      children: [
                        Icon(Icons.alt_route, size: 20),
                        Text('切换路线', style: TextStyle(fontSize: 14)),
                      ],
                    ),
                  ),
                  if (!showCastButton)
                    PopupMenuItem(
                      height: 42,
                      onTap: liveController.onCast,
                      child: const Row(
                        spacing: 8,
                        children: [
                          Icon(Icons.cast, size: 20),
                          Text('投屏', style: TextStyle(fontSize: 14)),
                        ],
                      ),
                    ),
                  // 全屏下也能进入（点击后先退全屏再收起）；系统 PiP 中没有控制栏，排除
                  // 桌面小窗形态下不能 pop 本页，不提供入口
                  if (!plPlayerController.isDesktopPip &&
                      !plPlayerController.isPipMode)
                    PopupMenuItem(
                      height: 42,
                      onTap: () => liveController.onRequestInAppPip?.call(),
                      child: const Row(
                        spacing: 8,
                        children: [
                          Icon(Icons.picture_in_picture_alt_outlined, size: 20),
                          Text('应用内画中画', style: TextStyle(fontSize: 14)),
                        ],
                      ),
                    ),
                  PopupMenuItem(
                    height: 42,
                    child: const Row(
                      spacing: 8,
                      children: [
                        Icon(Icons.info_outline, size: 20),
                        Text('播放信息', style: TextStyle(fontSize: 14)),
                      ],
                    ),
                    onTap: () => HeaderControlState.showPlayerInfo(
                      context,
                      player: player,
                    ),
                  ),
                  if (!plPlayerController.onlyPlayAudio.value)
                    PopupMenuItem(
                      height: 42,
                      onTap: showVideoPictureParameters,
                      child: const Row(
                        spacing: 8,
                        children: [
                          Icon(Icons.tune, size: 20),
                          Text('视频参数', style: TextStyle(fontSize: 14)),
                        ],
                      ),
                    ),
                  if (PlatformUtils.isMobile)
                    PopupMenuItem(
                      height: 42,
                      child: Row(
                        spacing: 8,
                        children: [
                          const Icon(Icons.volume_up, size: 20),
                          Text(
                            '播放器音量: ${player.state.volume.toStringAsFixed(0)}%',
                            style: const TextStyle(fontSize: 14),
                          ),
                        ],
                      ),
                      onTap: () => showPlayerVolumeDialog(
                        context,
                        () {},
                        onChanged: player.setVolume,
                      ),
                    ),
                ],
              ),
            ),
        ],
      ),
    );
    if (infoRow == null) {
      return appBar;
    }
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        appBar,
        Padding(
          // 左端与标题文字对齐：AppBar 的 titleSpacing + 返回按钮宽度
          padding: const EdgeInsets.only(
            left: 14 + 34,
            right: 14,
            top: 3,
            bottom: 4,
          ),
          child: infoRow,
        ),
      ],
    );
  }

  void _showLiveStreamDialog() {
    final controller = widget.liveController;
    if (controller.stream == null) {
      SmartDialog.showToast('直播流信息未就绪');
      return;
    }
    showModalBottomSheet(
      context: context,
      useSafeArea: true,
      isScrollControlled: true,
      constraints: BoxConstraints(
        maxWidth: math.min(640, context.mediaQueryShortestSide),
      ),
      builder: (context) {
        final maxChildSize =
            PlatformUtils.isMobile && !context.mediaQuerySize.isPortrait
            ? 1.0
            : 0.7;
        return DynDraggableScrollableSheet(
          minChildSize: 0,
          maxChildSize: maxChildSize,
          snap: true,
          expand: false,
          snapSizes: [maxChildSize],
          initialChildSize: maxChildSize,
          builder: (context, scrollController) {
            final theme = Theme.of(context);
            final secondary = theme.colorScheme.secondary;
            final onSurfaceVariant = theme.colorScheme.onSurfaceVariant;
            final currStyle = TextStyle(fontSize: 14, color: secondary);
            return Theme(
              data: theme.copyWith(dividerColor: Colors.transparent),
              child: Column(
                children: [
                  InkWell(
                    onTap: Get.back,
                    borderRadius: Style.bottomSheetRadius,
                    child: SizedBox(
                      height: 35,
                      child: Center(
                        child: Container(
                          width: 32,
                          height: 3,
                          decoration: BoxDecoration(
                            color: theme.colorScheme.outline,
                            borderRadius: const .all(.circular(1.5)),
                          ),
                        ),
                      ),
                    ),
                  ),
                  Expanded(
                    child: ListView(
                      controller: scrollController,
                      padding: .only(
                        bottom: MediaQuery.viewPaddingOf(context).bottom + 100,
                      ),
                      children: controller.stream!.mapIndexed((si, stream) {
                        final isCurrStream = si == controller.streamIndex;
                        final streamColor = isCurrStream
                            ? secondary
                            : onSurfaceVariant;
                        return _ExpansionTile(
                          initiallyExpanded: isCurrStream,
                          iconColor: streamColor,
                          collapsedIconColor: streamColor,
                          title: Text(
                            stream.protocolName ?? si.toString(),
                            style: isCurrStream
                                ? currStyle
                                : const TextStyle(fontSize: 14),
                          ),
                          children: stream.format.mapIndexed((fi, format) {
                            final isCurrFormat =
                                isCurrStream && fi == controller.formatIndex;
                            final formatColor = isCurrFormat
                                ? secondary
                                : onSurfaceVariant;
                            return _ExpansionTile(
                              initiallyExpanded: isCurrFormat,
                              iconColor: formatColor,
                              collapsedIconColor: formatColor,
                              title: Text(
                                format.formatName ?? fi.toString(),
                                style: isCurrFormat
                                    ? currStyle
                                    : const TextStyle(fontSize: 14),
                              ),
                              children: format.codec.mapIndexed((ci, codec) {
                                final isCurrCodec =
                                    isCurrFormat && ci == controller.codecIndex;
                                final codecColor = isCurrCodec
                                    ? secondary
                                    : onSurfaceVariant;
                                return _ExpansionTile(
                                  initiallyExpanded: isCurrCodec,
                                  iconColor: codecColor,
                                  collapsedIconColor: codecColor,
                                  title: Text(
                                    '${codec.codecName ?? ci.toString()} (${codec.currentQn.desc})',
                                    style: isCurrCodec
                                        ? currStyle
                                        : const TextStyle(fontSize: 14),
                                  ),
                                  children: codec.urlInfo.mapIndexed((ui, url) {
                                    final isCurrUrl =
                                        isCurrCodec &&
                                        ui == controller.liveUrlIndex;
                                    return ListTile(
                                      dense: true,
                                      title: Text(
                                        '${url.host}...',
                                        style: isCurrUrl
                                            ? const TextStyle(fontSize: 14)
                                            : TextStyle(
                                                fontSize: 14,
                                                color: onSurfaceVariant,
                                              ),
                                      ),
                                      selected: isCurrUrl,
                                      onTap: isCurrUrl
                                          ? null
                                          : () {
                                              Get.back();
                                              controller.initLiveUrl(
                                                streamIndex: si,
                                                formatIndex: fi,
                                                codecIndex: ci,
                                                liveUrlIndex: ui,
                                              );
                                              GStorage.setting.put(
                                                SettingBoxKey.liveStream,
                                                [
                                                  stream.protocolName!,
                                                  format.formatName!,
                                                  codec.codecName!,
                                                ],
                                              );
                                            },
                                    );
                                  }).toList(),
                                );
                              }).toList(),
                            );
                          }).toList(),
                        );
                      }).toList(),
                    ),
                  ),
                ],
              ),
            );
          },
        );
      },
    );
  }
}

class _ExpansionTile extends ExpansionTile {
  const _ExpansionTile({
    required super.title,
    super.initiallyExpanded,
    super.iconColor,
    super.collapsedIconColor,
    super.children,
  }) : super(
         dense: true,
         childrenPadding: const .only(left: 20),
       );
}
