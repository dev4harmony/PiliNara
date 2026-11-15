# OpenHarmony / HarmonyOS backend

Build this backend with the public OpenHarmony/HarmonyOS NDK at API 23 or later.
Use the SDK's `build/cmake/ohos.toolchain.cmake`, `OHOS_ARCH=x86_64` for the Windows
emulator, or `OHOS_ARCH=arm64-v8a` for Apple Silicon emulators and devices. Link the
`nativeapi` CMake target to inherit its required NDK libraries and C++17/PIC settings.

Implemented services (the full API coverage audit remains open):

| Service | Behavior |
| --- | --- |
| Application | Owning-Ability lifecycle events and actual running/foreground state, cancellable quit with real terminateSelf completion, live primary/window queries, application multi-instance metadata and application light/dark/system resources. Desktop SDK whole-Ability Show/Hide and mission-window icons; numeric system badges on phone/desktop. Explicit lifecycle forwarding; no nested Ability loop. |
| AppInfo | Current bundle identifier/version/build number and localized label through the ArkTS UI environment; workers marshal to UI. |
| Image | Native Image NDK decode/PNG base64 encode and file encode/decode; native PixelMap ownership; PNG/JPEG/WebP verified. |
| SecureStorage | Public ASSET encrypted values, isolated namespace enumeration/deletion and process persistence; no plaintext fallback. |
| AccessibilityManager | Live public SDK accessibility-extension state on UI/worker threads; Enable is a no-op because extension activation requires a system-only API. No local success flag. |
| DeviceInfo | Live public Settings device name on the attached UI host (NDK market fallback without context/on refusal), public NDK model/manufacturer/distribution OS/version, uname kernel and actual compiler/process ABI. Missing OS names stay empty. No unique identifiers. |
| Display / DisplayManager | Enumerates live displays, caches identity, reads logical geometry/density, work area, orientation and refresh rate. Native tokens encode the system ID plus one and must not be dereferenced/freed. |
| Preferences | App-sandbox NDK Preferences; scope names are encoded into safe filenames. Same-scope instances share one synchronized store until final close. Mutations flush synchronously; removing an absent key returns false. |
| Clipboard | Text/HTML/images/local files, combined formats, clear and local notifications through Pasteboard/UDMF. Reads obey the system permission policy. |
| Window / WindowManager | Attached WindowStage main/child windows, async child creation/content loading, live state/geometry, focus/input policy, actual SDK occlusion observations/events and cancellable child closing. Point queries use the public SDK visible-window stack per attached display: fresh on workers, nonblocking completed snapshots on UI. |
| FileDialog | Public system document picker for files/multiple files/folders/save, worker wait, exact authorized document URIs and native user cancellation; no public programmatic Close. |
| UrlOpener | Public bundle-manager handler queries and UIAbilityContext HTTP/HTTPS dispatch; worker Open waits for the real SDK callback and preserves failure diagnostics. |
| NotificationManager | Public notificationManager authorization/publish/cancel and immutable WantAgent activation; worker operations wait for SDK completion. Host forwards original Ability Wants through the notification bridge. |
| MessageDialog | Native ArkUI buttons/input/checkbox/progress and live updates, native close completion, worker modal waits, parent destruction and disposal cleanup. |
| Menu / MenuItem | Native ArkUI Menu/MenuItem through the companion ArkTS host; nested menus, dynamic properties, checked/radio states, native lifecycle and C ABI. Cursor query and keyboard/tooltip runtime audits remain open. |
| View / Label / Button / TextField / ImageView | Native ArkUI nodes in a host ContentSlot; shared layout/tree ownership, native focus/click events, text input modes and retained PixelMap images. Hover tooltips use the optional companion NativeTooltipHost and real SDK popups. |
| TrayIcon / TrayManager | Public HMS status-bar registration, real icon/menu/tooltip updates and left-click dispatch on supported desktops; stable shared identities and actual SDK completion. Phone service unavailable. |
| Shortcut / ShortcutManager | Public NDK global hotkeys and explicit companion-host application-local input, with guarded UI callbacks and no global reservation for local scope. Release/full keyboard-monitoring audits remain open. |
| KeyboardMonitor | Public NDK key press/release observers shared across C++/C instances, UI delivery, stopped-epoch and lifetime guards. Tested normal ACL apps remain service-denied; SDK system-app debug profiles deliver on phone/computer. Modifier-state and remaining runtime audits stay open. |
| LaunchAtLogin | Public SDK self-status reads on workers, nonblocking completed UI snapshots, real bundle metadata and local display-name configuration. Programmatic registration/controls remain unavailable. |
| Dispatcher | N-API thread-safe function targeting the host's ArkTS UI thread, including environment cleanup. |

For Clipboard and dispatcher, initialize the platform-internal host seam from the
UIAbility's N-API module initializer **on the ArkTS UI thread**, before other calls:

```cpp
#include "platform/ohos/ohos_runtime.h"

napi_value Init(napi_env env, napi_value exports) {
  if (!nativeapi::ohos::Initialize(env)) {
    // Report host initialization failure to the embedding application.
    return nullptr;
  }
  return exports;
}
```

Initialize one UI environment per linked nativeapi instance. Workers may post to
that environment; they must not initialize it themselves. The host owns the event
loop; `RunMainThreadLoopFor` does not run a nested ArkTS loop. Before initialization,
clipboard capability checks/writes return false and reads immediately fail.

App metadata/resource operations also use that initialized environment. Attach the
owning UIAbilityContext on its UI thread by exposing a N-API method that calls
`ohos::AttachContext(env, context)` and invoking it from ArkTS `onCreate`:

```typescript
onCreate(): void {
  nativeModule.attachContext(this.context);
}
```

The reference is released in environment cleanup. `GetName()` resolves the real
packaged label resource; other AppInfo getters query current bundle metadata.
Context-dependent services must use the same linked core instance as this host.

Accessibility state queries also use the initialized environment, without requiring
an AbilityContext or additional application permissions. `IsEnabled()` calls the
public `accessibility.isOpenAccessibilitySync()` on each query; workers marshal to
the UI environment. It means an enabled accessibility extension exists, rather than
screen-reader/touch-guide state or this application's authorization. Missing host
initialization or an SDK error returns false. `Enable()` is deliberately a no-op:
`accessibility.config.enableAbility` is a **system API** requiring
`ohos.permission.WRITE_ACCESSIBILITY_CONFIG`, unavailable to ordinary applications.
The port does not call it, grant itself permissions or simulate enablement. Users
manage extension activation through the system's accessibility settings.

Image and SecureStorage operations use their public native SDKs. SecureStorage
uses app-private ASSET records available after the device's first unlock; values
are limited to 1023 bytes, and the namespaced scope/key alias to 256 bytes.
Scope/key metadata is unencrypted, values are encrypted by ASSET. App uninstall
removes the records; process restarts do not. Invalid requests never fall back to
plaintext storage. Images retain their native PixelMaps across copies; moved-from
wrappers are empty. Clipboard image reads materialize independent pixels.

Declare and obtain `ohos.permission.READ_PASTEBOARD` in the host when reading
requires it. Local file writes first validate sandbox readability and use the native
FileUri converter. The system Pasteboard controls URI grants; a foreign app's private
sandbox URI is never relabeled as a same-named path in this app. Unmappable/non-local
URIs are ignored on reads. Cross-application file authorization remains unverified
in the current integration suite. Remote clipboard notifications are not subscribed. Framebuffer bit depth and global cursor position
are not exposed by these public NDK services (0 / origin is returned). Display
position is queried when the API 26 symbol is present; older systems use origin.
Display hotplug/configuration events are not yet wired.

## Application lifecycle and appearance

After `Initialize(env)` and `AttachContext(env, context)`, forward the owning
UIAbility's **actual** callbacks on its ArkTS UI thread through
`platform/ohos/ohos_application.h`:

```cpp
nativeapi::ohos::HandleApplicationLifecycle(env, context,
    nativeapi::ohos::AbilityLifecycle::Create); // onCreate, after AttachContext
// Foreground -> onForeground; Background -> onBackground; Destroy -> onDestroy.
```

The reference fixture exports this bridge as `applicationLifecycle(context, name)`:

```typescript
onCreate(want: Want): void {
  native.attachContext(this.context);
  native.applicationLifecycle(this.context, 'create');
}
onForeground(): void { native.applicationLifecycle(this.context, 'foreground'); }
onBackground(): void { native.applicationLifecycle(this.context, 'background'); }
onDestroy(): void { native.applicationLifecycle(this.context, 'destroy'); }
```

Subscribe before onCreate to observe Started. One owning UIAbility is supported
per linked core/UI environment. Duplicate Create/foreground/background forwarding
is idempotent; foreign contexts and invalid transitions are rejected. Lifecycle
state changes before its event is delivered. Backgrounding does not end IsRunning;
IsVisible follows Ability foreground/background, distinct from keyboard focus.
Actual onDestroy releases the matching context, ends its state and clears its
primary selection. Environment cleanup invalidates outstanding quit decisions
without calling JavaScript or inventing a normal exit event.

`Quit()` uses the shared EventRequest confirmation on UI, including worker calls,
C++/C ABI cancellation, deferred votes and merged duplicate requests. Only complete
approval calls public UIAbilityContext.terminateSelf. The pending request is held
through SDK completion and real onDestroy; an SDK rejection releases it for retry.
Actual destruction emits Exiting with the latest approved code as event data, not
a process exit status. Unrequested native destruction emits a required request
that cannot be vetoed or deferred. No process-kill fallback is used. This does not
terminate other Abilities or claim process-wide shutdown. Run and Run(window)
return -1 because the Ability owns its event loop.

After environment/context attachment and content loading, SetBrightness uses the
public **ApplicationContext.setColorMode**, affecting application resources. It
supports Light, Dark and System and rejects invalid values; SDK exceptions return
false. Per-Ability color overrides have higher priority. No lifecycle forwarding
is required just to query application multi-instance/clone metadata, query/set live
registered windows, or change application appearance. Initial primary lookup finds
the attached stage main window; nullptr explicitly clears selection and invalid
native wrappers are ignored. Selecting a window does not change WindowStage
ownership. All window queries marshal workers to UI and preserve existing window
identities.

On status-bar-capable desktops, Application::Show/Hide call the public owning-
Ability showAbility/hideAbility SDK after actual status-bar registration. Successful
completion updates application visibility and activation events. The system hides
the main window and registered descendants together; per-window isWindowShowing
alone remains true, so Window::IsVisible also includes SDK application hiding.
Activating Window::Show restores that owning group. Native primary-window visibility
also reconciles external launcher restoration; a false visibility callback alone
is not treated as application hiding because occlusion can produce it. The running
Dock item is removed while hidden on the tested desktop image. Phone profiles
return false, matching direct SDK error 801.

SetIcon uses desktop setMissionWindowIcon with a square decoded image and an
independent cloned PixelMap; it changes the owning window/mission snapshot icon,
not the installed launcher asset. SetBadgeLabel uses public setBadgeNumber: decimal
counts through INT32_MAX, empty/0 to clear, and 99+ as count 100. Other strings fail.
Actual display follows notification/badge settings. These promise-based operations
report submission on the UI thread and SDK completion to worker callers; environment
cleanup releases pending workers. No notification is published to create a badge.
Dock visibility, progress/menu-bar integration, multiple owning Abilities and the
full SDK failure/teardown matrix remain open; unimplemented calls return false.

## WindowStage host integration

The host creates its main window through the normal UIAbility lifecycle. After
`Initialize(env)`, expose a small N-API method that receives the real WindowStage
and calls `nativeapi::ohos::AttachWindowStage(env, stage)`. Call that method from
ArkTS `onWindowStageCreate`, before loading the main page. For a window created by
another host component, use `AttachWindow(env, window, parent_id)` instead.
Repeated attachment returns the same registered wrapper and WindowId.

```cpp
#include "platform/ohos/ohos_window.h"

napi_value AttachStage(napi_env env, napi_callback_info info) {
  napi_value stage = nullptr;
  size_t count = 1;
  napi_get_cb_info(env, info, &count, &stage, nullptr, nullptr);
  napi_value result;
  napi_get_boolean(env, count == 1 && nativeapi::ohos::AttachWindowStage(env, stage), &result);
  return result;
}
```

```typescript
onWindowStageCreate(stage: window.WindowStage): void {
  nativeModule.attachWindowStage(stage);
  stage.loadContent('pages/Index');
}
```

`WindowManager::GetCurrent()`, `Get(id)` and `GetAll()` now return attached native
windows. Focus queries prefer the focused attached window, falling back to the
main window. Bounds/content bounds use the actual window/drawable rectangles in
logical display units. Geometry, visibility, focus, focusability, touchability,
privacy and immersive-layout getters query the SDK rather than guessed constants.
Operations that return native promises complete later; observe getters or native
events after returning to the UI loop.

A child window needs a real ArkUI content page. Creation and content loading are
asynchronous, so `Window()` does not create one synchronously on this platform:

```cpp
const bool submitted = nativeapi::ohos::CreateSubWindow(
    "MyChild", "pages/Child", [](std::shared_ptr<nativeapi::Window> window) {
      if (!window) return;  // Creation/content loading failed.
      window->SetBounds({40, 100, 280, 220});
      window->Show();
    });
```

This internal host seam intentionally stays outside the generated API surface;
registered windows are available through the existing C++/C/language manager APIs.
The host must use the same linked nativeapi instance for attachment and consumers.
A completion receives `nullptr` on failure, runs on the ArkTS UI thread and is
cancelled during environment teardown. The new child is initially hidden.

Attached windows are host-owned. `GetNativeObject()` returns a borrowed opaque
identity token that can be passed to `Window(void*)` to share the same identity.
It is neither a `napi_value` nor an `OHNativeWindow` rendering surface; do not
free it or dereference it. Native destruction removes registry entries, invalidates
all retained wrappers and cancels pending close decisions. Releasing a wrapper
alone does not destroy the host's window. API 19+ parent queries read the actual
SDK relationship and return attached canonical wrappers; older SDKs without the
getter use the creation relationship. `SetParentWindow` uses the public SDK for
attached child/parent windows in the same UI environment, subject to device and
freeform support. It rejects nullptr detachment, main-window receivers, cycles and
overlapping requests. UI returns submission status, workers wait for completion;
SDK rejection preserves the native relationship, including repeated same-parent
requests rejected by the tested service. Destruction/cleanup releases pending workers.

`Show`, `ShowInactive`, `Hide`, `Focus`, `Blur`, position/size/content geometry and
`Center` use real window operations. Hiding a child uses the SDK's `minimize`
operation; it does not add a taskbar entry. Focus can move only among visible
windows of the same application. Hit testing includes only attached process windows,
ordered by observed show/focus operations; it cannot detect other applications
covering those windows.

Programmatic `Close()` supports cancellable/deferred requests on child windows.
Ability-owned main windows reject it: their lifecycle stays with UIAbility. When
window observation first starts, the adapter subscribes to the public SDK's
`windowWillClose` event on attached main/child windows. Actual system title-button
clicks then share the same per-identity C++/C votes. Cancel/exception resolves the
SDK reply with `true`; approval resolves `false` to continue the exact original
SDK close, including normal Ability termination for a main window. No synthetic
destroy/terminate call substitutes for that native path. Pending programmatic
requests and repeated title-button clicks coalesce. With no close observers and
no pending request, the callback immediately permits ordinary SDK closing.
An installed close interceptor stays until native destruction so removing every
listener while a decision is held cannot bypass it on the next click. Native
destruction invalidates old votes and resolves a pending reply as prevented;
environment teardown drops it without executing JavaScript. Hosts must coordinate
direct subscriptions to the same SDK event; replacement/coexistence is not yet
verified. Phone profiles may expose no system title buttons; programmatic child
closing remains available.
Manager/object notifications include creation, native focus/blur, native rectangle
changes and destruction. Window-mode changes produce minimize/maximize/restore events where supported.
Full-screen events follow the live IsFullScreen property after native callbacks or
nativeapi layout/system-bar changes; immersive layout alone is a separate property. Geometry/status monitoring stops with the last observer; a small
lifecycle callback stays attached to invalidate window identities on destruction.

### Computer window modes

On a `2in1` main window, `Maximize()` requests the SDK's non-immersive maximize
presentation instead of its full-screen default. `SetFullScreen(true)` enters the
native full-screen window mode; exiting it restores the previous floating or
maximized mode. `IsFullScreen()` and its events use that actual desktop window mode,
so changing immersive content layout does not pretend to maximize a floating window.
The full-screen geometry was verified against the complete native display.

`Restore()` uses the desktop SDK's `restore()` operation for a minimized main
window. That operation requires a foreground UIAbility. A desktop-only manifest
(`deviceTypes: ["2in1"]`) keeps its Ability foreground when the window is minimized.
A manifest that also declares `phone` instead backgrounds the Ability; the host
must bring that Ability foreground through its normal lifecycle before restoring
its window. Calling the NDK show operation alone does not restore that mixed-device
Ability. See Huawei's [window lifecycle rules](https://developer.huawei.com/consumer/cn/doc/doccenter-capabilities/window-lifecycle).

Native title-bar dragging, maximize/restore controls, title/decorations, size limits,
and main-window geometry were verified in the MateBook Pro computer emulator.
`StartDragging()` uses public `startMoving()` from an active touch gesture;
`StartResizing()` remains unsupported.

`SetFullScreen()` uses immersive SDK maximize/recover for freeform children as
well as the desktop main window. A child must support the requested native mode
(for example, an eligible `maximizeSupported: true` child); ordinary children can
be rejected by the SDK. Leaving fullscreen restores the preceding maximized
state or the SDK's original floating rectangle. `IsFullScreen()` reads actual
native mode in a freeform workspace, and confirmed changes reach object,
manager and C fullscreen events. Calls are asynchronous. Phone/non-freeform
content-layout and system-bar behavior remains subject to SDK restrictions.
On the phone, a child's SDK fullscreen property can become true when its content
layout extends to the edges, with its floating frame unchanged. Queries/events
retain that SDK property semantics outside a freeform workspace; this does not
claim a native screen-sized child window.

The separate `WindowDragSession` global pointer-tracking backend remains
unavailable. Public mouse monitoring requires `ohos.permission.INPUT_MONITORING`
and is limited to a screen-recording scenario; ordinary test apps are denied.
Local component mouse events cannot establish the session's across-window
button-release contract. The existing `StartDragging()` native gesture is a
different operation and remains available where the SDK supports it.

`SetFullScreenable()` now changes the public supported-mode mask. On API 26+
it uses the Window method; an absent primary-window method falls back to the
API 15+ WindowStage method after checking the Stage's actual main-window ID.
Fullscreen and nonimmersive maximize share the SDK FULL_SCREEN capability, so
disabling this mode prevents both. This is separate from caption-button policy
and does not control phone content-layout/system-bar flags.

Requests preserve the other known mode bits. The primary baseline comes from
the attached UIAbilityContext's `abilityInfo.supportWindowModes`. A floating
child establishes FLOATING without inventing SPLIT support. An already-fullscreen
child with an unknown mask cannot establish its other modes. Unknown child
policy getters return false until a host-published mask or successful request
establishes the policy; an unknown result is not proof of native incapability.
Empty/invalid resulting masks are rejected, so disabling a known FULL_SCREEN-only
window does not silently enable a different mode. SDK refusal leaves the old
policy intact. UI requests are ordered asynchronously; workers wait for their
own completion. Object/manager/C property events report actual committed changes,
and native destruction/environment cleanup cancels queued requests.

The SDK has no public live mask getter, and AbilityInfo remains declarative after
SDK mutations. A host using higher-priority StartOptions or direct SDK overrides
must publish its known mask through `SetWindowModeBaseline` from the private
`ohos_window_modes.h` seam after attaching the window. This function calls no SDK
setter; publish only values actually applied by the host. It validates enum
values (FULL_SCREEN=0, SPLIT=1, FLOATING=2), rejects duplicates and invalid window
combinations, and refuses to overwrite pending requests. For example, after a
successful SDK override to FULL_SCREEN + FLOATING:

```cpp
nativeapi::ohos::SetWindowModeBaseline(attached_window, {0, 2});
attached_window->SetFullScreenable(false);  // FLOATING stays enabled; SPLIT stays disabled.
```

The three independent `SetMinimizable`, `SetMaximizable` and `SetClosable`
policies apply to public SDK caption buttons on supported decorated windows.
They compose with collective control visibility and title-bar styles: hiding the
row or decorations preserves each choice, and showing it restores only enabled
buttons. Programmatic maximize/minimize remain available. Primary `Close()` keeps
its documented unsupported result; programmatic child closure is independent of
caption support. Unsupported phone and ordinary child SDK requests preserve the
previous state, with false getters until a successful button request.

These are **caption-only policies**. Hiding the maximize button does not stop a
real system title-bar double-click from maximizing. They do not restrict other
system gestures or menus. Complete user-maximize denial remains open; changing
supported window modes also blocks programmatic maximize and is not used as a
substitute. The SDK exposes no individual button-policy getter. Desktop primary
windows start with three enabled defaults at attachment; subsequent getters
report successful nativeapi policy applications, including when buttons are
collectively hidden. Direct host SDK button changes do not rewrite those policies.
The collective visibility getter continues to read the actual SDK button area.
Destroyed wrappers report false. Object, manager and C property events are
emitted only for effective policy transitions, on the owning UI thread.

`PerformTitleBarDoubleClick()` implements the action for a host's **custom**
title bar: explicitly forward the component's double-tap/double-click event to
this method. It uses public SDK maximize (EXIT_IMMERSIVE) from floating mode
and recover from maximized/split-screen modes. It rejects
fullscreen/minimized/unknown modes and an applied disabled maximize policy,
and preserves programmatic Maximize behavior. The live API 22+
`isInFreeWindowMode()` query must confirm support. This rejects phone children
whose SDK maximize promise fulfills without actually maximizing. SDKs lacking
that query use a 2in1-only fallback; a false/error result never falls back.
UI calls return whether the SDK
promise was submitted; workers wait for the real SDK result. A pending request
blocks another alias's duplicate action until completion. Native window removal
or environment cleanup invalidates pending work.

Maximizable SDK children are supported too. The host must create them with
`maximizeSupported: true` before attachment. The current computer SDK can
maximize/recover those children even though it rejects their individual caption
button setter with 1300004. Missing button-control support is not an applied
disable policy and does not prevent this custom action. SDK errors return false
on workers; a UI submission may still be rejected asynchronously. The method
does not alter the system's own native title-bar double-click behavior.

Device/window restrictions still apply to maximization, recovery, window limits,
decorations/title, shadows, movement/resizing policy and topmost status. Failed
operations do not update policy caches. Where the SDK has no getter, title, background
and policy values report only successful nativeapi changes, not external host
changes. `SetContentProtection` requires the normal `ohos.permission.PRIVACY_WINDOW`
authorization. Input forwarding, bottommost placement, whole-window opacity
and nativeapi-initiated edge resizing remain unsupported. Child polygon shapes
and custom shadows are described below.

Menus, tray, remaining native view/dialog cases, accessibility,
application lifecycle operations and global keyboard shortcuts remain under audit.
Their existing stubs are not evidence that the public SDK cannot support them.
The workspace `tools/tests/ohos_api_coverage.md` retains the full requested scope.

## Notifications

Initialize the OHOS runtime and attach the UIAbilityContext first. Subscribe to
`NotificationActivatedEvent` before calling `NotificationManager::Initialize()`.
Call `Initialize`, `Show` and `Remove` on workers while the ArkTS UI loop runs;
initialization requests the system's normal notification authorization when needed.
Denial and SDK failures return false with `GetLastError()` diagnostics. Capability
does not imply authorization or visible banners.

The host's N-API glue must forward the original Want from both `onCreate` and
`onNewWant` to `ohos::HandleNotificationWant(env, want)` in `ohos_notifications.h`.
The core builds explicit immutable WantAgents for the attached Ability, with
distinct body/button intents. Activation uses the normal main dispatcher and the
existing C ABI event bridge. The argument is `tag=<percent-encoded tag>&action=open`
or `action=button`; treat it as untrusted launch input. Cold Wants received before
Initialize are queued (up to 64) until the application has subscribed.

Tags are labels under a reserved nativeapi notification ID, so replacement and
removal work after process restart without a lossy tag hash or an in-memory ID map.
Tags contain 1-192 UTF-8 bytes without NUL. Titles/messages must be nonempty; SDK
display-length limits apply. Shutdown detaches activation delivery and registration
cleanup, leaving delivered notifications in place. Environment cleanup invalidates
registration and releases pending SDK sessions.

`NATIVEAPI_TEST_SUITE=notification` runs the independent notification group. Only
its isolated test HAP's authorization prompt and known notification tags are
operated. The default group remains separate because notification-center gestures
change foreground focus. See the smoke test README for invocation.

## URL dispatch

After attaching UIAbilityContext, `UrlOpener::CanOpen()` queries the public
`bundleManager.canOpenLink()` API. Declare `querySchemes: ["http", "https"]` in the
host module's manifest; a missing declaration/query failure returns false.
`Open()` does not require that query to pass: it independently requests the
registered handler using `UIAbilityContext.startAbility()` with the public
`ohos.want.action.viewData` action and the supplied URI. No browser bundle is
hard-coded in the implementation.

Call synchronous `Open()` from a worker and keep the ArkTS UI thread responsive.
It dispatches the actual SDK call on that UI thread and waits for its completion
callback. An ArkTS UI-thread call fails explicitly. Public SDK URL parsing rejects
malformed links; leading/trailing whitespace and scheme case are normalized.
System launch failures retain the SDK code/message in `UrlOpenResult`, and
environment cleanup wakes outstanding callers with a failure. Success acknowledges
system URL dispatch, not network page loading. Normal Ability startup and
foreground/permission rules apply; nativeapi does not bypass them.

See `tests/ohos_smoke/` for the runnable HAP and Windows/macOS test runners. These use
an isolated emulator, including test-only clipboard authorization; production hosts
must use the normal permission flow.

## Message dialogs

`MessageDialog` uses public Native ArkUI nodes and custom-dialog controllers, scoped
through the parent's real UIContext. A visible attached parent is required; an
unset parent uses `WindowManager::GetCurrent()`. Initialize the UI host first.
Non-modal `Open()` returns after native submission. Modal `Open()` must run on a
worker: it blocks until the native dismissal callback while ArkTS keeps handling
events. A modal call on the ArkTS UI thread returns false, since the host does not
provide a nested ArkTS loop. Buttons and parent are configured before opening;
input, checkbox, title/message and progress can update while displayed.

Application modality disables focus and pointer input on the other attached
nativeapi windows, including windows attached while open, and restores the recorded
policies at dismissal. Overlapping modal leases preserve the original policy until
the last owner releases it. Unattached windows from another nativeapi instance are
outside this boundary. Parent closure and object destruction dispose native nodes;
opaque callback tokens prevent late SDK callbacks from accessing freed C++ models.
The disposable phone and computer fixtures exercise these paths.

## System document picker

`FileDialog` starts the public `DocumentViewPicker` in the attached visible parent
window, using the owning UIAbilityContext. An unset parent resolves to the current
window. As with modal message dialogs, call blocking `Open()` from a worker and
keep ArkTS pumping events; an ArkTS UI-thread call fails explicitly. Configuration
stays immutable and results remain absent until Open returns. Public result/config
access is synchronized independently of the native UI wait.

`GetPaths()` returns the system's original document URIs, including authorization;
it does not remove their scheme or fabricate sandbox paths. Normal applications
use `@ohos.file.fs` to open those URIs and obtain descriptors. URI grants follow
system lifetimes. The fixture checks actual read/write bytes after saving and
reading those same documents after selecting them again. Save uses the SDK default
that pre-creates an empty document. Open filters combine all extensions into a
single SDK filter choice, preserving their union for mixed-type multiple selection.
A literal comma within an extension cannot be represented by this SDK filter
syntax; Open fails with an explicit error before displaying a picker.

`DocumentViewPicker` has no public close/cancel operation. `Close()` therefore
returns false and leaves the real native interaction pending; the user can cancel
in its own system UI. API 26 native callbacks use numeric status zero on success
and status -1 with an empty URI array for dismissal; both are adapted without
mistaking a native cancellation for an SDK error. Other SDK errors fail Open.

## Native views in an ArkTS page

After initializing the runtime and attaching the WindowStage, expose the internal
`ohos_views.h` bridge through the host's N-API module. The ArkTS page owns a
`NodeContent`, displays it in `ContentSlot(content)` inside a filling container,
and passes its `window.Window` plus that content to
`ohos::AttachViewContent(env, window, content)` on the UI thread. Call
`ohos::DetachViewContent(state)` before the slot disappears. Attachment preserves
an existing tree; window destruction and environment cleanup unmount the tree.
See `tests/ohos_smoke/entry/src/main/ets/pages/Views.ets` for a complete host.

`Window::GetContentView()` returns the attached root; a window with no slot
returns null. The root follows the slot's measured size. Views must be created
and used on the initialized ArkTS UI thread while an attached host UI context
exists. Native frames use logical vp coordinates; layout completion is asynchronous.
Attached positions are read through NodeUtils layout queries because the tested
runtime's NODE_POSITION attribute getter returned zero for positioned children.
Detached views retain the last successfully applied logical position.

Containers use native Stack nodes and the shared Absolute/Row/Column layout.
Labels/buttons use native text/button nodes. Text fields switch between native
TextInput and TextArea nodes, preserving View identity, text, style and tree
position; focus and selection reset. Programmatic SetText is silent. Read-only
fields veto native will-insert/will-delete events while retaining selection.
Secure input uses TextInput's password type with its reveal icon disabled;
secure and multiline modes cannot be combined, and conflicting requests are
ignored without changing the mode. ImageView retains both Image and its native
DrawableDescriptor and uses contain fitting.

For hover tooltips, construct `new NativeTooltipHost(uiContext)` from the canonical
`ets/NativeTooltipHost.ets` and pass it as the optional fourth argument to
`AttachViewContent(env, window, content, tooltipHost)`. The companion uses public
API 18+ openPopup/closePopup targeted to the native node's unique ID. Native
NODE_ON_HOVER events schedule display after 700 ms, independently of business
listeners. Popups are nonmodal, have no mask and do not acquire focus. Null/empty
text, pointer exit, hidden/disabled ancestors, removal, destruction, content
unmount or host replacement cancel the old presentation. A retained callback
validates the live node, window, ancestors and current hover before display and
while visible. Unmount disposes the companion; reattach with a fresh instance.
Repeated attachment of the same content/host is idempotent; a different host for
the same slot replaces the companion without replacing the native view tree.
Before supplying a companion, tooltip values are stored without display. Calls
require the owning ArkTS UI thread, just as other View operations do.

The view acceptance fixture tests native geometry and real guest clicks. IME,
submit/user-edit events, read-only editing denial, arbitrary
borrowed-node interop, secondary windows, theme/font scaling and environment
teardown still need further runtime coverage. API getters alone do not close
those audit items.

## Native menus

Copy `ets/NativeMenuHost.ets` into the host's ArkTS module (the smoke fixture's
Hvigor task copies this canonical file automatically). The page mounts
`NativeMenuAnchorView` inside its root Stack, sharing a `NativeMenuAnchorState`
with `new NativeMenuHost(uiContext, anchorId, anchorState)`. Pass the owning
`window.Window` and that host object to `ohos::AttachMenuHost(env, window, host)`
from its N-API bridge. Call `host.dispose()` when the page disappears. See
`tests/ohos_smoke/entry/src/main/ets/pages/Menus.ets` for complete integration.
No additional system permission is requested.

The backend presents actual ArkUI Menu/MenuItem components, with the SDK's
phone/computer popup policy. Computer menus use the SDK's default subwindow
presentation; phone menus use its in-window presentation. Absolute and relative
rectangle/window anchors use logical screen coordinates. An observed anchor is
laid out and measured before presentation, rather than relying on a fixed delay.
ArkTS FrameNode positions are already vp; its measured size is px. Placement
uses all 12 native SDK placements and may flip/clamp at bounds. The current
fixture verifies BottomStart with absolute and relative rectangle/offset anchors;
other placement combinations remain under audit. Cursor-position Open currently
returns false; a correct public current-pointer query still needs investigation.

Use Menu/MenuItem on the initialized ArkTS UI thread. Open/Close submit
asynchronous presentation/closure and report immediate validation/submission
failure. Opened/Closed events originate from native appearance/disappearance.
Closing an expanded submenu closes its popup hierarchy, as required by the SDK's
root ComponentContent controller. Pending presentation can be cancelled before
appearance without fabricated lifecycle events. A session retains its original
host through native closure, even if the page installs a replacement bridge.
Environment cleanup drops delivery and references; confirmed window destruction
closes its sessions. Disposal invalidates callback tokens and emitter guards.
Opaque native object tokens identify this backend's registered models, not raw
ArkUI nodes or arbitrary ArkTS menu objects. Wrapping a foreign token fails safely.

Collection and property changes update the real component content. Radio groups
are mutually exclusive across registered items; checkbox/radio state changes are
application-controlled. Selected state, disabled state and the mixed-state dash
are observable in the actual system UI tree. Images use encoded PNG sources.
Tooltips use public bindTips. Single-character and public FunctionKey accelerators
use keyboardShortcut while the menu exists; Ctrl/Shift/Alt are mapped. Other
modifier combinations remain displayed without binding, rather than silently
binding a different combination. Shortcut activation closes the popup hierarchy.
Actual keyboard and tooltip interactions, remaining modifier/key mappings,
repeated root presentation/bridge replacement, secondary windows, placement
matrices, long content, SDK failure branches and environment teardown still need
runtime audit. See the workspace coverage ledger; this is not full API completion.

## HarmonyOS desktop status items

TrayIcon uses the **HMS** public `@hms.pcService.statusBarManager` module (the
`@kit.DeskTopExtensionKit` kit), which is separate from the OpenHarmony SDK.
No mock widget, hidden system API or status-bar extension Ability is required:
an empty QuickOperation.abilityName requests the actual status-icon click callback.
The normal `Initialize` / owning `AttachContext` integration above is required.
API 26 ARM64 phone and computer profiles are the runtime verification targets;
earlier HMS versions and plain OpenHarmony distributions remain unverified.

`TrayManager::IsSupported()` checks the public system capability and module with
the attached context; it does not promise registration will succeed. A desktop
application may have only **one visible system item**. Multiple logical TrayIcons,
opaque native-token aliases, stable IDs and manager lookup/enumeration are supported.
Get/GetAll concern this library's live identities (including hidden ones), rather
than other applications' status items. The last wrapper unregisters its identity
and requests actual SDK removal. Callback guards prevent delivery into destroyed
wrappers. If removal is refused without a foreground window, the retained native
registration is retried on the next forwarded ApplicationActivated event; broader
background/teardown cases remain in the verification ledger.

Visibility, visible image/template and hover-text updates become observable only
after SDK success. SetVisible returns request-submission status, or false for a
pending operation, absent service, missing image, occupied application slot, invalid
menu or synchronous SDK rejection. Repeating the already completed state succeeds.
Void image/template/tooltip setters issued during a pending request coalesce to the
latest requested value, with a conservative one-second queued send interval to
avoid the SDK refresh limit; SDK failures retain the last applied property. `SetIcon(nullptr)`
removes a visible item. Template conversion makes separate white/black native RGBA
maps from the source alpha channel and leaves the original Image unchanged. Colored
icons also receive separate native clones for each theme, so SDK replacement/release
cannot invalidate a caller-owned Image.
Hover text follows the SDK's 1–128-character constraint. Clearing it restores the
application display name; the SDK has no empty-hover-text/reset operation.

Only the genuine `leftClick` callback emits TrayIconClickedEvent. SDK menu selections
emit MenuItemClickedEvent on the live menu item, never a fabricated tray right-click.
RightClicked context menus project to actual native status-bar menus: grouped
separators, icons, selected state, up to 20 top-level items and 20 children per
submenu, at most two levels. Menu edits refresh the native menu. Disabled entries,
mixed selection, accelerators, item tooltips, deeper nesting and submenu separators
have no corresponding SDK fields: an initial incompatible menu rejects registration;
an incompatible live update clears the SDK menu. The SDK may append its own Exit command, outside the application
menu projection. Stale callbacks recheck live
item membership and enabled ancestors before delivery. Full boundary/type/rate-limit
and background scenarios remain open.

The SDK owns status-item size/layout and exposes no bounds getter. GetBounds returns
an empty rectangle. Identifier/title/size/position/content-view properties retain
configuration without claiming custom inline text/views or layout changes. Clicked
and DoubleClicked menu triggers are rejected. The SDK exposes no programmatic menu
open/close and no menu lifecycle or tray right/double-click callback for this use;
OpenContextMenu/CloseContextMenu return false. The phone profile reports no status-bar
capability and refuses registration rather than drawing a replacement UI.

The independent `pages/TrayReference` page compares direct ArkTS SDK behavior.
`NATIVEAPI_TEST_SUITE=tray` runs the library fixture and guarded UI acceptance through
`run.sh` / `run.ps1`; the computer verifier needs Pillow in NATIVEAPI_TEST_PYTHON.
It restricts input to the named disposable guest, verifies native process/window
ownership, identifies the status item using the fixture's actual accessTokenId,
and checks actual system-tree removal and icon pixels. No host cursor or keyboard
input is used.

## Window occlusion

GetOcclusionState observes the public SDK occlusionStateChanged event (API 22),
falling back to windowVisibilityChange if registration fails. Both no occlusion
and partial occlusion map to Visible; full occlusion maps to Occluded. SDK rules
for shadows, transparent/animated windows and masks apply. Hidden or successfully
application-hidden windows are Occluded; closed/foreign wrappers and SDK query
failures are Unknown. A shown window remains Unknown until the first actual
observation; showing or focusing it does not invent an unobstructed state.
Observations stay active for attached windows even without public listeners, so
worker/UI queries remain current after unsubscribe. The private property dispatch
cache deduplicates aggregate changes and delivers WindowOcclusionChangedEvent to
the existing object, manager and C ABI observation paths.
IsOcclusionStateSupported checks initialized public Window.SessionManager support.

## Window corners and shadows

Child-window corner preferences use public setWindowCornerRadius: square is 0 vp,
RoundSmall is 4 vp and Round is 8 vp. The first request reads the existing radius
with getWindowCornerRadius; Default restores that original radius rather than
re-entering a changing system policy. Main windows reject these preferences.
The shared record changes only on actual SDK success. UI calls report submission;
workers wait for the SDK result. A concurrent pending preference request fails,
and actual native window destruction/environment cleanup releases pending workers.
Queries after destruction return Default. Direct SDK radius edits do not rewrite
the library's successful preference record.

Child shadow toggles use public setWindowShadowRadius (0 to disable, 18 logical
pixels to enable), avoiding the main-only API. Main shadows use
setWindowShadowEnabled and require the normal system_grant permission
`ohos.permission.SET_WINDOW_TRANSPARENT` in the host's requestPermissions on PCs
and tablets. The library does not request or shell-grant it. The fixture declares
it normally; missing-permission SDK rejection is retained as separate evidence.
The public SDK has no existing-shadow getter: HasShadow reports the last successful
library request, false until configured, and preserves it on SDK failure. Native
shadow color/offset remain system-controlled for that default path.

Custom shadows use the optional `NativeWindowShadowHost` Stage companion in
`ets/NativeWindowShadowHost.ets`. Register an application page whose `@Entry`
uses `{ useSharedStorage: true }`, put the exported `NativeWindowShadowSurface`
inside a full-size transparent `Stack`, and pass its page path to the companion:

```ts
const host = new NativeWindowShadowHost(stage, 'pages/NativeShadowSurface');
// The application's N-API bridge calls ohos::AttachWindowShadowHost(env, host).
nativeBridge.attachShadowHost(host);
```

The companion must belong to the attached Stage. Without it, SetCustomShadow
returns false. No public API signature or generated binding changes are needed.
Hidden-title-bar targets use a private transparent SDK child window. It is never
registered as a nativeapi Window, cannot focus or receive pointer input, and
preserves the target's content/frame geometry. Core builds a clipped mask,
three-pass blur and straight RGBA pixels on a real worker; the Stage companion
publishes those pixels at the actual target frame/density. Explicit shapes,
including fully off-window shapes, seed only their visible clipped area.

SetCustomShadow copies its input and GetCustomShadow returns independent copies.
SetHasShadow preserves the configuration and controls presentation. UI setters
report submission, workers wait for the real SDK promise. Overlapping configuration
requests fail; visibility changes during a pending request are serialized.
Geometry observation is retained independently of business event subscribers.
Both SDK rectangle and size notifications carry the new physical geometry;
geometry generations invalidate superseded images while the companion waits for
that particular SDK frame to be committed. A phone may return 801 from the caption
query even though its style setter succeeds. Explicitly requesting Hidden before
configuration supplies the known mode; an initially full-frame drawable area can
also establish it. A successful style setter/query updates the record. A resized
buffer retaining its old drawable dimensions is not mistaken for a new caption.
Native moves/resizes, visibility changes and public corner/shape changes refresh
the helper. Clearing removes it and restores the library's default native shadow
radius/visibility. Native destruction invalidates pending requests, hides the
helper and disposes it through the companion's SDK destruction observer.

On a supported computer, the same companion also renders primary-window custom
shadows after hiding the title bar and declaring SET_WINDOW_TRANSPARENT normally.
Restoring decorations suspends the custom helper without dropping its copied
configuration; hiding them again resumes it. Decorated configuration requests
fail while preserving the previous configuration. On the current phone profile,
the direct primary-shadow SDK rejects with 801, and primary custom requests are
refused without a helper or a changed configuration.

Both target and helper SDK destruction are observed. A destroyed helper is
removed from the companion's map and subscriptions; the next request recreates
it. A synchronous liveness check also covers a destroy promise settling before
its event. Delayed events from an old surface cannot remove its replacement.

The Stage must own the companion for its lifetime and dispose it on teardown.
SDK/renderer failure, replacing hosts in the same environment, multiple Stages,
primary rounded-contour compatibility and broader teardown/race scenarios remain under audit.
The existing normal SET_WINDOW_TRANSPARENT permission is needed when the public
SDK suppresses a main window's default shadow; no permission is shell-granted.


## Application-local shortcuts

`ShortcutOptions.scope = ShortcutScope::Application` uses normal ArkUI component
`onKeyPreIme` events, with the event-time Ctrl/Alt/Shift modifier snapshot. It never
reserves or probes a global hotkey. `IsAvailable(accelerator)` continues to mean
**global** availability; a different app owning that global chord does not prevent
local registration. Global handlers may consume the physical chord before it reaches
a local component, as with any normal UI input. Meta/Fn/lock modifiers and release
events remain unsupported. Bare nonmodifier keys are accepted in Application scope.

An ArkTS embedding host mounts one companion `ets/NativeShortcutHost.ets` instance
per SDK window. Expose `AttachLocalKeyHost`, `DetachLocalKeyHost` and
`DispatchLocalKey` from `ohos_local_shortcuts.h` through the embedding module's
N-API bridge (the smoke fixture supplies working exports). Construct the adapter
with the SDK Window and bridge; forward the root component's normal
`.onKeyPreIme((event: KeyEvent): boolean => host.handle(event))` and call
`host.dispose()` on page disappearance. Keep the app's normal focus policy;
the adapter neither steals focus nor replaces the content. Controls that consume
input before it reaches the root retain priority. No privileged input-monitoring
permission is needed. Without an attached key host, registration returns nullptr.
Additional windows/pages need their own host. Native-only hosts can forward the
equivalent real component event snapshot through the same private seam; that path
is not yet separately verified.

Do **not** feed API 26 `UIContext.addLocalInputEventMonitor` raw wrappers into this
adapter: the public SDK documents their `metaKey` and `getModifierKeyState` as
unavailable at that pre-dispatch stage. Tracking modifier down/up messages alone
also loses correctness when focus changes while keys are held. The normal component
callback used here provides the complete modifier snapshot for each key event.

Activation is queued to the UI thread and rechecks registration, enabled state,
host token, window lifetime and real SDK focus. Detaching/remounting a root gets a
new token, invalidating its pending deliveries. Blur clears held-key repeat state;
native window destruction and environment cleanup remove the corresponding hosts.
Callbacks and activation-event listeners may unregister their own shortcut. A
worker may register/unregister without waiting for UI while holding manager locks.
`run_local_shortcut_suite.py --allow-guest-keyboard` is a separate, explicitly
approved physical-input acceptance scenario; ordinary `run.sh` never sends keys.

## Launch-at-login status

The public API 21+ autoStartupManager module exposes current-bundle status only.
LaunchAtLogin.IsSupported/Enable/Disable return false because normal self-service
registration is unavailable; enterprise-admin APIs are not silently substituted.
IsEnabled still reads the real SDK self status for default or explicit self IDs.
Workers await the actual Promise; UI reads initiate refresh and return the last
completed value in the current environment, initially false. SDK failures return
false. Environment generations prevent reuse of old snapshots after teardown.
A fresh synchronous UI answer cannot be obtained from this asynchronous SDK API
without blocking its own event loop. Use a worker when freshness is required.

Default ID/label getters resolve actual AppInfo metadata even when the object was
constructed before initialization. Foreign/explicit empty IDs return false.
SetDisplayName records metadata only; system Settings uses the bundle label.
SetProgram refuses arbitrary executables/arguments, and their getters stay empty.
The API 26 computer scenario verifies real Settings-list add/remove transitions;
the phone SDK explicitly reports no auto-start capability in the tested image.
No actual boot-time launch or full environment replacement is claimed.

### Immersive content layout completion

`Window::SetContentUnderTitleBar` uses public `setWindowLayoutFullScreen` for
HarmonyOS immersive status/navigation-bar layout. UI callers report submission;
workers wait for the real promise and compare the actual NDK layout flag with
the request. A fulfilled SDK no-op is not reported as worker success. One request
per native window is active; overlapping C++/C aliases return false without
calling the SDK. SDK throw/rejection/malformed promise releases that guard.
Native destruction cancels the request and wakes a waiting worker; late outcomes
do not refresh fullscreen state or deliver events.

This preserves the existing OHOS mapping rather than claiming a transparent
system-caption overlay. On the API 26 computer, a floating main-window layout request leaves
the live layout flag unchanged, and child flags change without removing the decorated drawable inset.
The native frame and fullscreen/maximized mode remain independent. Query the
actual layout flag; a successful UI submission does not prove native geometry.
