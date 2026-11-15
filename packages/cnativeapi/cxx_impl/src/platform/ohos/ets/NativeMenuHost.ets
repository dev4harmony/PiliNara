import { ComponentContent, UIContext } from '@kit.ArkUI';

export interface NativeMenuEntry {
  id: number;
  type: number;
  label: string;
  enabled: boolean;
  state: number;
  icon: string;
  tooltip: string;
  accelerator: string;
  key: string;
  modifiers: number;
  submenu?: NativeMenuModel;
}
export interface NativeMenuModel { id: number; items: NativeMenuEntry[]; }
export interface NativeMenuAnchor { x: number; y: number; width: number; height: number; }
@Observed
export class NativeMenuAnchorState {
  x: number = 0;
  y: number = 0;
  width: number = 1;
  height: number = 1;
}
@Component
export struct NativeMenuAnchorView {
  @ObjectLink state: NativeMenuAnchorState;
  anchorId: string = '';
  build() {
    Row().width(this.state.width).height(this.state.height)
      .position({ x: this.state.x, y: this.state.y }).id(this.anchorId)
  }
}
type Notify = (event: string, id: number) => void;
class RenderParams {
  menu: NativeMenuModel;
  notify: Notify;
  parent: number;
  constructor(menu: NativeMenuModel, notify: Notify, parent: number = 0) {
    this.menu = menu; this.notify = notify; this.parent = parent;
  }
}
function keys(bits: number): ModifierKey[] {
  let result: ModifierKey[] = [];
  if (bits & 1) { result.push(ModifierKey.SHIFT); }
  if (bits & 2) { result.push(ModifierKey.CTRL); }
  if (bits & 4) { result.push(ModifierKey.ALT); }
  return result;
}
function shortcutKey(key: string): string | FunctionKey {
  const names: string[] = ['ESC', 'ESCAPE', 'F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7',
    'F8', 'F9', 'F10', 'F11', 'F12', 'TAB', 'UP', 'DOWN', 'LEFT', 'RIGHT'];
  const codes: FunctionKey[] = [FunctionKey.ESC, FunctionKey.ESC, FunctionKey.F1, FunctionKey.F2,
    FunctionKey.F3, FunctionKey.F4, FunctionKey.F5, FunctionKey.F6, FunctionKey.F7,
    FunctionKey.F8, FunctionKey.F9, FunctionKey.F10, FunctionKey.F11, FunctionKey.F12,
    FunctionKey.TAB, FunctionKey.DPAD_UP, FunctionKey.DPAD_DOWN, FunctionKey.DPAD_LEFT, FunctionKey.DPAD_RIGHT];
  const index = names.indexOf(key.toUpperCase());
  return index >= 0 ? codes[index] : key.length === 1 ? key : '';
}
@Builder
function MenuSeparator() { Divider().margin({ top: 4, bottom: 4 }) }
@Component
struct MenuTreeComponent {
  @Prop menu: NativeMenuModel = { id: 0, items: [] };
  notify: Notify = () => {};
  parentItem: number = 0;
  @Builder
  submenu(entry: NativeMenuEntry) {
    MenuTreeComponent({ menu: entry.submenu!, notify: this.notify, parentItem: entry.id })
  }
  build() {
    Menu() {
      ForEach(this.menu.items, (entry: NativeMenuEntry) => {
        if (entry.type === 3) {
          MenuItem(MenuSeparator).height(9).enabled(false).focusable(false)
        } else if (entry.submenu) {
          MenuItem({ content: entry.label, startIcon: entry.icon, labelInfo: entry.accelerator,
            builder: this.submenu.bind(this, entry) })
            .enabled(entry.enabled).bindTips(entry.tooltip)
        } else {
          MenuItem({ content: entry.label, startIcon: entry.icon,
            labelInfo: entry.state === 2 ? '— ' + entry.accelerator : entry.accelerator })
            .enabled(entry.enabled).selected(entry.state === 1)
            .selectIcon(entry.type === 1 || entry.type === 2).bindTips(entry.tooltip)
            .keyboardShortcut(shortcutKey(entry.key), keys(entry.modifiers), () => {
              if (entry.enabled && entry.key.length > 0) { this.notify('shortcut', entry.id); }
            })
            .onClick(() => { if (entry.enabled) { this.notify('click', entry.id); } })
        }
      }, (entry: NativeMenuEntry) => JSON.stringify(entry))
    }
    .onAppear(() => { this.notify(this.parentItem ? 'submenu-open' : 'open', this.parentItem || this.menu.id); })
    .onDisAppear(() => { this.notify(this.parentItem ? 'submenu-close' : 'close', this.parentItem || this.menu.id); })
  }
}
@Builder
function MenuTree(params: RenderParams) {
  MenuTreeComponent({ menu: params.menu, notify: params.notify, parentItem: params.parent })
}
class Presentation {
  content: ComponentContent<RenderParams>;
  ready: Promise<void>;
  notify: Notify;
  submitted: boolean = false;
  cancelled: boolean = false;
  constructor(content: ComponentContent<RenderParams>, ready: Promise<void>, notify: Notify) {
    this.content = content; this.ready = ready; this.notify = notify;
  }
}
// Mount NativeMenuAnchorView in the page's root Stack. This class positions
// that observed anchor and uses the SDK's actual menu presenter and lifecycle.
export class NativeMenuHost {
  private context: UIContext;
  private anchorId: string;
  private anchor: NativeMenuAnchorState;
  private presentations: Map<number, Presentation> = new Map();
  private disposed: boolean = false;
  constructor(context: UIContext, anchorId: string, anchor: NativeMenuAnchorState) {
    this.context = context; this.anchorId = anchorId; this.anchor = anchor;
  }
  show(model: NativeMenuModel, anchor: NativeMenuAnchor, placement: number, notify: Notify): boolean {
    const node = this.context.getFrameNodeById(this.anchorId);
    if (this.disposed || !node || this.presentations.has(model.id)) { return false; }
    const origin = node.getPositionToScreen();
    const local = node.getPositionToParent();
    const x = anchor.x - Number(origin.x) + Number(local.x);
    const y = anchor.y - Number(origin.y) + Number(local.y);
    this.anchor.x = x; this.anchor.y = y;
    this.anchor.width = Math.max(1, anchor.width); this.anchor.height = Math.max(1, anchor.height);
    const forward: Notify = (event: string, id: number) => {
      if (event === 'close' && id === model.id) { this.release(model.id, content); }
      notify(event, id);
    };
    const content = new ComponentContent(this.context, wrapBuilder(MenuTree), new RenderParams(model, forward),
      { nestingBuilderSupported: true });
    const placements: Placement[] = [Placement.Top, Placement.TopLeft, Placement.TopRight,
      Placement.Right, Placement.RightTop, Placement.RightBottom, Placement.Bottom,
      Placement.BottomLeft, Placement.BottomRight, Placement.Left, Placement.LeftTop, Placement.LeftBottom];
    // The observed anchor must reach native layout before menu presentation.
    const ready = new Promise<void>((resolve, reject) => {
      const present = (attempt: number): void => {
        const session = this.presentations.get(model.id);
        if (!session || session.cancelled || session.content !== content) { reject(new Error('Menu presentation cancelled')); return; }
        const actual = node.getPositionToScreen();
        const size = node.getMeasuredSize();
        const matches = Math.abs(Number(actual.x) - anchor.x) < 1 &&
          Math.abs(Number(actual.y) - anchor.y) < 1 &&
          Math.abs(this.context.px2vp(size.width) - Math.max(1, anchor.width)) < 1 &&
          Math.abs(this.context.px2vp(size.height) - Math.max(1, anchor.height)) < 1;
        if (!matches) {
          if (attempt < 90) { setTimeout(() => { present(attempt + 1); }, 16); return; }
          notify('failed', model.id); this.release(model.id, content); reject(new Error('Menu anchor layout failed')); return;
        }
        console.info(`NATIVEAPI_MENU_ANCHOR requested=${JSON.stringify(anchor)} actual=${JSON.stringify(actual)}`);
        session.submitted = true;
        this.context.getPromptAction().openMenu(content, { id: this.anchorId }, {
          placement: placements[placement],
          onDidDisappear: () => { this.release(model.id, content); }
        }).then(resolve).catch((error: Error) => { notify('failed', model.id); this.release(model.id, content); reject(error); });
      };
      setTimeout(() => { present(0); }, 0);
    });
    // Keep a handled rejection even when nobody subsequently closes the menu.
    ready.catch(() => {});
    this.presentations.set(model.id, new Presentation(content, ready, forward));
    return true;
  }
  update(model: NativeMenuModel, notify: Notify): boolean {
    const session = this.presentations.get(model.id);
    if (!session) { return false; }
    session.content.update(new RenderParams(model, session.notify));
    return true;
  }
  close(id: number): boolean {
    const session = this.presentations.get(id);
    if (!session) { return false; }
    if (!session.submitted) {
      session.cancelled = true;
      session.notify('failed', id); this.release(id, session.content); return true;
    }
    session.ready.then(() => {
      this.context.getPromptAction().closeMenu(session.content).catch(() => { session.notify('close-failed', id); });
    }).catch(() => { this.release(id, session.content); });
    return true;
  }
  dispose(): void {
    if (this.disposed) { return; }
    this.disposed = true;
    this.presentations.forEach((_session: Presentation, id: number) => { this.close(id); });
  }
  private release(id: number, content: ComponentContent<RenderParams>): void {
    const session = this.presentations.get(id);
    if (!session || session.content !== content) { return; }
    this.presentations.delete(id);
    // SDK callbacks complete before disposal of the corresponding ComponentContent.
    setTimeout(() => { session.content.dispose(); }, 0);
  }
}
