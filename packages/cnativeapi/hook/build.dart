// Builds the nativeapi core, C ABI included, into the shared library that the
// `@Native` functions in lib/src/bindings_generated.dart resolve against,
// together with this package's own event loop shim (src/, bound in
// lib/src/event_loop.dart).
//
// The source lists mirror core/src/CMakeLists.txt; keep the two in step.

import 'dart:io';

import 'package:code_assets/code_assets.dart';
import 'package:hooks/hooks.dart';
import 'package:logging/logging.dart';
import 'package:native_toolchain_c/native_toolchain_c.dart';

void main(List<String> args) async {
  await build(args, (input, output) async {
    if (!input.config.buildCodeAssets) {
      return;
    }
    final targetOS = input.config.code.targetOS;
    // PiliNara 鸿蒙分支本地 shim：Ohos 目标直接跳过原生库编译。
    //
    // ── 为什么必须拦：鸿蒙构建一定会跑本 hook ──
    // AotOhosAssetBundle.dependencies = [LinkHooks, KernelSnapshot,
    // InstallCodeAssets]，且 LinkHooks 依赖 BuildHooks；只要依赖树里有带
    // hook/build.dart 的包就必然执行（build_planner 只判断文件是否存在，
    // 没有平台过滤）。hook 抛异常 = 整次 `flutter build hap` 失败
    // （throwToolExit，不是 warning），且 nativeAssets feature 在 stable
    // 默认开启，无法用 flag 关掉。
    //
    // ── 为什么拦得住：target_os 会被伪装成 linux ──
    // 鸿蒙 Flutter SDK 的 OhosAssetTarget（flutter_tools/lib/src/isolated/
    // native_assets/targets.dart）类内部用 `super(os: OS.ohos)` 供 Flutter
    // 自己过滤/打包资产，但发给 hook input 的 CodeAssetExtension 里写的是
    // `targetOS: OS.linux`，注释原文：
    //   "The hosted package does not know OHOS yet, so use a known OS without
    //    extra protocol configuration in the hook input ... Hooks that support
    //    Linux code assets may run their Linux path for OHOS until this
    //    compatibility shim is removed."
    // 因此本 hook 在鸿蒙构建时拿到的是 target_os=linux，会走 case OS.linux，
    // 上游那句 `_ => throw UnsupportedError` 实际不可达。
    //
    // ── 不拦会怎样 ──
    // 走 Linux 分支 = 执行 `pkg-config --cflags gtk+-3.0 x11 xi`。
    // Windows 主机上没有 pkg-config → ProcessException 直接崩（实测）；
    // 即便有，也会按 GTK/X11 链接出错误产物（鸿蒙上根本没有 GTK）。
    //
    // ── 判据为什么是编译器路径 ──
    // target_os 已被伪装，不能用它识别鸿蒙；但同一个 shim 会把 cCompiler
    // 换成 DevEco / OpenHarmony SDK 的 llvm 工具链，路径含 openharmony，
    // 这是当前 SDK 上唯一可靠的信号。
    //
    // `targetOS.name == 'ohos'` 这半句保留作前瞻：SDK 里留有
    // `TODO(ohos): Revert to OS.ohos once hosted package:code_assets
    // recognizes OHOS.`，一旦兑现，target_os 会变回 ohos，此分支即生效。
    //
    // ── 跳过是否安全 ──
    // nativeapi 及其上层（window_manager / tray_manager）在鸿蒙运行时不执行：
    // window_manager 至今不在 ohos 的 GeneratedPluginRegistrant 里，与替换前
    // 使用的 window_manager fork 情况一致。Dart 侧 @ffi.Native external 声明
    // 编译期不需要原生库，只有运行时真正调用才会失败，而鸿蒙侧无调用路径。
    //
    // 其它平台（含桌面 CI）行为与上游完全一致，不受影响。
    if (targetOS.name == 'ohos' || _isOhosToolchain(input.config.code.cCompiler)) {
      return;
    }
    final core = _locateCore(input.packageRoot);
    final src = core.resolve('src/');

    final platformDir = switch (targetOS) {
      OS.android => 'android',
      OS.iOS => 'ios',
      OS.linux => 'linux',
      OS.macOS => 'macos',
      OS.windows => 'windows',
      _ => throw UnsupportedError('cnativeapi does not support $targetOS'),
    };
    final apple = targetOS == OS.iOS || targetOS == OS.macOS;
    final shim = input.packageRoot.resolve('src/');
    final shimSource = switch (targetOS) {
      OS.macOS => 'event_loop_macos.mm',
      OS.windows => 'event_loop_windows.cpp',
      OS.linux => 'event_loop_linux.cpp',
      _ => 'event_loop_other.cpp',
    };

    final sources = [
      ..._list(src, '', '.cpp'),
      ..._list(src, 'foundation/', '.cpp'),
      ..._list(src, 'capi/', '.cpp'),
      ..._list(src, 'platform/$platformDir/', apple ? '.mm' : '.cpp').where(
        // The WinUI 3 backend is opt-in in core's CMake build and needs the
        // Windows App SDK; the Win32 implementations stand in for it.
        (path) => !RegExp(
          r'(menu_winui3|message_dialog_winui3|window_winui3|winui3_runtime|view_winui3)_windows\.cpp$',
        ).hasMatch(path),
      ),
      shim.resolve(shimSource).toFilePath(),
    ];

    final flags = <String>[];
    final libraries = <String>[];
    final libraryDirectories = <String>[];
    var frameworks = <String>[];
    switch (targetOS) {
      case OS.macOS:
        flags.addAll(_objectiveCpp);
        // SMAppService is macOS 13+, so link ServiceManagement weakly. With a
        // normal link dyld refuses to load the whole library on macOS 12,
        // even when the app never calls launch-at-login.
        flags.addAll(['-weak_framework', 'ServiceManagement']);
        frameworks = [
          'Cocoa',
          'QuartzCore',
          'Carbon',
          'IOKit',
          'CoreGraphics',
          'ApplicationServices',
        ];
      case OS.iOS:
        flags.addAll(_objectiveCpp);
        frameworks = ['UIKit', 'Foundation', 'CoreGraphics'];
      case OS.linux:
        flags.addAll(await _pkgConfig(['--cflags', ..._linuxPackages]));
        // Libraries go after the sources, where --as-needed keeps them.
        for (final flag in await _pkgConfig(['--libs', ..._linuxPackages])) {
          if (flag.startsWith('-l')) {
            libraries.add(flag.substring(2));
          } else if (flag.startsWith('-L')) {
            libraryDirectories.add(flag.substring(2));
          } else {
            flags.add(flag);
          }
        }
        flags.add('-pthread');
      case OS.android:
        libraries.addAll(['log', 'android']);
      case OS.windows:
        flags.addAll(['/EHsc', '/utf-8']);
        libraries.addAll([
          'user32',
          'shell32',
          'ole32',
          'uuid',
          'comctl32',
          'dwmapi',
          'gdiplus',
          'crypt32',
          'advapi32',
          'version',
          'gdi32',
        ]);
    }

    final builder = CBuilder.library(
      name: 'cnativeapi',
      assetName: 'cnativeapi.dart',
      sources: sources,
      includes: [src.toFilePath(), shim.toFilePath()],
      frameworks: frameworks,
      libraries: libraries,
      libraryDirectories: libraryDirectories,
      flags: flags,
      language: Language.cpp,
      std: 'c++17',
      // Native assets package this one library; no Gradle CMake target supplies
      // libc++_shared.so. Keep the NDK runtime inside our Android library.
      cppLinkStdLib: targetOS == OS.android ? 'c++_static' : null,
    );
    await builder.run(
      input: input,
      output: output,
      logger: Logger('')
        ..level = Level.ALL
        ..onRecord.listen((record) => print(record.message)),
    );
  });
}

/// Compiles every source as Objective-C++ under ARC. The flags come after the
/// `-x c++` that [Language.cpp] adds, so they win.
const _objectiveCpp = [
  '-x',
  'objective-c++',
  '-fobjc-arc',
  '-DOBJC_OLD_DISPATCH_PROTOTYPES=0',
];

const _linuxPackages = ['gtk+-3.0', 'x11', 'xi'];

/// The core sources: a published package carries a copy in cxx_impl/ (vendored
/// by the release workflow); inside the workspace repository the package
/// builds the checkout's core/ directly.
Uri _locateCore(Uri packageRoot) {
  for (final candidate in [
    packageRoot.resolve('cxx_impl/'),
    packageRoot.resolve('../../../core/'),
  ]) {
    if (File.fromUri(candidate.resolve('src/CMakeLists.txt')).existsSync()) {
      return candidate;
    }
  }
  throw StateError(
    'nativeapi core sources not found next to ${packageRoot.toFilePath()}; '
    'run `git submodule update --init core` in the repository',
  );
}

/// The files directly in [dir] (relative to [src]) ending in [extension],
/// sorted so the build is reproducible.
List<String> _list(Uri src, String dir, String extension) {
  final directory = Directory.fromUri(src.resolve(dir));
  return [
    for (final entity in directory.listSync())
      if (entity is File && entity.path.endsWith(extension)) entity.path,
  ]..sort();
}

/// 判断传入的 C 工具链是否指向鸿蒙（OHOS / OpenHarmony）SDK。
///
/// 鸿蒙 Flutter SDK 的 OhosAssetTarget 会把 hook input 的 target_os 设成
/// linux（见 flutter_tools/lib/src/isolated/native_assets/targets.dart），
/// 所以不能只靠 targetOS 判断。但那个 shim 同时会把 cCompiler 换成
/// DevEco / OpenHarmony SDK 自带的 llvm 工具链，路径里带 openharmony。
bool _isOhosToolchain(CCompilerConfig? cCompiler) {
  if (cCompiler == null) return false;
  final path = cCompiler.compiler.toFilePath().toLowerCase();
  return path.contains('openharmony') ||
      path.contains('deveco') ||
      path.contains('hmos');
}

Future<List<String>> _pkgConfig(List<String> args) async {
  final ProcessResult result;
  try {
    result = await Process.run('pkg-config', args);
  } on ProcessException {
    // pkg-config 不存在（典型场景：Windows 主机上跑 flutter test，或
    // 鸿蒙构建把 target_os 伪装成 linux 传进来）。这种情况下没有 GTK/X11
    // 需要链接，返回空即可，不要打断整个构建。
    return const <String>[];
  }
  if (result.exitCode != 0) {
    throw StateError(
      'pkg-config ${args.join(' ')} failed; install the GTK 3, X11 and Xi '
      'development packages.\n${result.stderr}',
    );
  }
  return (result.stdout as String)
      .split(RegExp(r'\s+'))
      .where((flag) => flag.isNotEmpty)
      .toList();
}
