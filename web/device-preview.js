// A Sticky screen for the setup page, as the firmware draws it.
//
// The fixed parts come from images rendered by the firmware's own code
// (web/previews/, made by web/make_previews.py). The parts that depend on the
// visitor's choices (status bar, light and player tiles, the app key) are
// drawn here: a line-by-line port of those routines in remote_ui.h, using the
// firmware's exact glyphs (web/fonts/, from the ESPHome build). Checked pixel
// for pixel against the firmware's own render (web/check_preview.py).

const W = 480, H = 800, M = 14, STATUS_H = 36;
const TILE_Y = 44, TILE_H = 128, TILE_GAP = 16;
const KEY_Y = 580, KEY_H = 72, KEY_W = 104, KEY_GAP = 12;
const ROLES = ["t18", "t20", "t22", "t26", "t30", "i24", "i40", "i52", "i64", "i120"];
const ICON = {
  bulb_on: 0xf0335, bulb_off: 0xf0336, ceiling_on: 0xf0769, ceiling_off: 0xf17c7,
  floor_on: 0xf08dd, floor_off: 0xf17c8, star_on: 0xf0ae2, star_off: 0xf0ae3,
  power: 0xf0425, wifi: 0xf05a9, wifi_sync: 0xf16c7, movie: 0xf0fcf,
};
const idiv = (a, b) => Math.trunc(a / b); // C integer division

// --- Fonts (ESPHome glyph tables, see web/make_web_fonts.py) ---------------------------
class Font {
  constructor(text) {
    this.glyphs = new Map();
    this.first = 0;
    let lowest = Infinity;
    for (const line of text.split("\n")) {
      const p = line.split(" ");
      if (p[0] === "font") { this.ascent = +p[1]; this.height = +p[2]; }
      else if (p[0] === "g") {
        const [cp, adv, ox, oy, w, h] = p.slice(1, 7).map(Number);
        const hex = p[7], bits = new Uint8Array(w * h);
        if (hex && hex !== "-") for (let i = 0; i < w * h; i++) {
          const byte = parseInt(hex.substr((i >> 3) * 2, 2), 16);
          bits[i] = (byte >> (7 - (i & 7))) & 1;
        }
        this.glyphs.set(cp, { adv, ox, oy, w, h, bits });
        if (cp < lowest) { lowest = cp; this.first = adv; }
      }
    }
  }
}

const fontCache = new Map();
export async function loadFonts(set) {
  if (!fontCache.has(set)) {
    fontCache.set(set, (async () => {
      const fonts = {};
      await Promise.all(ROLES.map(async (r) => {
        fonts[r] = new Font(await (await fetch(`fonts/${set}/${r}.fnt`)).text());
      }));
      return fonts;
    })());
  }
  return fontCache.get(set);
}

const baseCache = new Map();
async function loadBase(name) {
  if (!baseCache.has(name)) {
    baseCache.set(name, new Promise((resolve, reject) => {
      const img = new Image();
      img.onload = () => {
        const c = document.createElement("canvas");
        c.width = W; c.height = H;
        const g = c.getContext("2d");
        g.drawImage(img, 0, 0);
        const d = g.getImageData(0, 0, W, H).data, px = new Uint8Array(W * H);
        for (let i = 0; i < W * H; i++) px[i] = d[i * 4] > 127 ? 1 : 0;
        resolve(px);
      };
      img.onerror = reject;
      img.src = `previews/${name}.png`;
    }));
  }
  return baseCache.get(name);
}

// --- Drawing primitives, as ESPHome's Display does them --------------------------------
const INK = 0, PAPER = 1;
class Screen {
  constructor(px) { this.px = px; }
  pixel(x, y, c) { if (x >= 0 && y >= 0 && x < W && y < H) this.px[y * W + x] = c; }
  rect(x, y, w, h, c) { for (let j = y; j < y + h; j++) for (let i = x; i < x + w; i++) this.pixel(i, j, c); }
  circle(cx, cy, r, c) {
    let dx = -r, dy = 0, err = 2 - 2 * r, e2;
    do {
      for (let x = cx + dx; x <= cx - dx; x++) { this.pixel(x, cy + dy, c); this.pixel(x, cy - dy, c); }
      e2 = err;
      if (e2 < dy) { err += ++dy * 2 + 1; if (-dx === dy && e2 <= dx) e2 = 0; }
      if (e2 > dx) err += ++dx * 2 + 1;
    } while (dx <= 0);
  }
  rrect(x, y, w, h, r, c) {
    r = Math.min(r, idiv(Math.min(w, h), 2));
    this.rect(x + r, y, w - 2 * r, h, c);
    this.rect(x, y + r, r, h - 2 * r, c);
    this.rect(x + w - r, y + r, r, h - 2 * r, c);
    this.circle(x + r, y + r, r, c);
    this.circle(x + w - r - 1, y + r, r, c);
    this.circle(x + r, y + h - r - 1, r, c);
    this.circle(x + w - r - 1, y + h - r - 1, r, c);
  }
  strokeRrect(x, y, w, h, r, t) {
    this.rrect(x, y, w, h, r, INK);
    this.rrect(x + t, y + t, w - 2 * t, h - 2 * t, Math.max(0, r - t), PAPER);
  }

  // Font::measure + Display::get_text_bounds
  measure(text, f) {
    let minX = 0, x = 0, has = false;
    for (const ch of text) {
      const g = f.glyphs.get(ch.codePointAt(0));
      if (!g) { x += f.first; continue; }
      minX = has ? Math.min(minX, x + g.ox) : g.ox;
      x += g.adv; has = true;
    }
    return { width: x - minX, xOffset: minX };
  }
  // align: [horizontal "left"|"center"|"right", vertical "top"|"center"]
  print(x, y, f, c, align, text) {
    const { width, xOffset } = this.measure(text, f);
    let x1 = align[0] === "right" ? x - width - xOffset : align[0] === "center" ? x - idiv(width + xOffset, 2) : x;
    const y1 = align[1] === "center" ? y - idiv(f.height, 2) : y;
    for (const ch of text) {
      const g = f.glyphs.get(ch.codePointAt(0));
      if (!g) {   // ESPHome outlines a missing character
        this.rect(x1, y1, f.first, 1, c); this.rect(x1, y1 + f.height - 1, f.first, 1, c);
        this.rect(x1, y1, 1, f.height, c); this.rect(x1 + f.first - 1, y1, 1, f.height, c);
        x1 += f.first; continue;
      }
      for (let j = 0; j < g.h; j++) for (let i = 0; i < g.w; i++)
        if (g.bits[j * g.w + i]) this.pixel(x1 + g.ox + i, y1 + g.oy + j, c);
      x1 += g.adv;
    }
  }
  // remote_ui::print_fit: shorten with "…" to fit maxW
  printFit(x, y, f, c, align, text, maxW) {
    let cps = [...text];
    if (this.measure(text, f).width > maxW) {
      while (cps.length) {
        cps.pop();
        if (this.measure(cps.join("") + "…", f).width <= maxW) break;
      }
      while (cps.length && cps[cps.length - 1] === " ") cps.pop();
      cps.push("…");
    }
    this.print(x, y, f, c, align, cps.join(""));
  }
}

// --- The parts that depend on the visitor's choices --------------------------------------
const tileW = (n) => idiv(W - 2 * M - (n - 1) * TILE_GAP, n);
const tileX = (i, n) => M + i * (tileW(n) + TILE_GAP);
const glyph = (name) => String.fromCodePoint(ICON[name]);

function drawStatus(s, f, d) {
  const cy = idiv(STATUS_H, 2);
  let x = W - M;
  if (d.clock) {
    s.print(x, cy, f.t20, INK, ["right", "center"], d.clock);
    x -= s.measure(d.clock, f.t20).width + 14;
  }
  s.print(x, cy + 1, f.i24, INK, ["right", "center"], glyph("wifi"));
  x -= 24 + 12;
  s.printFit(M, cy, f.t20, INK, ["left", "center"], d.room || "", x - M);
}

function drawTile(s, f, i, n, label, icon, state, on) {
  const x = tileX(i, n), w = tileW(n), r = 14;
  if (on) s.rrect(x, TILE_Y, w, TILE_H, r, INK); else s.strokeRrect(x, TILE_Y, w, TILE_H, r, 3);
  const fg = on ? PAPER : INK, cx = x + idiv(w, 2);
  s.print(cx, TILE_Y + 42, f.i52, fg, ["center", "center"], glyph(icon));
  s.printFit(cx, TILE_Y + 88, f.t22, fg, ["center", "center"], label, w - 12);
  s.print(cx, TILE_Y + 110, f.t18, fg, ["center", "center"], state);
}

function lightState(l) {
  if (!l.on) return l.offline ? "Offline" : "Off";
  return l.brightness != null ? `${Math.round(l.brightness * 100 / 255)} %` : "On";
}

/**
 * Draws the Remote page with these choices into `canvas` (480×800).
 * d = { set, room, clock, lights: [{label, icon, on, brightness, offline}], player: {label, on}, app }
 */
export async function renderRemote(canvas, d) {
  const [fonts, base] = await Promise.all([loadFonts(d.set), loadBase(`base-${d.set}`)]);
  const s = new Screen(new Uint8Array(base));
  drawStatus(s, fonts, d);
  const n = d.lights.length + 1;
  d.lights.forEach((l, i) => drawTile(s, fonts, i, n, l.label || "", `${l.icon || "bulb"}_${l.on ? "on" : "off"}`, lightState(l), l.on));
  drawTile(s, fonts, n - 1, n, d.player.label || "Player", "power", d.player.on ? "On" : "Off", d.player.on);
  // The app key (third key of the Remote page), drawn as draw_remote does.
  const kx = M + 2 * (KEY_W + KEY_GAP);
  s.rect(kx, KEY_Y, KEY_W, KEY_H, PAPER);
  s.strokeRrect(kx, KEY_Y, KEY_W, KEY_H, 16, 3);
  s.print(kx + idiv(KEY_W, 2), KEY_Y + 24, fonts.i40, INK, ["center", "center"], glyph("movie"));
  s.printFit(kx + idiv(KEY_W, 2), KEY_Y + 54, fonts.t18, INK, ["center", "center"], d.app || "App", KEY_W - 8);
  paint(canvas, s.px);
  return s.px;
}

/** Shows a fixed screen (welcome page). */
export async function renderImage(canvas, name) { paint(canvas, await loadBase(name)); }

// E-paper tones: ink and paper are never pure black and white.
function paint(canvas, px) {
  canvas.width = W; canvas.height = H;
  const g = canvas.getContext("2d"), img = g.createImageData(W, H);
  for (let i = 0; i < W * H; i++) {
    const v = px[i] ? 226 : 34, o = i * 4;
    img.data[o] = v; img.data[o + 1] = v - 2; img.data[o + 2] = v - 8; img.data[o + 3] = 255;
  }
  g.putImageData(img, 0, 0);
}
