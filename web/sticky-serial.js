// The Sticky over USB: its setup protocol (sticky_device.h) and flashing.
//
// Setup protocol: newline-delimited JSON at 115200 baud on the console UART,
// shared with the log. We keep lines starting with '{' that carry
// "sticky":"setup". Commands: get, set (settings and/or wifi), reset. The board
// sends {"event":"hello",…} at boot. Its receive buffer is small: we send 64
// bytes at a time with short pauses.

import { ESPLoader, Transport } from "./vendor/esptool-js-0.7.1.js";

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export class StickySerial {
  constructor(port) {
    this.port = port;
    this.listeners = new Set();
    this.reading = false;
  }

  /** Opens the port at 115200 without disturbing the chip (DTR/RTS low). */
  async open() {
    await this.port.open({ baudRate: 115200, bufferSize: 8192 });
    try { await this.port.setSignals({ dataTerminalReady: false, requestToSend: false }); } catch {}
    this.writer = this.port.writable.getWriter();
    this.reading = true;
    this.readLoop();
  }

  async readLoop() {
    const decoder = new TextDecoder();
    let pending = "";
    while (this.reading && this.port.readable) {
      this.reader = this.port.readable.getReader();
      try {
        for (;;) {
          const { value, done } = await this.reader.read();
          if (done) break;
          pending += decoder.decode(value, { stream: true });
          let i;
          while ((i = pending.search(/[\r\n]/)) >= 0) {
            const line = pending.slice(0, i).trim();
            pending = pending.slice(i + 1);
            if (!line.startsWith("{") || !line.includes('"sticky"')) continue;
            try {
              const msg = JSON.parse(line);
              if (msg.sticky === "setup") this.listeners.forEach((f) => f(msg));
            } catch {}
          }
          if (pending.length > 65536) pending = pending.slice(-4096);
        }
      } catch { /* port closed or device reset */ }
      finally { try { this.reader.releaseLock(); } catch {} }
      if (this.reading) await sleep(200);
    }
  }

  async close() {
    this.reading = false;
    try { await this.reader?.cancel(); } catch {}
    try { this.writer?.releaseLock(); } catch {}
    try { await this.port.close(); } catch {}
  }

  async send(obj) {
    const bytes = new TextEncoder().encode("\n" + JSON.stringify(obj) + "\n");
    for (let i = 0; i < bytes.length; i += 64) {
      await this.writer.write(bytes.slice(i, i + 64));
      await sleep(12);
    }
  }

  /** Next message matching `test` within `ms`, or null. */
  next(test, ms) {
    return new Promise((resolve) => {
      const done = (v) => { clearTimeout(t); this.listeners.delete(on); resolve(v); };
      const on = (m) => { if (!test || test(m)) done(m); };
      const t = setTimeout(() => done(null), ms);
      this.listeners.add(on);
    });
  }

  /** Asks for the info line, retrying; null if the firmware doesn't answer. */
  async info(tries = 3) {
    for (let i = 0; i < tries; i++) {
      const wait = this.next((m) => m.ok && m.event, 1500);
      await this.send({ sticky: "setup", cmd: "get" });
      const r = await wait;
      if (r) return r;
    }
    return null;
  }

  /** Starts a Wi-Fi scan (firmware 0.4+; older ones answer with an error, ignored). The list comes with info(). */
  async scan() { try { await this.send({ sticky: "setup", cmd: "scan" }); } catch {} }
  /** Forgets the key Home Assistant gave it, so Home Assistant can add it afresh. */
  async unpair() { try { await this.send({ sticky: "setup", cmd: "unpair" }); } catch {} }

  /** Saves settings and/or Wi-Fi; resolves once the board confirmed (it then restarts). */
  async set({ settings, wifi }) {
    const cmd = { sticky: "setup", cmd: "set" };
    if (settings) cmd.settings = settings;
    if (wifi) cmd.wifi = wifi;
    const wait = this.next((m) => m.saved || m.ok === false, 5000);
    await this.send(cmd);
    const r = await wait;
    if (!r) throw new Error("The Sticky didn't confirm. Try again, or reconnect the cable.");
    if (r.ok === false) throw new Error(r.error || "The Sticky refused the settings.");
    return r;
  }

  /** After a restart: waits for the board to answer again. */
  async waitForBoard(ms = 25000) {
    const end = Date.now() + ms;
    while (Date.now() < end) {
      const r = await this.info(1);
      if (r) return r;
      await sleep(800);
    }
    return null;
  }
}

/** Flashes `data` (a factory image) at 0x0, reporting progress (0…1) and log lines. */
export async function flash(port, data, { eraseAll, onProgress, onLog }) {
  const transport = new Transport(port, false);
  const terminal = {
    clean() {},
    writeLine: (l) => onLog?.(l),
    write: (l) => onLog?.(l),
  };
  const loader = new ESPLoader({ transport, baudrate: 460800, romBaudrate: 115200, terminal });
  try {
    const chip = await loader.main("default_reset");
    onLog?.(`Connected to ${chip}`);
    if (!/ESP32-S3/i.test(chip)) throw new Error(`This is an ${chip}, not the Sticky's ESP32-S3.`);
    await loader.writeFlash({
      fileArray: [{ data, address: 0 }],
      flashMode: "keep", flashFreq: "keep", flashSize: "keep",
      eraseAll, compress: true,
      reportProgress: (_i, written, total) => onProgress?.(written / total),
    });
    onLog?.("Restarting the Sticky…");
    await loader.after("hard_reset");
  } finally {
    try { await transport.disconnect(); } catch {}
  }
}

// --- A pretend Sticky, for trying the wizard without hardware ------------------------
export class DemoSticky {
  constructor({ installed, configured }) {
    this.state = {
      fw: installed ? "0.2.0" : null, name: "sticky-remote-d3m0a1", wifi: null, settings: {},
      ha: { connected: false, paired: false },
    };
    if (configured) {   // set up and in use; like the real one, it restarts when the port opens
      this.state.wifi = { ssid: "Maison" };
      this.state.ha = { connected: false, paired: true };
      this.booted = Date.now();
      this.state.settings = { player: "media_player.salon", remote: "remote.salon", player_name: "Apple TV", app: "Infuse",
        app_label: "Open Infuse", light1: "light.floor_lamp", light1_name: "Floor lamp", light1_icon: "floor",
        light2: "light.ceiling", light2_name: "Ceiling", light2_icon: "ceiling", room: "Salon", ha_url: "", font_set: "inter",
        touch_sound: "quiet", sleep_after: "3", sleep_clock: "on" };
    }
  }
  /** Demo: after "flashing", the new version runs; erasing loses everything. */
  finishFlash(version, erased) {
    if (erased) Object.assign(this.state, { wifi: null, settings: {}, ha: { connected: false, paired: false } });
    else this.state.ha = { connected: false, paired: true };   // reconnects a bit later
    this.state.fw = version;
    setTimeout(() => { if (this.state.ha.paired) this.state.ha.connected = true; }, 5000);
  }
  async open() {}
  async close() {}
  /** Demo: the list fills in a few seconds after the first scan, as on the device. */
  async scan() { this.scanned ??= Date.now(); }
  async unpair() { this.state.ha = { connected: false, paired: false }; }
  async info() {
    await sleep(400);
    if (!this.state.fw) return null;
    const s = this.state;
    if (this.booted && Date.now() - this.booted > 8000 && s.ha.paired) s.ha.connected = true;
    return {
      sticky: "setup", event: "info", ok: true, protocol: 1, fw: s.fw, name: s.name, mac: "d0:ef:76:d3:a0:a1",
      wifi: s.wifi && !(this.booted && Date.now() - this.booted < 4000) ? { connected: true, ip: "192.168.1.57", ssid: s.wifi.ssid }
                   : { connected: false, ip: "" },
      ha: s.ha, configured: !!(s.settings.player || s.settings.light1), settings: s.settings,
      caps: ["scan", "touch_sound", "sleep_after", "sleep_clock", "unpair"],
      networks: this.scanned && Date.now() - this.scanned > 2500
        ? [{ ssid: "Maison", rssi: -48, secure: true }, { ssid: "Maison-IoT", rssi: -61, secure: true },
           { ssid: "Maison", rssi: -70, secure: true }, { ssid: "Cafe du coin", rssi: -77, secure: false },
           { ssid: "Voisins", rssi: -82, secure: true }]
        : [],
    };
  }
  async set({ settings, wifi }) {
    await sleep(500);
    if (settings) this.state.settings = { ...settings };
    if (wifi) {   // demo: any password works except "wrong"
      this.state.wifi = wifi.pass === "wrong" ? null : wifi;
      if (this.state.ha.paired) { this.state.ha.connected = false; setTimeout(() => { this.state.ha.connected = true; }, 6000); }
    }
    return { ok: true, saved: true, restarting: true };
  }
  async waitForBoard() { await sleep(2500); return this.info(); }
  /** Demo only: Home Assistant adds the Sticky a few seconds after asking. */
  pretendAdded() { setTimeout(() => { this.state.ha = { connected: true, paired: true }; }, 4000); }
}

export async function demoFlash({ onProgress, onLog }) {
  onLog?.("Connected to ESP32-S3 (demo)");
  for (let p = 0; p <= 1.0001; p += 0.04) { onProgress?.(p); await sleep(120); }
  onLog?.("Restarting the Sticky…");
}
