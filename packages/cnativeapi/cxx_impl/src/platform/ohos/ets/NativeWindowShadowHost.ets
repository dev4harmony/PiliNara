import { window } from '@kit.ArkUI';
import { image } from '@kit.ImageKit';

// Embed this component in a registered @Entry({useSharedStorage:true}) page.
// Each helper receives its own LocalStorage through the public loadContent API.
@Component
export struct NativeWindowShadowSurface {
  @LocalStorageLink('nativeapiShadowPixels') pixels?: image.PixelMap = undefined;
  build() { Stack() { Image(this.pixels).width('100%').height('100%').objectFit(ImageFit.Fill).interpolation(ImageInterpolation.None) }
    .width('100%').height('100%').backgroundColor('#00000000').hitTestBehavior(HitTestMode.None) }
}
class ShadowSurface {
  target: window.Window;
  helper: window.Window;
  storage: LocalStorage = new LocalStorage();
  destroyed: (event: window.WindowEventType) => void = () => {};
  helperDestroyed: (event: window.WindowEventType) => void = () => {};
  constructor(target: window.Window, helper: window.Window) { this.target=target; this.helper=helper; }
}
// One companion per attached WindowStage. The helper is private SDK UI, never a
// public nativeapi Window. Core supplies actual raster pixels and cancellation.
export class NativeWindowShadowHost {
  private stage: window.WindowStage;
  private page: string;
  private surfaces: Map<number,ShadowSurface> = new Map();
  private serial: number = 0;
  private disposed: boolean = false;
  constructor(stage: window.WindowStage, page: string) { this.stage=stage; this.page=page; }
  private alive(valid: () => boolean): void { if(this.disposed || !valid())throw new Error('Shadow request invalidated'); }
  private async remove(id: number, expected?: ShadowSurface): Promise<void> {
    const surface=this.surfaces.get(id);if(!surface || (expected && surface!==expected))return;
    this.surfaces.delete(id);
    try {surface.target.off('windowEvent',surface.destroyed);}catch(_){}
    try {surface.helper.off('windowEvent',surface.helperDestroyed);}catch(_){}
    try {await surface.helper.destroyWindow();}catch(_){}
  }
  async apply(target: window.Window, pixels: image.PixelMap, margin: number, width: number, height: number,
    enabled: boolean, primary: boolean, valid: () => boolean): Promise<void> {
    this.alive(valid);
    const id=target.getWindowProperties().id;
    let surface=this.surfaces.get(id);
    // SDK destruction may settle before its event is delivered. Never retain a
    // dead helper that would make all subsequent configuration requests fail.
    if(surface)try {surface.helper.getWindowProperties();}catch(_) {
      await this.remove(id,surface);this.alive(valid);surface=undefined;
    }
    if(!enabled) {
      if(surface)await surface.helper.minimize();
      if(primary)await target.setWindowShadowEnabled(false);else target.setWindowShadowRadius(0);
      this.alive(valid);return;
    }
    let created=false;
    try {
      // RectChangeOptions carries the new frame before the synchronous property
      // cache is committed on some phones. Wait for that specific frame. The
      // native validity callback rejects superseded geometry generations.
      let rect=target.getWindowProperties().windowRect;
      for(let i=0;rect.width+margin*2!==width || rect.height+margin*2!==height;++i) {
        if(i===120)throw new Error('Shadow geometry was not committed');
        await new Promise<void>((resolve)=>{setTimeout(resolve,16);});this.alive(valid);
        rect=target.getWindowProperties().windowRect;
      }
      if(!surface) {
        const helper=await this.stage.createSubWindow(`NativeAPI shadow ${id} ${++this.serial}`);
        surface=new ShadowSurface(target,helper);created=true;this.surfaces.set(id,surface);this.alive(valid);
        const current=surface;
        surface.destroyed=(event: window.WindowEventType)=>{if(event===window.WindowEventType.WINDOW_DESTROYED)this.remove(id,current);};
        target.on('windowEvent',surface.destroyed);
        surface.helperDestroyed=(event: window.WindowEventType)=>{if(event===window.WindowEventType.WINDOW_DESTROYED)this.remove(id,current);};
        helper.on('windowEvent',surface.helperDestroyed);
        await helper.loadContent(this.page,surface.storage);this.alive(valid);
        helper.setWindowBackgroundColor('#00000000');helper.setWindowShadowRadius(0);
        await helper.setWindowCornerRadius(0);this.alive(valid);
        await helper.setWindowFocusable(false);await helper.setWindowTouchable(false);this.alive(valid);
        await helper.setSubWindowZLevel(primary ? -10000 : target.getSubWindowZLevel());this.alive(valid);
      }
      rect=target.getWindowProperties().windowRect;
      if(rect.width+margin*2!==width || rect.height+margin*2!==height)throw new Error('Shadow raster size became stale');
      surface.storage.setOrCreate('nativeapiShadowPixels',pixels);
      await surface.helper.resize(width,height);this.alive(valid);
      await surface.helper.moveWindowTo(rect.left-margin,rect.top-margin);this.alive(valid);
      if(primary)await target.setWindowShadowEnabled(false);else target.setWindowShadowRadius(0);
      this.alive(valid);
      if(target.isWindowShowing()) {
        await surface.helper.showWindow({focusOnShow:false});this.alive(valid);
        // Same z-level: place the real child over its private helper. This does
        // not request focus; all interior pixels are transparent in either order.
        if(!primary)await target.raiseToAppTop();
      } else await surface.helper.minimize();
      this.alive(valid);
    } catch(error) { if(created)await this.remove(id);throw error as Error; }
  }
  async clear(target: window.Window, enabled: boolean, primary: boolean): Promise<void> {
    const id=target.getWindowProperties().id;
    await this.remove(id);
    if(primary)await target.setWindowShadowEnabled(enabled);else target.setWindowShadowRadius(enabled ? 18*target.getWindowDensityInfo().systemDensity : 0);
  }
  async hide(id: number): Promise<void> { const surface=this.surfaces.get(id);if(surface)try {await surface.helper.minimize();}catch(_){} }
  async dispose(): Promise<void> { this.disposed=true;for(const id of this.surfaces.keys())await this.remove(id); }
}
