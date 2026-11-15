import { window } from '@kit.ArkUI';

// Companion to ohos_local_shortcuts.h. Mount one host per native window and
// forward its root component's normal onKeyPreIme callback. Do not forward
// UIContext.addLocalInputEventMonitor wrappers: their modifiers are unavailable.
export interface NativeShortcutBridge {
  shortcutHostAttach(target: window.Window): number;
  shortcutHostDetach(token: number): void;
  shortcutKey(token: number, code: number, type: number, modifiers: number, device: number): boolean;
}
export class NativeShortcutHost {
  private bridge: NativeShortcutBridge;
  private token: number;
  constructor(target: window.Window, bridge: NativeShortcutBridge) {
    this.bridge = bridge;
    this.token = bridge.shortcutHostAttach(target);
    if (!this.token) { throw new Error('NativeAPI shortcut host attachment failed'); }
  }
  handle(event: KeyEvent): boolean {
    if (!this.token || !event.getModifierKeyState) { return false; }
    const modifiers = (event.getModifierKeyState(['Shift']) ? 1 : 0) |
      (event.getModifierKeyState(['Ctrl']) ? 2 : 0) |
      (event.getModifierKeyState(['Alt']) ? 4 : 0) | (event.metaKey ? 8 : 0);
    return this.bridge.shortcutKey(this.token, event.keyCode, event.type, modifiers, event.deviceId);
  }
  dispose(): void {
    if (!this.token) { return; }
    this.bridge.shortcutHostDetach(this.token);
    this.token = 0;
  }
}
