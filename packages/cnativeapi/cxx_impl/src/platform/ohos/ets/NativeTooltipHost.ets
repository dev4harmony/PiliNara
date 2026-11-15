import { ComponentContent, UIContext } from '@kit.ArkUI';

class TooltipParams {
  text: string;
  constructor(text: string) { this.text = text; }
}
@Builder
function TooltipBody(params: TooltipParams) {
  Text(params.text).fontSize(14).fontColor('#FFFFFFFF').padding(8)
    .constraintSize({ maxWidth: 320 }).hitTestBehavior(HitTestMode.None)
}
class TooltipSession {
  key: number;
  node: number;
  text: string;
  valid: () => boolean;
  timer: number = -1;
  poll: number = -1;
  content?: ComponentContent<TooltipParams>;
  ready: Promise<void> = Promise.resolve();
  constructor(key: number, node: number, text: string, valid: () => boolean) {
    this.key = key; this.node = node; this.text = text; this.valid = valid;
  }
}
// One instance per attached ContentSlot/window. Uses the real native node's
// unique ID as the public popup target; no invisible overlay intercepts input.
export class NativeTooltipHost {
  private context: UIContext;
  private current?: TooltipSession;
  private closing: Promise<void> = Promise.resolve();
  private disposed: boolean = false;
  constructor(context: UIContext) { this.context = context; }
  private valid(session: TooltipSession): boolean {
    if (this.disposed || this.current !== session) return false;
    try {
      const node = this.context.getFrameNodeByUniqueId(session.node);
      return node !== null && node.isAttached() && node.isVisible() && session.valid();
    } catch (_) { return false; }
  }
  show(key: number, node: number, text: string, valid: () => boolean): boolean {
    if (this.disposed) return false;
    this.retire();
    if (text.length === 0) return true;
    const session = new TooltipSession(key, node, text, valid);
    this.current = session;
    session.timer = setTimeout(() => { this.present(session); }, 700);
    session.poll = setInterval(() => { if (!this.valid(session)) this.hide(key); }, 100);
    return true;
  }
  private async present(session: TooltipSession): Promise<void> {
    await this.closing;
    if (!this.valid(session)) { this.hide(session.key); return; }
    try {
      const content = new ComponentContent(this.context, wrapBuilder(TooltipBody), new TooltipParams(session.text));
      session.content = content;
      session.ready = this.context.getPromptAction().openPopup(content, { id: session.node }, {
        placement: Placement.Bottom, enableArrow: false, autoCancel: true,
        showInSubWindow: false, mask: false, focusable: false,
        popupColor: '#E6222222', backgroundBlurStyle: BlurStyle.NONE, radius: 6,
        onStateChange: (event: PopupStateChangeParam) => {
          if (!event.isVisible && this.current === session) this.hide(session.key);
        }
      });
      await session.ready;
      if (!this.valid(session)) this.hide(session.key);
    } catch (_) { this.hide(session.key); }
  }
  hide(key: number): void { if (this.current?.key === key) this.retire(); }
  private retire(): void {
    const session = this.current;
    this.current = undefined;
    if (!session) return;
    clearTimeout(session.timer); clearInterval(session.poll);
    const content = session.content;
    if (!content) return;
    const previous = this.closing;
    this.closing = previous.then(async () => {
      try { await session.ready; await this.context.getPromptAction().closePopup(content); } catch (_) {}
      try { content.dispose(); } catch (_) {}
    });
  }
  dispose(): void { this.disposed = true; this.retire(); }
}
