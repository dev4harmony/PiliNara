# Merge resolution policy v3 — sync upstream 2.6.1.1 -> 2.6.2 into HarmonyOS branch

工作区 `D:\github\PiliNara` 正处于 `git merge --no-commit --no-ff 2.6.2`（tag `2.6.2` = `a80c655ac`）中途。

> 上一轮 cherry-pick 已被放弃（它会逐个提交重放 155 个 commit、产生 435 文件冲突）。
> 正确做法是**一次真正的 merge**：真实 merge base 存在，只有 **58** 个冲突文件。

## 三方坐标

| 代号 | 是什么 | 怎么读 |
|---|---|---|
| **OURS** = HEAD | 鸿蒙分支 sync-2.6.2（PiliNara 产品代码 + 鸿蒙适配层 + 本地修复） | `git show HEAD:<path>` |
| **THEIRS** = `2.6.2` | 上游 PiliNara 2.6.2（= `a80c655ac`） | `git show 2.6.2:<path>` |
| **BASE** | `1f87a63d1`（上次合并 upstream 的提交） | `git show 1f87a63d1:<path>` |

查看某个文件的冲突 hunk：`git diff <path>`，或直接看文件里的 `<<<<<<<` 块。

## 唯一总则

**这一轮的目的是把上游 2.6.1.1 -> 2.6.2 的产品更新搬进鸿蒙分支，同时不丢失鸿蒙适配层。**

- **产品行为以 THEIRS 为准**：新功能、重构、bug 修复、UI、设置项、新文件。上游结构就是目标态。
- **鸿蒙平台层以 OURS 为准，且一个都不许丢**：`OS.isHarmony` / `Platform.isOhos` 分支、
  `package:PiliNara/harmony_adapt/*`、`media_kit_adapt/*` 导入、`HarmonyChannel.*`、
  挖孔/安全区/装饰栏/状态栏处理、ohos 插件调用点、ohos 特有的 MediaQuery/手势修正、
  `ohos/` 目录树、`packages/{material_ui,cupertino_ui}` shim、`.vscode` 的 ohos 任务。
- **两边各改一处不同关注点时：两边都要。** 采用 THEIRS 的新结构，再把 OURS 的平台守卫/本地修复织回去。
  这是本轮最常见的情况。

## 必须保住的本地方案（回归即失败，见 AGENT.md）

| 项 | 内容 | 依据 |
|---|---|---|
| 环境/依赖钉死 | `sdk >=3.12.2`、`flutter >=3.44.9`、`.fvmrc 3.44.9`、gitcode ohos fork、`flutter_math_fork` override、`material_ui`/`cupertino_ui` 本地 path shim | AGENT.md 4.3 / 6.7 |
| `share_plus` 旧 API | `Share.share` / `Share.shareXFiles`（**不存在** `SharePlus.instance` / `ShareParams`） | AGENT.md 6.6 |
| `file_picker` | `package:file_picker_ohos/file_picker_ohos.dart` | AGENT.md 4.4 |
| `playerStatus` 普通字段 | 不许写 `.value`；PiP 服务用 `Builder` + `StreamBuilder<bool>` | AGENT.md 6.8 |
| media_kit 适配 | `PlayerExtension.setProperty`、`player.state.playlist.medias`（无 `Player.current`）、`SubtitleViewConfiguration` 4 参、`SimpleVideo` 来自 fork 不再 vendored、`setShader` 1 参 | AGENT.md 6.1 |
| JNI 替身 | `lib/utils/android/bindings.g.dart` 保持 no-jni 桩，**不要**换回 jnigen 生成物 | AGENT.md 6.3 |
| 品牌 | `PiliNara`、`dev4harmony/PiliNara`、非鸿蒙平台保持 `com.example.pilinara`；鸿蒙包名 `com.dev4harmony.pilinara` | AGENT.md 3 |

## 依赖策略（本轮极其关键，别自由发挥）

实测三方依赖差异（`dependencies` / `dev_dependencies` / `dependency_overrides`）：

- 上游相对 base **只新增 3 个**：`nm`、`webview_all_linux`、`webview_platform_interface`；**删除 0 个**。
- 上游把 `dependency_overrides`（media_kit 全家桶指向 `Starfallan/media-kit#native` 等）加了回来，
  并新增 `media_kit_libs_video` / `media_kit_video` / `flutter_inappwebview_android` / `flutter_inappwebview_windows` 等 override。
- 我们相对 base 新增 36 个条目，全部是鸿蒙必需（ohos 插件 + gitcode fork + 本地 shim）。
- 上游与我们**同名不同源**的 override：`media_kit*`（他们是 `Starfallan/media-kit#native`，我们是 `HelloZeroNick/media-kit` ohos fork）。

**裁决**：
1. `environment` 保持 OURS（`>=3.12.2` / `>=3.44.9`），**不要**接受上游的 `3.13.0` / `3.47.9`。
2. `dependency_overrides` 保持 OURS 的整套 media_kit 指向（ohos fork），不要被上游覆盖。
3. 上游新增的 `nm` 可以加（上游 UI 用到）；`webview_all_linux` / `webview_platform_interface`
   只有在业务代码确实 import 时才加 —— **先 grep 确认，别投机添加**。
4. `pubspec.lock` **不要手工解冲突**：删掉重建即可（主控统一处理）。
5. `jni` / `jnigen` 保持**不引入**（AGENT.md 6.3 / 6.9 的既定取舍）。

## 硬性纪律

1. 不留任何 `<<<<<<<` / `=======` / `>>>>>>>` 标记。
2. 不许为了「冲突好解」而丢弃上游功能，也不许丢弃鸿蒙支持。
3. 冲突 hunk 之外**不要重排/重格式化**（会让下一轮同步再次冲突）。
4. **不许跑任何会改状态的 git 命令**（`add`/`commit`/`rm`/`checkout`/`stash`/`merge --abort`）。只读 git 可以。
5. 只改分配给你的文件；其他文件有别的成员在并行处理。
6. **不要跑 `flutter pub get` / `dart analyze`** —— 主控最后统一跑。
7. 单文件语法自检：`dart format --output=none <file>`（只报解析错误，不写文件）。
   若 `dart` 不在 PATH，用 `D:\workspace\flutter_flutter\bin\dart.bat`。

## 完成标准

每个交付文件：内容已裁决、能解析、无冲突标记，并给出一行报告：
`path -> ours|theirs|both — 一句话理由`
