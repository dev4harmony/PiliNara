# Merge resolution policy v2 — ohos ← upstream PiliNara 2.1.5.1-Pearl

Working tree at `D:\workspace\pilinara-ohos\PiliNara` is mid-`git merge` of
`true-origin/main` (= `1807e22ed`, Starfallan/PiliNara at 2.1.5.1-Pearl) into
branch `ohos`.

- Merge base: `ce17223a8` (Release 2.1.4)
- OURS = current `ohos` branch — read with `git show HEAD:<path>` — this is
  **PiliNara product code + HarmonyOS platform layer + local fixes**
- THEIRS = upstream PiliNara 2.1.5.1 — read with `git show MERGE_HEAD:<path>`
  (equivalently `git show 1807e22ed:<path>`) — read-only reference copy also at
  `D:\workspace\PiliNara`
- BASE = `git show ce17223a8:<path>`

> NOTE: this direction is the MIRROR IMAGE of `RESOLUTION_POLICY.md` (v1).
> v1 synced the ohos layer into PiliNara; v2 syncs **upstream product code into
> the ohos branch**. Read this file, not v1.

## The one rule

**This round exists to bring upstream's new product features (综合搜索, AI 弹幕
人像防挡, 应用内画中画手动入口, AI 对话思考过程, 屏蔽动态视频, …) into the
HarmonyOS branch, without losing the HarmonyOS platform layer.**

- **THEIRS wins for product behavior**: upstream features, refactors, bug fixes,
  UI, settings, new files. Upstream structure is the target state; the user
  explicitly asked 写法尽量向上游靠拢.
- **OURS wins for HarmonyOS platform glue**: `OS.isHarmony` / `Platform.isOhos`
  branches, `package:PiliNara/harmony_adapt/*` / `media_kit_adapt/*` imports,
  `HarmonyChannel.*`, cutout / safe-area / decoration-bar / status-bar handling,
  ohos plugin call sites, ohos-specific MediaQuery/gesture fixes, `ohos/` tree,
  `packages/{material_ui,cupertino_ui}` shims, `.vscode` ohos tasks.
- **OURS wins for deliberate local decisions** (do not regress these):
  - branding/URLs pointing at `dev4harmony/PiliNara` (commit a7fadbb73)
  - `share_plus` OLD static API `Share.share/shareXFiles` (AGENT.md §6.6)
  - `file_picker` → `package:file_picker_ohos/file_picker_ohos.dart` imports
  - `playerStatus` is a plain field: NO `.value` in `Obx` — keep the
    `Builder` + `StreamBuilder<bool>` pattern in the two pip overlay services
    (AGENT.md §6.8)
  - media_kit adaptation facts (AGENT.md §6.1): `PlayerExtension.setProperty`,
    `player.state.playlist.medias`, `SubtitleViewConfiguration` 4-arg form,
    `SimpleVideo` from the `support-pilinara` branch of `media_kit_video`
    (no longer vendored in `lib/media_kit_adapt/`), `setShader` 1-arg
  - `lib/utils/android/bindings.g.dart` stays the 34-line no-jni stub
  - environment/dep pins: `sdk >=3.12.2`, `flutter >=3.44.9`, `.fvmrc 3.44.9`,
    gitcode ohos forks, `flutter_math_fork` override
  - local fixes from this session: topbar clip fix (`bc5b92693`,
    `lib/pages/home/view.dart`, `lib/pages/main/view.dart`), PiP auto-start
    bridge `OS.isHarmony` guard (`d033fc50e`), secondary subtitle wiring
    (`6e098b92c`)
- **Merge BOTH** when each side touched a different concern in the same region
  (the common case): adopt THEIRS' new structure and weave OURS' platform guard
  / local fix back inside it. When in doubt, merge both rather than picking one.

## Special instructions

- **PiP files** (`lib/services/pip_overlay_service.dart`,
  `lib/services/live_pip_overlay_service.dart`, `lib/plugin/pl_player/*`,
  video-page PiP entry points): upstream is the reference implementation the
  user wants aligned with. Take THEIRS' structure; re-apply OURS' ohos needs:
  `OS.isHarmony` platform gate for the auto-start bridge, §6.8 StreamBuilder,
  `Pref.enableInAppPipToSystemPip`.
- **DU files (deleted by us, modified by them)**: decide by whether merged code
  still needs the file. To restore upstream's version materialize it with
  `git show MERGE_HEAD:<path> | Set-Content <path>` (this is allowed; it is
  just file content). To keep it deleted, leave it absent and report. The
  orchestrator runs `git add` / `git rm`.
- **AA files (added by both)**: pick per the rules above; the file already has
  conflict markers — resolve them as normal content.

## Hard rules

1. Leave NO `<<<<<<<` / `=======` / `>>>>>>>` markers in your files.
2. Never drop an upstream feature and never drop HarmonyOS support to "make the
   conflict easier".
3. No reformatting / reordering of code outside the resolved hunks (it causes
   churn in the next sync).
4. NO state-changing git commands (`add`, `commit`, `rm`, `checkout`, `stash`,
   `merge --abort`, …). Read-only git (`show`, `log`, `diff`, `grep`, `status`)
   is fine. The orchestrator stages and commits.
5. Verify each file parses: `dart format --output=none <file>` (prints parse
   errors only, writes nothing). If `dart` is not on PATH, use
   `D:\workspace\flutter_flutter\bin\dart.bat`.
6. Other files in the tree are being resolved in parallel by other agents —
   do not edit files outside your assigned list. Do not run `flutter analyze`
   (orchestrator runs it once at the end).

## Done means

For each assigned file: content resolved, parses, no markers, and a short
report line: `path → chosen (ours|theirs|both) — why`.
