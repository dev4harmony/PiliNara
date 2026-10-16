# AGENT.md — PiliNara 鸿蒙分支合并策略与后续开发指南

> 本文档面向在本仓库（`pilinara-ohos/PiliNara`）上继续开发的 AI/人类协作者。
> 目的：让「把 PiliNara 适配到 HarmonyOS」这件事可以用**最少的 token / 最少的人工冲突**长期做下去。
>
> 本文件本身在 `.gitignore` 里未被忽略，可以提交；若你不想提交，加入 `.gitignore` 即可。

---

## 0. TL;DR

- 本仓库 = **PiliNara 的产品代码** + **从 PiliPlus `ohos` 分支移植的鸿蒙平台适配层**。
- **不要把两个仓库当成互不相干的代码库做手工 diff/merge**：它们同源，upstream 提交 SHA 完全一致，
  存在真实的 merge base，可以直接 `git merge`。这是本方案 token 效率的关键。
- 产品行为、界面、分支特性一律以 **PiliNara（OURS/HEAD）** 为准；鸿蒙侧只贡献**平台使能**代码。
- 依赖上，**Nara 的依赖清单是基准**，鸿蒙的兼容性改写通过 `dependency_overrides` 表达，
  从而既不丢 Nara 的 fork 依赖，又能让鸿蒙插件生效。
- 未适配项一律**显式 TODO 化**（见 §6），不要静默删除。
- 上游关于构建，CI等的更改，一般不要合并

---

## 1. 仓库与分支拓扑

| 角色 | 路径 / URL | 说明 |
| --- | --- | --- |
| 本仓库 | `D:\workspace\pilinara-ohos\PiliNara` | 产出物；分支 `ohos` = 交付分支，`main` = PiliNara 原始状态 |
| 产品上游 | `origin` → `https://api.gitproxy.dev/github.com/meneurwz-glitch/PiliNara.git`（镜像）<br>`true-origin` → `https://github.com/Starfallan/PiliNara.git`（真源） | PiliNara 本体，需要持续同步 |
| 鸿蒙适配来源 | `ohosref` → `D:\workspace\PiliPlus`（分支 `ohos`） | 只取平台适配层 |
| PiliPlus 上游 | `https://github.com/bggRGjQaUbCoE/PiliPlus` | 只作参考 |
| PiliPlus 鸿蒙组织 | `https://github.com/dev4harmony/PiliPlus` | 参考实现作者 |

**关键 SHA（写死在这里，方便复现本次合并）**

| 名称 | SHA | 说明 |
| --- | --- | --- |
| merge base | `32538c4d705c9f5c74747cd00bacdd061d4aed99` | `git merge-base PiliNara.main PiliPlus.ohos`，即 PiliNara 最后一次合并的上游提交 |
| PiliNara HEAD | `f74288d84` | 2026-09-16 |
| PiliPlus ohos HEAD | `58b059e12` | 2026-10-01 "build: update .fvmrc" |

**环境**

| 项 | 值 |
| --- | --- |
| 鸿蒙 Flutter SDK | `D:\workspace\flutter_flutter`，`3.44.9+ohos-0.0.1-canary1`（Dart 3.12.2） |
| Flutter engine | `5a2a6a42cce67f965cf540fcecf616faca624aa1` |
| DevEco SDK | `DEVECO_SDK_HOME=C:\Program Files\Huawei\DevEco Studio\sdk`，API 26 |
| 应用包名 | `com.dev4harmony.pilinara` |

> ⚠️ **不要动 flutter SDK / ohos SDK 目录**（约定见原始需求）。

---

## 2. 为什么用 `git merge` 而不是模式化的手工搬运

先看量级（`git diff --no-index` 目录级对比）：

| 对比 | 文件数 | 增 | 删 |
| --- | --- | --- | --- |
| PiliPlus `lib/` vs PiliNara `lib/` | 579 | 54,122 | 49,183 |
| PiliNara 相对 base（`32538c4d7..f74288d84`） | 313 | 37,768 | 4,272 |
| PiliPlus ohos 相对 base（`32538c4d7..58b059e12`） | 535 | 49,518 | 28,096 |
| 双方都改过的文件 | 148 | — | — |
| 仅 Nara 改过 / 仅 ohos 改过 | 165 / 387 | — | — |

如果按「文件级三分法」手工搬运，需要人工判断 579 个文件；
而 `git merge` 只把 **93 个文件**抛给你，其中真正有语义冲突的 hunk 只有 **205 个**。
一次 merge 的开销远低于 579 次人工比对，而且能保住全部上游历史。

**为什么方向是「把 ohos 合进 PiliNara」而不是反过来**：

- PiliNara 的改动横跨产品代码（313 文件），是「本体」；
- 鸿蒙适配层绝大多数是**新增**（`ohos/` 整个 DevEco 工程、`packages/` 两个 shim、
  `lib/harmony_adapt/`、`lib/media_kit_adapt/`），外加少量调用点分支；
- 以 Nara 为基准，冲突天然收敛在「双方都动过的那 148 个文件」，且裁决规则简单（见 §3）。

> 实测本次 merge：**M 324 / A 106 / UU 86 / D 40 / UD 7**，冲突 93 个文件、7349 行冲突区、205 个 hunk。

---

## 3. 冲突裁决原则（每轮都照这个来）

详细版见 `tool_merge/RESOLUTION_POLICY.md`（随合并引入的辅助文件）。

一句话：**这个产品是 PiliNara；鸿蒙侧只提供平台使能。**

| 情况 | 取哪边 |
| --- | --- |
| 品牌、文案、URL、仓库地址、桌面文件名、Android/iOS bundle id | **OURS**（PiliNara） |
| 鸿蒙包名 `bundleName` | **改为 `com.dev4harmony.pilinara`**（唯一例外） |
| 纯鸿蒙胶水：`OS.isHarmony` 分支、`HarmonyChannel.*`、挖孔/安全区、装饰栏、跨设备接续、ohos 插件调用点、MediaQuery/手势修正 | **THEIRS** |
| `ohos/` DevEco 工程、`packages/{material_ui,cupertino_ui}`、`lib/harmony_adapt/`、`lib/media_kit_adapt/` | **THEIRS**（直接采用） |
| 鸿蒙 CI（`pr_check.yml`、`release.yml`） | **THEIRS**，但把 `PiliPlus` 字样改成 `PiliNara`；`.github/workflows/` 只保留这两个（发 PR → `pr_check.yml` 自动检查，打 tag → `release.yml` 自动发 release），合并时若上游带回其他 workflow 一律**删除** |
| Nara 的 CI（android/ios/linux/mac/win/debug/release_arm64） | **删除**（本仓库只发布鸿蒙侧，这些平台 workflow 已于本轮移除，不得再合回来） |
| 双方各改一处不同关注点 | **两边都要**（例如 `_builder`：既保留 Nara 的 HyperOS padding 修复，也保留 ohos 的 `ListenableBuilder`） |
| Nara 独有功能 vs ohos 的删除 | **保留 Nara**（例：`lib/scripts/patch.ps1`、`tool/jnigen.dart`） |

**硬性纪律**

1. 不留任何冲突标记；不 `git checkout --theirs` 整文件了事（除少数纯新增文件）。
2. 不丢 Nara 功能；不重排/重格式化无关代码（会让下一轮同步再次冲突）。
3. **非冲突区域同样不可信**：git 的自动合并可能产生重复声明
   （本次实测：`lib/utils/utils.dart` 里 `static const channel = MethodChannel(...)` 被合并成两份）。
   ⇒ 每轮合并结束必须跑 `dart analyze`（见 §8），把「合并出来的新错误」清零。

---

## 4. 依赖策略（本方案最核心的部分）

### 4.1 结构

```
dependencies:            ← Nara 的依赖为基准；鸿蒙需要的插件在此"加"
  ...
dependency_overrides:    ← 平台兼容性改写统一放这里（overrides 会绕过版本约束）
  material_ui: {path: packages/material_ui}
  cupertino_ui: {path: packages/cupertino_ui}
  device_info_plus: {git: gitcode CPF-Flutter ...}
  ...
```

参考实现（PiliPlus ohos）的做法：**普通 pub 约束照写，被鸿蒙 fork 替换的实现全部塞进
`dependency_overrides`**。这样：
- 上游 pubspec 的普通约束不会因为鸿蒙而被迫降级（合并时冲突面更小）；
- 只有真正缺鸿蒙实现的包才被改向。

### 4.2 `material_ui` / `cupertino_ui` 本地 shim（必须保留）

Flutter 3.47 把 Material/Cupertino 拆成了独立 pub 包，上游据此一次性改写了 **506 处**
`package:material_ui/material_ui.dart` 与 2 处 `package:cupertino_ui/...` 的 import。
鸿蒙 fork 目前只在 3.44.9，没有这两个包。

`packages/material_ui`、`packages/cupertino_ui` 各只有一个文件：

```dart
export 'package:flutter/material.dart' hide TranslateAnimationSource;
```

**退出方式**：鸿蒙 fork 升到 3.47 后，删掉 `packages/` 下这两个目录，
把 `dependency_overrides` 里的 `path:` 改回 `material_ui: ^1.0.0` / `cupertino_ui: ^1.0.0`，
业务代码一行都不用动。

### 4.3 环境约束

```yaml
environment:
  sdk: ">=3.12.2"          # 从 Nara 的 >=3.13.0 放宽：3.44.9 只带 Dart 3.12.2
  flutter: ">=3.44.9"      # 从 Nara 的精确 3.47.2 放宽
```

同时 `.fvmrc` 已改为 `3.44.9`。

### 4.4 被鸿蒙 fork 替换 / 新增的包（能力缺口见 §6）

| 包 | 鸿蒙来源 | 备注 |
| --- | --- | --- |
| `material_ui` / `cupertino_ui` | 本仓库 `packages/` shim | 3.47 后可删 |
| `device_info_plus` | gitcode `CPF-Flutter/flutter_plus_plugins` | 版本被降到 `^10.1.0` |
| `share_plus` | gitcode `CPF-Flutter/flutter_plus_plugins` `br_share_plus-v10.1.1_ohos` | 降到 `^10.1.1` |
| `flutter_inappwebview` | gitcode `CPF-Flutter` `b596ae0...` + `br_v6.1.5_ohos_dev` | |
| `video_player` | gitcode `openharmony-tpc/flutter_packages` `br_video_player-v2.11.1_ohos` | |
| `url_launcher` | gitcode `hellozeronick/url_launcher` | |
| `path_provider` / `shared_preferences` | gitcode `openharmony-tpc/flutter_packages` / `CPF-Flutter` | |
| `image_picker` / `image_cropper` | gitcode `CPF-Flutter` 系列 | |
| `package_info_plus` | gitcode `CPF-Flutter` `package_info_plus-9.0.0-ohos-1.0.0` | |
| `media_kit*` | github `cnoim/media-kit` `feat-ohos` + `My-Responsitories/media-kit` `version_1.2.5` | **⚠ 见 §6.1** |
| `audio_service` / `audio_session` | gitcode `CPF-Flutter/fluttertpc_*` | |
| `native_device_orientation` | gitcode `hellozeronick/fluttertpc_native_device_orientation` `br_2.1.0_ohos` | |
| `catcher_2` | github `HelloZeroNick/catcher_2` | **⚠ 见 §6.2** |
| `get` | github `bggRGjQaUbCoE/getx` | **⚠ 见 §6.2** |
| `floating` | github `qinshah/floating` `3eefccb...` | |
| `chat_bottom_container` | github `qinshah/flutter_chat_packages` `dba2bf1...` | |
| `cached_network_image_ce` | github `My-Responsitories/flutter_cached_network_image_ce` | 钉在某 commit |
| `file_picker` → `file_picker_ohos` | gitcode `hellozeronick/fluttertpc_file_picker` | **包名不同，import 写法变了** |
| `connectivity_plus_ohos` | github `HelloZeroNick/connectivity_plus_ohos` | 鸿蒙专用，新增 |
| `flutter_math_fork` | gitcode `CPF-Flutter/flutter_math_fork` `br_v0.7.4_flutter3.35.7_ohos` | **Nara 独有**（经 `flutter_markdown_plus_latex` 间接引入，PiliPlus 无此包）→ 见 §6.7 |
| 其余 ohos 插件 | `permission_handler_ohos`、`wakelock_plus_ohos`、`battery_plus_ohos`、`app_links_ohos`、`os_type`、`screen_brightness*` | 新增依赖 |

### 4.5 依赖自检

```powershell
flutter pub get                       # 必须成功；pubspec.lock 冲突时直接删掉重建，不要手工解冲突
# 检查有没有"用了但没在 pubspec 里"的包：
Get-ChildItem -Recurse -File -Filter *.dart lib | Select-String 'package:(\w+)/' |
  ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique
```

---

## 5. 鸿蒙适配层的边界

属于「鸿蒙胶水」、可以放心接受 ohos 侧改动的路径：

```
ohos/                              # 整个 DevEco 工程（AppScope/、entry/、hvigor*、oh-package.json5）
packages/material_ui/              # pub 名映射 shim
packages/cupertino_ui/
lib/harmony_adapt/                 # continuation.dart / harmony_channel.dart / shell_bars_observer.dart
lib/media_kit_adapt/               # initializer_isolate.dart / initializer_native_event_loop.dart / media_kit_adapt.dart
lib/plugin/linux_webview.dart      # 鸿蒙分支自带的 Linux 内嵌 webview 实现（替代 desktop_webview_window）
linux/runner/plugins/linux_webview_plugin.{cc,h}
lib/common/widgets/native_top_spacer.dart
lib/common/utils/status_bar_tap.dart
lib/plugin/pl_player/widgets/top_inset_padding.dart
lib/utils/screenshot.dart
lib/utils/image_memory_cleaner.dart
.vscode/{tasks.json,build_env.dart}
.github/workflows/{pr_check.yml,release.yml}
```

判据：**删掉它，非鸿蒙平台是否仍然完全正常？**
是 → 属于鸿蒙胶水，可以整块接受；
否（它同时改了 Android/iOS/桌面行为）→ 必须逐 hunk 裁决，走 §3。

---

## 6. TODO / 未适配清单

> 约定：代码里留 `TODO-JNI` / `TODO-OHOS` 注释，本节登记。**禁止静默删除 Nara 功能**。

### 6.1 media_kit：Nara 的 `native` fork 被替换（现指向 HelloZeroNick 私有 fork）

- 现状：`dependency_overrides` 里 media_kit 全系列指向 `HelloZeroNick/media-kit` `feat-secondary-subtitle`
  （fork 自 `cnoim/media-kit` `feat-ohos` @ `762fdc6368fb95ad61ab2d99a9cf70a59a00c2e3`
  + 副字幕提交 `d1a8fb90`，media_kit **1.2.3**）。
  Nara 原本用 `Starfallan/media-kit` `native`
  （resolved `83dc986255a260932ccf3cfa865da31956050eb2`，media_kit **1.1.11**）。
- 影响：**Nara 在 `native` 分支上的改进不再生效**，两版 API 有实质差异；
  副字幕（`setSecondarySubtitleTrack` + `SubtitleViewConfiguration` 4 参数）已按
  Starfallan/native 在本 fork 补齐（提交 `d1a8fb90`）。
- ⚠️ 下表 4 处仍按 cnoim 语义改写 —— **再次合并时不要“还原”成 Nara 写法**（会重新编译失败）；
  标 ✅ 的 2 处已在本 fork 补齐并恢复 Nara 原实现：

| Nara fork 能力 | cnoim 1.2.3 / 本 fork | 已采用的写法 |
| --- | --- | --- |
| `Player.setProperty(k, v)`（同步 `void`） | 只在 `NativePlayer` 上有 `Future<void> setProperty(...)`（`media_kit/lib/src/player/native/player/real.dart`） | `lib/media_kit_adapt/media_kit_adapt.dart` 末尾新增 `extension PlayerExtension on Player`，转发给 `platform?.maybeAsNativePlayer`；两个调用方（`lib/plugin/pl_player/controller.dart`、`lib/pages/video/widgets/header_control.dart`）本就 import 该文件 |
| `Player.current`（`Playlist` 别名） | 无此 getter | `player.state.playlist.medias`（`PlayerState.playlist` → `Playlist.medias`）×3，均在 `lib/plugin/pl_player/controller.dart` |
| `Player.setSecondarySubtitleTrack(...)` ✅ | 本 fork `d1a8fb90` 已实现：`sub-add` 用 `auto` 标志（不动主字幕选择）、等 track-list 新条目（5s 超时回退扫描 `state.tracks`）后写 `secondary-sid`；track-list 处理解析 `selected` / `main-selection`（0=sid, 1=secondary-sid）推导 `state.track.secondarySubtitle` | 已恢复：`lib/pages/video/controller.dart` `setSecondarySubtitle` + `_setSecondarySubtitleTrack`（disposed 守卫 + try/catch，镜像 `_setSubtitleTrack`），`vttSecondarySubtitlesIndex` 随选择更新 |
| `SubtitleViewConfiguration` 的 `strokeStyle` / `secondaryStyle` / `secondaryStrokeStyle` / `spacing` ✅ | 本 fork `d1a8fb90` 已补齐（`spacing` 默认 4.0；副字幕未显式给 `secondaryStyle` 时回落主 `strokeStyle` 描边） | 已恢复：`lib/plugin/pl_player/controller.dart` `getSubConfig` 回填 4 参数（保留 `textScaler: TextScaler.noScaling`）；`lib/plugin/pl_player/view/view.dart` SubtitleView 构造透传 4 字段（现版本由外部管理拖拽/padding，需逐字段重建 config） |
| `PlayerStream.size`（`Stream<(int,int)>`）+ 非空 `state.width/height` | 只有 `Stream<int?> width` / `Stream<int?> height`，`state.width/height` 为 `int?` | 本 fork `support-pilinara` 的 `media_kit_video` 自带 `SimpleVideo`（见下） |
| `setShader(type, player)` 双参 | 第 2 参 `NativePlayer? pp` 是冗余的（`setShader` 内部会自己取 player） | 调用点去掉第 2 参：`unawaited(setShader(defaultSuperResolutionType))`（`lib/plugin/pl_player/controller.dart`） |

- **`SimpleVideo`**：cnoim 里不存在该类。原先 vendored 到 `lib/media_kit_adapt/simple_video.dart`，
  现已**回迁到 fork**：`support-pilinara` 分支新增
  `media_kit_video/lib/src/video/simple_video_texture.dart`（由 `src/video/video.dart` 导出），
  工作区不再保留副本，导入点直接 `package:media_kit_video/media_kit_video.dart`（2 处：
  `lib/plugin/pl_player/view/view.dart`、`lib/common/widgets/pip_mini_video_content.dart`）。
  相对 upstream/main 的差异只有两点：`stream.size` 换成 `stream.width` + `stream.height`
  两条订阅（`_visible` 用 `(_width ?? 0) > 0 && (_height ?? 0) > 0` 判空）；
  平台视图路径——`OhosVideoController.usePlatformView` 为真时挂 `OhosPlatformVideo`，
  否则画 `Texture`（HDR 输出没有纹理 ID，只画纹理会全黑）。
  其余语义（宽高未就绪不渲染、`rect.notifyListeners()` 强制重建、尺寸除以 `devicePixelRatio`）
  保持原样。
- `support-pilinara` 同时 **revert 了 `d669b8d1`**：`SimpleVideo` 进库后，
  `media_kit_video.dart` 不再需要导出 `OhosVideoController` / `OhosPlatformVideo`，
  两者退回 `lib/src` 的实现细节。
- 若将来改用 fork 自带的 `Video`：必须配 `controls: NoVideoControls` 才等价；
  `SubtitleViewConfiguration` 已支持副字幕双行渲染（`d1a8fb90`），`rect` 是否除以 dpr 的尺寸语义仍需复核。
- TODO：`Starfallan/media-kit#native` 的副字幕已移植完成（本 fork `feat-secondary-subtitle`）；
  仍需评估能否把 ohos 支持 + 副字幕 rebase 回 Nara 的 fork，或在非鸿蒙平台条件切换依赖。
- 取源：github.com:443 被网络拦截时，`pub get` 需 `GIT_CONFIG_GLOBAL=<repo>\.gitconfig-gh`
  （内含该 URL 的 `insteadOf` 改写到 `git@github.com:`，经 `ssh.github.com:443` + deploy key 拉取）。

### 6.2 若干 Nara fork 依赖被 ohos fork 顶掉

| 包 | Nara 侧 | 当前实际 |
| --- | --- | --- |
| `get` | `Starfallan/getx#dev` | `bggRGjQaUbCoE/getx` |
| `catcher_2` | `My-Responsitories/catcher_2#dev` | `HelloZeroNick/catcher_2` |
| `audio_service` | `bggRGjQaUbCoE/audio_service#main` | gitcode `CPF-Flutter/fluttertpc_audio_service#br_v0.18.18_ohos` |
| `native_device_orientation` | `bggRGjQaUbCoE/flutter_native_device_orientation#master` | gitcode `hellozeronick/...` `br_2.1.0_ohos` |

- `get` / `catcher_2` 目前**直接用了 ohos 的 fork**，Nara 侧改动未知，需要 diff 后决定是否合并。
- TODO：逐个 `git log --oneline` + `git diff` 对比，把 Nara 的改动搬到鸿蒙 fork 上（或反之）。

### 6.3 JNI / Android 原生绑定在鸿蒙上被掏空

- `lib/utils/android/bindings.g.dart` 从 jnigen 生成物换成了**同名的空实现替身**
  （原因：`jni` 的原生构建钩子在鸿蒙目标不可用）。
- 所有调用点都由 `Platform.isAndroid` 守卫，鸿蒙运行时不执行，因此安全；
  但**如果以后要在鸿蒙上支持这些功能，需要重新实现**。
- 恢复 Android 生成物的方法（已写进 `pubspec.yaml` 注释）：
  加回 `jnigen: ^1.0.0` + `jni: ^1.0.0`，然后 `dart run tool/jnigen.dart`。
- 涉及调用点：`lib/pages/live_room/controller.dart`、`lib/pages/setting/models/extra_settings.dart`、
  `lib/plugin/pl_player/controller.dart`、`lib/plugin/pl_player/view/view.dart`、
  `lib/services/audio_handler.dart`、`lib/utils/device_utils.dart`、`lib/utils/font_utils.dart`、
  `lib/utils/max_screen_size.dart`、`lib/utils/android/android_helper.dart`。

**⚠️ 已确认的安卓侧真实回归：`DeviceUtils.sdkInt` 恒为 0。**
`lib/utils/device_utils.dart:6` 是 `static final int sdkInt = AndroidHelper.sdkInt();`，
而替身 `lib/utils/android/bindings.g.dart:33` 里 `static int sdkInt() => 0;`。
`AndroidHelper.sdkInt()` 在上游由 JNI 读 `Build.VERSION.SDK_INT`，摘掉 `jni` 后安卓构建
拿到 0，会走错分支：

| 位置 | 判断 | 结果 |
| --- | --- | --- |
| `lib/plugin/pl_player/utils/fullscreen.dart:130` | `Platform.isAndroid && DeviceUtils.sdkInt < 29` | 恒真 → 用 `.manual` 而非 `.edgeToEdge` |
| `lib/plugin/pl_player/controller.dart:835` | `DeviceUtils.sdkInt < 31` | 恒真 → 走旧 PiP 回调路径 |
| `lib/services/pip_overlay_service.dart:116` | `DeviceUtils.sdkInt >= 31` | 恒假 → 跳过 Android 12+ 画中画路径 |
| `lib/services/live_pip_overlay_service.dart:75` | 同上 | 同上 |
| `lib/utils/image_utils.dart:114` | `DeviceUtils.sdkInt < 29` | 恒真 → 走旧版保存路径 |

TODO（三选一）：① 在 `device_utils.dart` 里对 `Platform.isAndroid` 用
`android.os.Build.VERSION` 之类的替代来源；② 把 `jnigen`/`jni` 恢复成
Android-only 的 dev/条件依赖；③ 明确接受安卓侧降级并记录在案。
另外替身里 `AndroidHelper.isPipAvailable` 已无调用点（`lib/pages/video/view.dart:883`
注释说明画中画状态改由 `floating` 插件查询）。

**⚠️ 另一处安卓侧真实回归：系统字体列表丢失。**
替身里唯一被删掉的成员是 `AndroidHelper.fontFamilies()`，它的唯一调用方是 HEAD 的
`FontUtils._initAndroid`，而 ohos 重写 `lib/utils/font_utils.dart` 时把该方法删掉了，
所以合并后没有任何调用点需要补桩。代价：**安卓端「设置 → 字体」里读不到系统已装字体，
只剩「默认」+ 用户自己导入的字体池**。恢复同样需要 `jni` + `dart run tool/jnigen.dart`
重新生成，再把 `fontFamilies()` 与 `_initAndroid` 加回来。

### 6.4 `desktop_webview_window` 被移除，Linux 内嵌 webview 降级

- Nara 依赖 `Predidit/linux_webview_window`，用于 `lib/pages/webview/view.dart`、
  `lib/pages/video/note/view.dart`、`lib/pages/login/geetest/geetest_webview_dialog.dart`、
  `lib/utils/linux_cookie_manager.dart`。
- 现状：
  - `lib/utils/linux_cookie_manager.dart` **被 ohos 删除**；
  - 新的 Linux 内嵌实现是 `lib/plugin/linux_webview.dart` +
    `linux/runner/plugins/linux_webview_plugin.{cc,h}`，已接入
    `linux/runner/CMakeLists.txt` 与 `linux/runner/my_application.cc:103`（`LinuxWebviewPluginRegister`）；
  - 但只有 `lib/pages/login/geetest/geetest_webview_dialog.dart:90` 用了 `LinuxWebview`；
    `lib/pages/webview/view.dart:111` 在 `Platform.isLinux` 时**直接返回 "unsupported" 占位页**。
- Windows 侧不受影响（走 `flutter_inappwebview` + `main.dart` 的 `webViewEnvironment`）。
- TODO：Linux 内置 webview 页回归（把 `WebviewPage` 接到 `LinuxWebview`），或恢复原依赖并条件化。

### 6.5 其它需要回归确认的点

- [ ] `flutter_html` 从 Nara 的 git fork 退回 pub `^3.0.0`；确认渲染差异。
- [ ] `saver_gallery` 5.0.2 → 4.1.2、`flex_seed_scheme` 5.0.0 → 4.0.1、
      `permission_handler_android` 14.0.0 → 13.0.1 等降级对 Android 侧的影响。
- [ ] `webview_cookie_manager` 在鸿蒙 pubspec 里被注释掉（Nara 当前无引用，确认即可）。
- [ ] `lib/utils/extension/scroll_controller_ext.dart` 的跳转阈值
      （Nara `viewportDimension * 7` vs ohos `viewportDimension * 2` + `positions.length != 1` 守卫）需人工确认取哪边。
- [ ] 鸿蒙「防窥」功能在 ohos 分支曾被加入后又 revert，确认是否要重新引入。
- [ ] `lib/harmony_adapt/harmony_channel.dart:280-283` 有一个从 PiliPlus 带来的死代码
      `static Future csy(value)`（注释自称「测试用，ai生成信息请忽略这部分更改」），
      原生侧 `HarmonyChannel.ets` 没有对应 handler，全仓库无调用点 —— 建议直接删除。

**通道协议一致性（已逐条核对）**：Dart 侧 `MethodChannel('harmonyChannel')`
（`lib/harmony_adapt/harmony_channel.dart:18`）与原生侧
`new MethodChannel(binding.getBinaryMessenger(), "harmonyChannel")`
（`ohos/entry/src/main/ets/plugins/HarmonyChannel.ets:48`）同名。
Dart 共调用 31 个方法名，原生 `onMethodCall`（`HarmonyChannel.ets:440`）实现 31 个 case；
交集 30 个，唯一不匹配的两个是无害的：
- `csy`：只有 Dart 侧，原生无 handler（即上面的死代码）；
- `setWindowLayoutFullScreen`：只有原生侧，留给 embedding 的 `SystemChrome` 通道。

原生 `EntryAbility.ets:56-73` 通过 `eventHub` 回调的
`showTab` / `onTopSearchTap` / `onTopMsgTap` / `onTopMineTap` / `onHomeTabChange`
在 Dart `handler`（`harmony_channel.dart:56-73`）里都有对应 case。

### 6.6 `share_plus` 必须使用旧的 `Share.*` 静态 API（**易踩坑，改动前先读**）

这是整次合并里**唯一一处「依赖版本和调用方 API 必须同时降级」**的地方：

| | 版本 | 调用方写法 | 位置 |
|---|---|---|---|
| Nara 上游（`HEAD`、base `32538c4d7`） | `share_plus: ^13.1.0` | `SharePlus.instance.share(ShareParams(...))` | 3 处 |
| ohos 适配 | `share_plus: ^10.1.1` + `dependency_overrides` → gitcode `CPF-Flutter/flutter_plus_plugins`，path `packages/share_plus/share_plus`，ref `br_share_plus-v10.1.1_ohos` | `Share.share(...)` / `Share.shareXFiles(...)` | 4 处 |

**关键事实**：那个 fork 虽然 `version: "10.1.1"`，但 `lib/share_plus.dart` 里
**只有 `class Share` 的静态方法**（`share` / `shareXFiles` / `shareUri`），
**不存在 `SharePlus.instance`，也不存在 `ShareParams`** —— 即 `pubspec.yaml:349-353`
指向的是一个停留在更老 API 的鸿蒙移植版。因此全仓库的分享调用点都必须写成 `Share.*` 形式。

当前已改好的 4 处（**不要再改回 `SharePlus.instance`，否则立刻编译失败**）：

- `lib/utils/share_utils.dart:35` — `await Share.share(text, sharePositionOrigin: await sharePositionOrigin)`
  （上方保留了注释 `// 保留 share_plus 10.x API：鸿蒙适配 fork 停留在 br_share_plus-v10.1.1_ohos`）
- `lib/utils/image_utils.dart:44` — `await Share.shareXFiles([XFile(res.path)], sharePositionOrigin: await ShareUtils.sharePositionOrigin)`
- `lib/pages/save_panel/view.dart:308` — `Share.shareXFiles([XFile.fromData(pngBytes, name: picName, mimeType: 'image/png')], sharePositionOrigin: await ShareUtils.sharePositionOrigin)`
- `lib/models_new/download/bili_download_entry_info.dart:197` — `await Share.shareXFiles(xFiles);`

改完可用 `git grep -n 'SharePlus\.instance\|ShareParams' -- lib` 自检，**应为 0 命中**。

fork 本身 `android/ ios/ macos/ windows/ ohos/` 平台目录齐全，对其它平台无害，
所以这是「为了鸿蒙牺牲上游新 API」，不是平台降级。将来若
`br_share_plus-v10.1.1_ohos` 升级到 v11+ API，可把这 4 处一次性改回 `SharePlus.instance`。

### 6.7 `flutter_math_fork` 必须 override（Nara 独有依赖，PiliPlus 没有）

- 来源链：`flutter_markdown_plus_latex: ^1.0.5`（Nara 为公式渲染引入）→ `flutter_math_fork: ^0.7.4`。
  PiliPlus **没有** `flutter_markdown_plus_latex`，所以参考仓库的 `pubspec.lock` 里查不到
  `flutter_math_fork` —— 这个坑只会在本仓库出现，别去 PiliPlus 找答案。
- 症状（**编译期**，`flutter analyze` 抓不到）：`Target kernel_snapshot_program failed`，伴随
  `flutter_math_fork/lib/src/...` 多处
  `The type 'TargetPlatform' is not exhaustively matched by the switch cases since it doesn't match 'TargetPlatform.ohos'`
  （pub 版 0.7.4 只覆盖了 Flutter 官方的 5 个平台，没有鸿蒙的 `TargetPlatform.ohos`）。
- 已加 override（`pubspec.yaml` 的 `dependency_overrides`，紧随 `meta` 之后）：

  ```yaml
  flutter_math_fork: # 数学公式渲染.鸿蒙适配（补 TargetPlatform.ohos 分支）
    git:
      url: https://gitcode.com/CPF-Flutter/flutter_math_fork.git
      ref: br_v0.7.4_flutter3.35.7_ohos   # 默认分支，版本 0.7.4 正好满足 ^0.7.4
  ```

  （同仓另有上游 `https://gitcode.com/openharmony-sig/flutter_math_fork` 可参考。）
- **改完必须重新 `flutter pub get`** 才会解析到 override。

### 6.8 `playerStatus` 是普通字段不是 Rx —— `Obx` 里读 `.value` 必编译失败

- `PlPlayerController.playerStatus` 的类型是普通 `PlayerStatus` enum 字段，
  通过 `addStatusLister` / `removeStatusLister`（源码里就是这个拼写）手动广播到
  `_statusListeners`，**没有 `.value`**。
- 症状：`undefined_getter - The getter 'value' isn't defined for the type 'PlayerStatus'`。
- 已改的 2 处（**再合并时若被写回 `playerStatus.value` 会复发**）：
  - `lib/services/pip_overlay_service.dart` —— 画中画底部播放/暂停按钮
  - `lib/services/live_pip_overlay_service.dart` —— 直播画中画底部播放/暂停按钮

  改法：`Obx` → `Builder` + `StreamBuilder<bool>`，流取
  `plController.videoPlayerController?.stream.playing`（`PlayerStream.playing`），
  `initialData` 取 `player.state.playing`（`PlayerState.playing`）；两文件里
  随之无用的 `play_status.dart` 导入已删除（`Obx` 本身在两文件别处仍在用，`get` 导入保留）。

### 6.9 分析基线：`tool/jnigen.dart` 已从 analyzer 排除

- `analysis_options.yaml` 的 `analyzer.exclude` 多了 `- tool/jnigen.dart`（带 TODO 注释）。
- 原因见 §6.3：恢复 Android 生成物要装 `jnigen` 依赖，而 `verify.ps1` 第 5 步要求
  pubspec **不得**出现 `jnigen`，二者冲突 → 保留文件、只排除分析。
- 恢复路径：装依赖并 `dart run tool/jnigen.dart` 之后，把这行 exclude 删掉即可。
- 注意 `analysis_options` **不包含** `tool/**`，`tool/danmaku_merge_debug.dart` 仍参与分析。

---

## 7. 每轮同步上游的标准动作

### 7.1 同步 PiliNara 本体（常规）

```powershell
cd D:\workspace\pilinara-ohos\PiliNara
git fetch origin                                  # 或 true-origin，注意 origin 是镜像
git checkout ohos
git merge origin/main                             # 冲突只会出现在 §2 那类"双方都改过"的文件
# 按 §3 裁决 → 跑 §8 验证 → commit
```

### 7.2 同步 PiliPlus 鸿蒙适配层（有新适配时）

```powershell
git fetch ohosref "+refs/heads/ohos:refs/remotes/ohosref/ohos"
git merge ohosref/ohos                            # 只取平台层；产品代码冲突按 §3 取 OURS
```

> 若 `ohosref` 是浅克隆会报
> `warning: rejected refs/remotes/ohosref/ohos because shallow roots are not allowed to be updated`。
> 先把参考仓库改成完整克隆（`git -C D:\workspace\PiliPlus fetch --unshallow`）再 fetch。

### 7.3 冲突处理的工具

仓库内保留了本次合并用到的辅助脚本（可保留、可删）：

```powershell
# 只打印某个文件的冲突 hunk，按 OURS/THEIRS 分组
.\tool_merge\showconf.ps1 <文件绝对路径> [每侧最多行数]
```

**踩过的坑（别重复踩）**

- `Select-String -Pattern '^<<<<<<<' -SimpleMatch` **永远匹配不到**（`-SimpleMatch` 下 `^` 是字面量），
  要用 `$_ -like '<<<<<<<*'`。
- `edit` 工具对 CRLF 文件做字面替换会失败，批量改写用 PowerShell 的
  `[System.IO.File]::ReadAllLines / WriteAllLines`。
- `.github/ISSUE_TEMPLATE/` 下的中文文件名用 `Test-Path` 会报
  `Illegal characters in path`（git 以八进制转义输出），要单独处理。
- `pubspec.lock` 有冲突标记时 `flutter pub get` 直接失败：
  `Failed parsing lock file: Error on line 109, column 12 ... Expected ':'`。
  **删掉 lock 文件重新 `pub get` 即可**，不要手工合并锁文件。
- `flutter pub get` 在 pwsh 里可能返回 exit code 1，仅仅因为 Flutter 把
  `Flutter assets will be downloaded from https://storage.flutter-io.cn` 写到 stderr；
  判断成功要看输出里有没有 `Changed N dependencies!`。

---

## 8. 每轮合并后的验证清单

**一条命令跑完全部检查（推荐）：**

```powershell
cd D:\workspace\pilinara-ohos\PiliNara
powershell -NoProfile -ExecutionPolicy Bypass -File .\tool_merge\verify.ps1              # 含 flutter analyze
powershell -NoProfile -ExecutionPolicy Bypass -File .\tool_merge\verify.ps1 -SkipAnalyze # 跳过分析，秒出
```

`tool_merge/verify.ps1` 覆盖 8 组检查：①0 个未合并文件 ②0 处冲突标记
③鸿蒙平台层关键文件存在性（18 个）④包名/品牌（鸿蒙 `com.dev4harmony.pilinara`，
非鸿蒙平台有意保持 `com.example.pilinara`）⑤pubspec 依赖 + `sdk >=3.12.2` /
`flutter >=3.44.9` / `.fvmrc` ⑥Dart↔ArkTS 通道协议（方法名 30/30 对齐、通道名一致）
⑦`GeneratedPluginRegistrant.ets` 的 25 个插件与 import 一一对应、两个 shim 的导出内容
⑧`flutter analyze` 无 error。退出码 = 失败项数。

手工分步执行：

```powershell
cd D:\workspace\pilinara-ohos\PiliNara

# 1) 不能有冲突残留
git diff --name-only --diff-filter=U                 # 必须为空
git grep -n -E '^(<<<<<<<|>>>>>>>) ' -- .           # 必须为空

# 2) 依赖能解析
Remove-Item pubspec.lock -ErrorAction SilentlyContinue
flutter pub get                                      # 期望 "Changed N dependencies!"

# 3) 静态分析（也是发现"自动合并重复声明"的唯一手段，见 §3 纪律 3）
dart analyze --no-fatal-warnings

# 4) 单文件语法快检（合并过程中随时可用）
dart format --output=none <file>                     # 只报解析错误，不写文件
```

> 原始需求明确「完成后不需要构建 HAP」，因此本轮以 `dart analyze` + 依赖解析为准。
> 真正出包时用：

```powershell
# vscode 任务 build_hap，或直接：
dart .vscode/build_env.dart
flutter build hap --release --dart-define-from-file=.vscode/env.json
```

### 已验证过的事实（本轮）

- `flutter pub get` 成功：`Changed 264 dependencies!`（39 个包有更新但被约束排除）。
- `lib/main.dart` 通过 `dart format --output=none` 解析（合并脚本 `tool_merge/fix_main.ps1` 生成）。
- `ohos/` 工程、`packages/` shim、`lib/harmony_adapt/`、`lib/media_kit_adapt/`、
  `.vscode/{tasks.json,build_env.dart}`、`.github/workflows/{pr_check,release}.yml` 均已落地。

### 当前进度快照（本轮末状态）

- 冲突已全部裁决：**0 个未合并文件、0 处冲突标记**；**合并在工作区尚未提交**
  （分支 `ohos`，`MERGE_HEAD = 58b059e128c106d58d424c298a3d18ae1460dbdb`）。
- 合并产物曾带出 **46 个 `flutter analyze` error**，已按 §9 步骤 11 全部修掉。
  之后的一次复跑被中断，**没有留下新的分析文件**：`tool_merge/analyze_now.txt` /
  `analyze_final.txt` 是修复前的过期记录（仍显示 46 error），且均为未跟踪文件、不入库。
- 分析之外，第一次 `flutter build` 又暴露 2 处 `flutter analyze` 抓不到的错误
  （`lib/pages/live_room/widgets/header_control.dart` 把 `part` 文件当库导入 →
  `Error when reading ... draggable_scrollable_sheet.dart` + `DynDraggableScrollableSheet` 未定义）
  以及 §6.7 的 `flutter_math_fork` 穷举问题，均已修。
- **本轮由用户自行执行验证**（协作者只改代码、不自行跑 flutter 命令）；
  验证通过后再按本节流程：提交 → `tool_merge/verify.ps1` → §9 步骤 9 复核 README → 出报告。

---

## 9. 本次合并的流水账（便于复现与回溯）

1. `D:\workspace\PiliNara` → `D:\workspace\pilinara-ohos\PiliNara`（robocopy `/E /MT:16`，`.git` 保留）。
2. 加 remote `ohosref = D:\workspace\PiliPlus`，fetch 其 `ohos` 分支。
3. `git merge-base HEAD ohosref/ohos` = `32538c4d7…`（**真实 merge base，本次方案成立的前提**）。
4. `git checkout -b ohos` → `git merge --no-commit --no-ff ohosref/ohos`
   → **M 324 / A 106 / UU 86 / D 40 / UD 7**，93 个冲突文件、205 个 hunk、7349 行冲突区。
5. 基础层冲突（`.gitignore`、`.github/`、`lib/common/constants.dart`、`lib/http/api.dart`、
   `lib/utils/utils.dart`、`windows/runner/main.cpp`、`pubspec.yaml`、`pubspec.lock`、`README.md`、
   `lib/main.dart`、`assets/linux/com.example.pilinara.desktop`、`ohos/` 品牌字段、
   `lib/plugin/linux_webview.dart`、`linux/runner/plugins/linux_webview_plugin.cc`）由主控直接解决。
6. `lib/**` 的产品代码冲突按域分派给并行子代理处理（播放器 / 视频页 / 直播页 / 信息流与下载 /
   通用组件与工具 / 设置与杂项），统一遵循 `tool_merge/RESOLUTION_POLICY.md`。
7. 品牌与包名落地：`ohos/AppScope/app.json5` 的 `bundleName` → `com.dev4harmony.pilinara`，
   `shortcuts_config.json` 4 处同步；`string.json` ×4 的 `PiliPlus` → `PiliNara`；
   `ohos/oh-package.json5` 的 `name` → `pilinara`；`lib/main.dart` 的鸿蒙下载目录同步改名。
   **Android/iOS/Linux 的 `com.example.pilinara` 保持不变**（那是 Nara 的既有标识）。
8. `.github/workflows/release.yml` 里的 `PiliPlus` 字样改为 `PiliNara`
   （产物名 `PiliNara_ohos_<tag>_unsigned.hap`，pubspec 重命名步骤产出 `name: pilinara`）。
9. `README.md`：头部改写为 PiliNara 鸿蒙版说明 + 编译运行指引，保留 `# 以下是原上游项目README` 之后的内容。
10. `pubspec.lock` 删除重建；`pubspec.yaml`、`pubspec.lock`、基础设施文件全部 `git add`。
11. **合并产物的编译期修复：46 个 `flutter analyze` error → 0**，分三桶执行
    （分析输入见 `tool_merge/analyze_now.txt`，仅存过期版本）：
    - *简单桶*（5 处）：`lib/pages/download/detail/widgets/item.dart` 补
      `common/widgets/dialog/simple_dialog_option.dart` 导入（`DialogOption` 未定义 ×3）；
      `lib/pages/live_room/superchat/superchat_card.dart` 第 4 个菜单项
      `PopupMenuItem(` → `CustomPopupMenuItem<void>(`（与前 3 项统一）；
      `lib/plugin/pl_player/utils/danmaku_options.dart` 删重复的
      `fontFamily: ThemeUtils...` 与随之无用的 `theme_utils.dart` 导入；
      `analysis_options.yaml` 排除 `tool/jnigen.dart`（§6.9）。
    - *结构桶*：`lib/pages/live_room/widgets/header_control.dart`（part 文件导入路径 →
      `common/widgets/flutter/draggable_scrollable_sheet.dart`、build 内补
      `const btnHeight = 30.0;`、`controller.stream!.mapIndexed`）；
      `lib/pages/member_video/controller.dart` 删掉与基类 `common_controller.dart` 冲突的
      `refreshKey` 字段与 onInit 里的赋值、`view.dart` 相应去掉 `!`；
      `lib/plugin/pl_player/view/view.dart` `screenshotWebp` → `_screenshotWebp`；
      两处画中画按钮按 §6.8 改 `StreamBuilder`；副字幕按 §6.1 注释关闭。
    - *media_kit 适配桶*：全部见 §6.1 的表格（`PlayerExtension.setProperty`、
      `player.state.playlist.medias` ×3、`SubtitleViewConfiguration` 裁参、
      `SimpleVideo` 回迁到 `support-pilinara` 分支的 `media_kit_video`、`setShader` 去掉第 2 参、
      补 `import 'package:PiliPlus/pages/setting/models/play_settings.dart' show kMaxVolume;`）。
12. 补 `flutter_math_fork` 的鸿蒙 `dependency_override`（§6.7）—— 它只在 Nara 的依赖链上，
    必须等真实编译才暴露，analyze 不报。
13. **状态：合并尚未提交** —— 待用户验证通过后 `git add -A`（**排除** `tool_merge/analyze_*.txt`）
    → commit（分支 `ohos`，带 MERGE_HEAD）→ `tool_merge/verify.ps1` 8 步验收 →
    §9 步骤 9 复核 `README.md` → 出最终报告（原始需求：**不打 HAP**）。

---

## 10. 给后续 Agent 的三条硬建议

1. **先 `git merge-base` 再决定策略。** 同源仓库永远直接 merge；
   只有确认没有共同历史时，才退化成手工移植。
2. **裁决规则写成文件**（`tool_merge/RESOLUTION_POLICY.md`），
   每轮把「哪些文件属于鸿蒙胶水」的清单保持更新，下一代 Agent 就不需要重新推理。
3. **合并不是终点，`dart analyze` 才是。** 自动合并会在非冲突区制造重复声明/半截代码，
   只有静态分析能把它们抓出来。
