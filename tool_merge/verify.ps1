# 合并结果验收脚本 —— 对应 AGENT.md §8「验收清单」
# 用法: pwsh -File tool_merge/verify.ps1  (在仓库根目录执行)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$fail = 0
function Step($name) { Write-Host ""; Write-Host "=== $name ===" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "  [OK]   $m" -ForegroundColor Green }
function Bad($m)  { Write-Host "  [FAIL] $m" -ForegroundColor Red; $script:fail++ }
function Warn($m) { Write-Host "  [WARN] $m" -ForegroundColor Yellow }

Step '1. 未合并文件（应为 0）'
$u = @(git diff --name-only --diff-filter=U)
if ($u.Count -eq 0) { Ok '无未合并文件' } else { Bad "仍有 $($u.Count) 个未合并文件"; $u | Select-Object -First 20 | ForEach-Object { "         $_" } }

Step '2. 冲突标记（应为 0 行）'
$markers = @(git grep -n -E '^(<<<<<<<|=======$|>>>>>>>) ' -- . 2>$null)
if ($markers.Count -eq 0) { Ok '无冲突标记' } else { Bad "发现 $($markers.Count) 处冲突标记"; $markers | Select-Object -First 20 | ForEach-Object { "         $_" } }

Step '3. 关键鸿蒙平台层文件存在性'
$must = @(
  'ohos/AppScope/app.json5', 'ohos/entry/oh-package.json5', 'ohos/oh-package.json5',
  'ohos/entry/src/main/ets/plugins/HarmonyChannel.ets',
  'ohos/entry/src/main/ets/plugins/GeneratedPluginRegistrant.ets',
  'packages/material_ui/pubspec.yaml', 'packages/material_ui/lib/material_ui.dart',
  'packages/cupertino_ui/pubspec.yaml', 'packages/cupertino_ui/lib/cupertino_ui.dart',
  'lib/harmony_adapt/harmony_channel.dart', 'lib/harmony_adapt/continuation.dart',
  'lib/harmony_adapt/shell_bars_observer.dart',
  'lib/media_kit_adapt/media_kit_adapt.dart',
  '.vscode/tasks.json', '.vscode/build_env.dart',
  '.github/workflows/pr-check-ohos.yml', '.github/workflows/release-ohos.yml',
  'lib/utils/android/bindings.g.dart'
)
foreach ($f in $must) {
  if (Test-Path -LiteralPath $f) { Ok $f } else { Bad "缺失: $f" }
}

Step '4. 包名 / 品牌（鸿蒙必须是 com.dev4harmony.pilinara）'
$bundle = (Select-String -LiteralPath 'ohos/AppScope/app.json5' -Pattern 'bundleName' | Select-Object -First 1).Line
if ($bundle -match 'com\.dev4harmony\.pilinara') { Ok "app.json5 bundleName = $($bundle.Trim())" } else { Bad "bundleName 未改: $bundle" }
$sc = Get-Content -LiteralPath 'ohos/entry/src/main/resources/base/profile/shortcuts_config.json' -Raw -Encoding utf8
if ($sc -match 'com\.example\.piliplus') { Bad 'shortcuts_config.json 仍含 com.example.piliplus' } else { Ok 'shortcuts_config.json 已改' }
if (Test-Path -LiteralPath 'ohos/entry/src/main/resources/base/element/string.json') {
  $sj = Get-Content -LiteralPath 'ohos/entry/src/main/resources/base/element/string.json' -Raw -Encoding utf8
  if ($sj -match 'PiliPlus') { Bad 'string.json 仍含 PiliPlus' } else { Ok 'string.json 已改' }
}
# 非鸿蒙平台保持 com.example.pilinara（有意为之）
$lin = (Select-String -LiteralPath 'linux/CMakeLists.txt' -Pattern 'APPLICATION_ID').Line
if ($lin -match 'com\.example\.pilinara') { Ok "Linux 保持 $($lin.Trim())（有意）" } else { Warn "Linux APPLICATION_ID: $lin" }

Step '5. pubspec / 依赖'
$ps = Get-Content -LiteralPath 'pubspec.yaml' -Raw -Encoding utf8
foreach ($d in @('material_ui', 'cupertino_ui', 'os_type', 'file_picker_ohos', 'clipboard', 'hashlib',
                 'markdown', 'flutter_markdown_plus', 'flutter_markdown_plus_latex',
                 'audio_service_mpris', 'audio_service_win')) {
  if ($ps -match "(?m)^\s+$([regex]::Escape($d)):") { Ok "pubspec 含 $d" } else { Bad "pubspec 缺少 $d" }
}
foreach ($d in @('jnigen', 'desktop_webview_window')) {
  if ($ps -match "(?m)^\s+$([regex]::Escape($d)):") { Warn "pubspec 仍含 $d（应为已移除）" } else { Ok "pubspec 已移除 $d" }
}
if ($ps -match 'sdk:\s*">=3\.12\.2"') { Ok 'sdk >=3.12.2' } else { Warn 'sdk 约束未按预期放宽' }
if ($ps -match 'flutter:\s*">=3\.44\.9') { Ok 'flutter >=3.44.9' } else { Warn 'flutter 约束未按预期放宽' }
if (Test-Path -LiteralPath 'pubspec.lock') { Ok 'pubspec.lock 存在' } else { Bad 'pubspec.lock 缺失（需 flutter pub get）' }
if (Test-Path -LiteralPath '.fvmrc') {
  $fvm = Get-Content -LiteralPath '.fvmrc' -Raw -Encoding utf8
  if ($fvm -match '3\.44\.9') { Ok '.fvmrc = 3.44.9' } else { Bad ".fvmrc 不是 3.44.9: $fvm" }
}

Step '6. 通道协议 (dart vs ets)'
$dart = Get-Content -LiteralPath 'lib/harmony_adapt/harmony_channel.dart' -Raw -Encoding utf8
$dnames = [regex]::Matches($dart, "_channel\.invokeMethod(?:<[^>]*>)?\(\s*'([^']+)'|_invoke\(\s*'([^']+)'") |
  ForEach-Object { if ($_.Groups[1].Success) { $_.Groups[1].Value } else { $_.Groups[2].Value } } | Sort-Object -Unique
$ets = Get-Content -LiteralPath 'ohos/entry/src/main/ets/plugins/HarmonyChannel.ets' -Raw -Encoding utf8
$enames = [regex]::Matches($ets, "case\s+[`"']([^`"']+)[`"']") | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
$missing = @($dnames | Where-Object { $enames -notcontains $_ -and $_ -ne 'csy' })
if ($missing.Count -eq 0) { Ok "Dart 调用的 $($dnames.Count) 个方法名都有原生 handler（csy 死代码除外）" }
else { Bad "以下 Dart 方法原生无 handler: $($missing -join ', ')" }
$dch = [regex]::Match($dart, "MethodChannel\('([^']+)'").Groups[1].Value
$ech = [regex]::Match($ets, 'new MethodChannel\(binding\.getBinaryMessenger\(\),\s*"([^"]+)"').Groups[1].Value
if ($dch -eq $ech -and $dch -ne '') { Ok "通道名一致: $dch" } else { Bad "通道名不一致: dart='$dch' ets='$ech'" }

Step '7. 插件注册表 vs 依赖'
$reg = Get-Content -LiteralPath 'ohos/entry/src/main/ets/plugins/GeneratedPluginRegistrant.ets' -Raw -Encoding utf8
# 每个插件形如  flutterEngine.getPlugins()?.add(new XxxPlugin());
$adds = @([regex]::Matches($reg, 'getPlugins\(\)\?\.add\(new\s+(\w+)\(') | ForEach-Object { $_.Groups[1].Value })
$imports = @([regex]::Matches($reg, "(?m)^import\s+(\w+)\s+from\s+'([^']+)'") | ForEach-Object { $_.Groups[1].Value })
Ok "GeneratedPluginRegistrant 注册 $($adds.Count) 个插件 / $($imports.Count) 个 import"
if ($adds.Count -ge 20) { Ok "插件数量合理（>=20）" } else { Bad "插件注册数量异常: $($adds.Count)" }
$unimported = @($adds | Where-Object { $imports -notcontains $_ })
if ($unimported.Count -eq 0) { Ok '所有注册的插件类都有 import' } else { Bad "注册但未 import: $($unimported -join ', ')" }
# 反向：import 了但没注册（排除工具类）
$allAdds = @([regex]::Matches($reg, 'add\(new\s+(\w+)\(') | ForEach-Object { $_.Groups[1].Value })
$unused = @($imports | Where-Object { $_ -notin @('FlutterEngine', 'Log') -and $allAdds -notcontains $_ })
if ($unused.Count -eq 0) { Ok '所有插件 import 都被注册' } else { Warn "import 但未注册: $($unused -join ', ')" }
foreach ($p in @('material_ui', 'cupertino_ui')) {
  $shim = Get-Content -LiteralPath "packages/$p/lib/$p.dart" -Raw -Encoding utf8
  $exp = [regex]::Match($shim, "export 'package:flutter/[a-z]+\.dart'[^;]*;").Value
  if ($exp -match "export 'package:flutter/") { Ok "$p shim -> $exp" } else { Bad "$p shim 内容异常" }
}

Step '8. 静态分析（耗时较长）'
if ($args -contains '-SkipAnalyze') { Warn '已跳过 dart analyze' }
else {
  $out = & flutter analyze --no-pub 2>&1 | Out-String
  $errs = @([regex]::Matches($out, '(?m)^\s*error\s+'))
  if ($errs.Count -eq 0) { Ok 'flutter analyze 无 error' }
  else {
    Bad "flutter analyze 有 $($errs.Count) 个 error"
    ($out -split "`n") | Where-Object { $_ -match '^\s*error\s+' } | Select-Object -First 40 | ForEach-Object { "         $($_.Trim())" }
  }
}

Write-Host ""
if ($fail -eq 0) { Write-Host "全部检查通过 ✅" -ForegroundColor Green }
else { Write-Host "$fail 项检查未通过 ❌" -ForegroundColor Red }
exit $fail
