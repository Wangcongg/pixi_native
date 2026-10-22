import { P, V8, unsupported } from "./environment.mjs";
import { applyOptions, installContainerAPI, installViewHitAPI } from "./container.mjs";
const TextStyle = V8.TextStyle;
const fields = ["align", "breakWords", "fontFamily", "fontSize", "fontStyle", "fontVariant", "fontWeight", "leading", "letterSpacing", "lineHeight", "padding", "textBaseline", "trim", "whiteSpace", "wordWrap", "wordWrapWidth"];
function nativeStyle(style) {
  const o = {};
  for (const key of fields) o[key] = style[key];
  const fill = style._fill, stroke = style._stroke, shadow = style.dropShadow;
  if (fill?.fill || stroke?.fill) unsupported("TextStyle textured/gradient fill");
  o.fill = fill?.color ?? 0;
  o.stroke = stroke?.color ?? 0;
  o.strokeThickness = stroke?.width ?? 0;
  o.lineJoin = stroke?.join ?? "miter";
  o.miterLimit = stroke?.miterLimit ?? 10;
  o.dropShadow = !!shadow;
  if (shadow) {
    o.dropShadowColor = shadow.color;
    o.dropShadowAlpha = shadow.alpha;
    o.dropShadowAngle = shadow.angle;
    o.dropShadowBlur = shadow.blur;
    o.dropShadowDistance = shadow.distance;
  }
  return new P.TextStyle(o);
}
const styleDescriptor = Object.getOwnPropertyDescriptor(P.Text.prototype, "style");
const automaticFonts = /* @__PURE__ */ new Map();
let automaticFontId = 0, bitmapPageId = 0;
function bitmapFamily(style) {
  return Array.isArray(style.fontFamily) ? style.fontFamily[0] : style.fontFamily;
}
function bitmapFontFor(style, text) {
  const named = P.BitmapFont.available[bitmapFamily(style)];
  if (named) {
    named.ensureCharacters?.(text);
    return { font: named, name: bitmapFamily(style), automatic: null };
  }
  const key = style.styleKey;
  let entry = automaticFonts.get(key);
  if (!entry) {
    const tintable = !style._fill?.fill && !style._stroke && !style.dropShadow, copy = style.clone();
    if (tintable) copy.fill = 16777215;
    const name = "__pixi_native_auto_" + ++automaticFontId;
    const font = BitmapFont.install({ name, style: copy, chars: text || " ", dynamicFill: tintable });
    copy.destroy();
    entry = { key, name, font, users: 0 };
    automaticFonts.set(key, entry);
  }
  entry.font.ensureCharacters(text);
  return { font: entry.font, name: entry.name, automatic: entry };
}
function releaseAutomaticFont(entry) {
  // Named/loaded fonts remain caller-owned. Only automatically generated fonts
  // are released with their last BitmapText view.
  if (entry && --entry.users === 0) {
    automaticFonts.delete(entry.key);
    P.BitmapFont.uninstall(entry.name);
  }
}
class Text extends P.Text {
  constructor(options = {}, legacyStyle) {
    if (typeof options !== "object") options = { text: options, style: legacyStyle };
    super(options.text ?? "", {});
    this.style = options.style || {};
    if (options.resolution !== void 0) this.resolution = options.resolution;
    applyOptions(this, options);
  }
  get style() {
    return this.__readingNativeStyle || !this._v8Style ? styleDescriptor.get.call(this) : this._v8Style;
  }
  set style(value) {
    this._v8Style?.off("update", this.onStyleChange, this);
    this._v8Style = value instanceof V8.TextStyle ? value : new V8.TextStyle(value instanceof P.TextStyle ? Object.fromEntries(fields.map((k) => [k, value[k]])) : value);
    this._v8Style.on("update", this.onStyleChange, this);
    this.onStyleChange();
  }
  onStyleChange() {
    this.__v8StyleKey = null;
    this._invalidateNativeText();
  }
  _syncNativeText(force) {
    if (this._v8Style && this.__v8StyleKey !== this._v8Style.styleKey) {
      styleDescriptor.set.call(this, nativeStyle(this._v8Style));
      this.__v8StyleKey = this._v8Style.styleKey;
    }
    this.__readingNativeStyle = true;
    try {
      return super._syncNativeText(force);
    } finally {
      this.__readingNativeStyle = false;
    }
  }
  destroy(options) {
    if (this.destroyed) return;
    this._v8Style?.off("update", this.onStyleChange, this);
    const style = this._v8Style;
    super.destroy(options);
    this._v8Style = null;
    if (options === true || options?.style) style?.destroy();
  }
}
installContainerAPI(Text.prototype);
installViewHitAPI(Text.prototype);
class BitmapText extends P.BitmapText {
  constructor(options = {}) {
    if (typeof options !== "object") options = { text: options };
    const style = options.style instanceof TextStyle ? options.style : new TextStyle({ fill: 16777215, ...options.style }), resolved = bitmapFontFor(style, String(options.text ?? ""));
    super(options.text ?? "", { fontName: resolved.name, fontSize: style.fontSize, align: style.align, letterSpacing: style.letterSpacing, tint: resolved.font.__nativeDynamicFill === false ? 16777215 : style._fill?.color ?? 16777215 });
    this._v8Style = style;
    this.__automaticFont = resolved.automatic;
    if (resolved.automatic) resolved.automatic.users++;
    style.on("update", this.onStyleChange, this);
    applyOptions(this, options);
  }
  get style() {
    return this._v8Style;
  }
  set style(value) {
    this._v8Style?.off("update", this.onStyleChange, this);
    this._v8Style = value instanceof TextStyle ? value : new TextStyle(value);
    this._v8Style.on("update", this.onStyleChange, this);
    this.onStyleChange();
  }
  onStyleChange() {
    const s = this._v8Style, resolved = bitmapFontFor(s, this.text), old = this.__automaticFont;
    if (resolved.automatic !== old) {
      if (resolved.automatic) resolved.automatic.users++;
      this.__automaticFont = resolved.automatic;
    }
    this.fontName = resolved.name;
    this.fontSize = s.fontSize;
    this.align = s.align;
    this.letterSpacing = s.letterSpacing;
    this.tint = resolved.font.__nativeDynamicFill === false ? 16777215 : s._fill?.color ?? 16777215;
    if (old !== resolved.automatic) releaseAutomaticFont(old);
  }
  _syncIfReady() {
    const font = P.BitmapFont.available[this._fontName];
    font?.ensureCharacters?.(this._text);
    return super._syncIfReady();
  }
  validate() {
    P.BitmapFont.available[this._fontName]?.ensureCharacters?.(this._text);
    return super.validate();
  }
  destroy(options) {
    if (this.destroyed) return;
    this._v8Style?.off("update", this.onStyleChange, this);
    super.destroy(options);
    releaseAutomaticFont(this.__automaticFont);
    this.__automaticFont = null;
  }
}
installContainerAPI(BitmapText.prototype);
installViewHitAPI(BitmapText.prototype);
class BitmapFont {
  static install(options, ...args) {
    if (options?.name) {
      const style = options.style instanceof TextStyle ? options.style : new TextStyle(options.style), native = nativeStyle(style);
      const dynamicFill = options.dynamicFill ?? (!style._stroke && !style._fill?.fill && (!style.dropShadow || style.dropShadow.color === 0) && style._fill?.color === 16777215);
      if (dynamicFill) native.fill = 16777215;
      const config = Object.fromEntries(Object.entries({ chars: options.chars, resolution: options.resolution, padding: options.padding, textureWidth: options.textureSize, textureHeight: options.textureSize, skipKerning: options.skipKerning }).filter(([, v]) => v !== void 0));
      const font2 = P.BitmapFont.from(options.name, native, config);
      font2.fontFamily = font2.font;
      font2.__nativeDynamicFill = dynamicFill;
      font2.ensureCharacters = (text) => {
        if (!font2.chars) throw new Error("BitmapFont has been destroyed");
        const missing = [...new Set(Array.from(String(text ?? "")))].filter((ch) => ch !== "\n" && ch !== "\r" && !font2.chars[ch.codePointAt(0)]);
        if (!missing.length) return;
        let extra;
        // BitmapFont.from replaces and destroys an existing registration. Use
        // a fresh temporary name so live glyphs and their texture owners survive.
        const pageName = "__pixi_native_page_" + ++bitmapPageId;
        try {
          extra = P.BitmapFont.from(pageName, native, { ...config, chars: missing.join("") });
        } finally {
          delete P.BitmapFont.available[pageName];
        }
        const offset = Object.keys(font2.pageTextures).length;
        for (const [id, page] of Object.entries(extra.pageTextures)) font2.pageTextures[offset + Number(id)] = page;
        for (const [id, glyph] of Object.entries(extra.chars)) {
          glyph.page += offset;
          font2.chars[id] = glyph;
        }
        // Transfer ownership to the original font; its normal destroy releases
        // all pages exactly once. Previously returned glyph textures stay valid.
        extra.chars = {};
        extra.pageTextures = {};
      };
      return font2;
    }
    const font = P.BitmapFont.install(options, ...args);
    font.fontFamily = font.font;
    return font;
  }
  static uninstall(name) {
    return P.BitmapFont.uninstall(name);
  }
  static get available() {
    return P.BitmapFont.available;
  }
  static get ASCII() {
    return P.BitmapFont.ASCII;
  }
  static get ALPHA() {
    return P.BitmapFont.ALPHA;
  }
  static get ALPHANUMERIC() {
    return P.BitmapFont.ALPHANUMERIC;
  }
  static get NUMERIC() {
    return P.BitmapFont.NUMERIC;
  }
}
export {
  BitmapFont,
  BitmapText,
  Text,
  TextStyle
};
