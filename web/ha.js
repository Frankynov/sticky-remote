// Home Assistant from the browser, over its WebSocket API (the REST API
// refuses other sites; WebSockets don't). The token stays in this page.

export class HAError extends Error {}

export class HAClient {
  constructor(url, token) {
    this.base = url.replace(/\/+$/, "");
    this.token = token;
    this.id = 1;
    this.pending = new Map();
    this.subs = new Map();
  }

  /** Why this page can't reach `url`, or null. */
  static problem(url) {
    let u;
    try { u = new URL(url); } catch { return "That isn't a valid address. Use something like https://homeassistant.example.com"; }
    if (u.protocol !== "https:" && u.protocol !== "http:") return "The address must start with https://";
    if (u.protocol === "http:" && location.protocol === "https:" && !["localhost", "127.0.0.1"].includes(u.hostname))
      return "Browsers don't let a secure page (https) talk to an http:// address. Use your Home Assistant's https address (for example your Nabu Casa or reverse-proxy URL), or enter the entities by hand below.";
    return null;
  }

  connect() {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(this.base.replace(/^http/, "ws") + "/api/websocket");
      this.ws = ws;
      const fail = (m) => { reject(new HAError(m)); try { ws.close(); } catch {} };
      const timer = setTimeout(() => fail("Home Assistant didn't answer. Check the address."), 10000);
      ws.onerror = () => { clearTimeout(timer); fail("Couldn't reach Home Assistant at that address."); };
      ws.onmessage = (ev) => {
        const msg = JSON.parse(ev.data);
        if (msg.type === "auth_required") ws.send(JSON.stringify({ type: "auth", access_token: this.token }));
        else if (msg.type === "auth_invalid") { clearTimeout(timer); fail("Home Assistant refused the token."); }
        else if (msg.type === "auth_ok") { clearTimeout(timer); this.version = msg.ha_version; resolve(this); }
        else if (msg.type === "result" && this.pending.has(msg.id)) {
          const { res, rej } = this.pending.get(msg.id);
          this.pending.delete(msg.id);
          msg.success ? res(msg.result) : rej(new HAError(msg.error?.message || "Request failed"));
        } else if (msg.type === "event" && this.subs.has(msg.id)) this.subs.get(msg.id)(msg.event);
      };
    });
  }

  call(payload) {
    const id = ++this.id;
    this.ws.send(JSON.stringify({ id, ...payload }));
    return new Promise((res, rej) => this.pending.set(id, { res, rej }));
  }

  states() { return this.call({ type: "get_states" }); }
  config() { return this.call({ type: "get_config" }); }

  /** Calls f(entity_id, {state, attributes}) whenever one of `entities` changes. */
  async watch(entities, f) {
    const id = this.id + 1, known = {};
    this.subs.set(id, (ev) => {
      for (const [eid, e] of Object.entries(ev.a || {})) {
        known[eid] = { state: e.s, attributes: e.a || {} };
        f(eid, known[eid]);
      }
      for (const [eid, diff] of Object.entries(ev.c || {})) {
        const k = known[eid] || (known[eid] = { state: "", attributes: {} });
        if (diff["+"]?.s !== undefined) k.state = diff["+"].s;
        if (diff["+"]?.a) Object.assign(k.attributes, diff["+"].a);
        for (const gone of diff["-"]?.a || []) delete k.attributes[gone];
        f(eid, k);
      }
    });
    await this.call({ type: "subscribe_entities", entity_ids: entities });
  }

  close() { try { this.ws?.close(); } catch {} }
}

/** A pretend Home Assistant for demo mode. */
export class DemoHA {
  constructor() { this.version = "2026.11.0 (demo)"; this.lightOn = false; }
  async connect() { await new Promise((r) => setTimeout(r, 600)); return this; }
  async config() { return { location_name: "Demo home" }; }
  async states() {
    const L = (id, name, s) => ({ entity_id: id, state: s, attributes: { friendly_name: name } });
    return [
      { entity_id: "media_player.salon", state: "idle",
        attributes: { friendly_name: "Salon Apple TV", source_list: ["Infuse", "Netflix", "YouTube", "TV"] } },
      { entity_id: "media_player.kitchen_speaker", state: "idle", attributes: { friendly_name: "Kitchen speaker" } },
      L("remote.salon", "Salon Apple TV", "on"),
      L("light.floor_lamp", "Floor lamp", "off"),
      { entity_id: "light.ceiling", state: "on", attributes: { friendly_name: "Ceiling", brightness: 158 } },
      L("light.desk", "Desk lamp", "off"),
    ];
  }
  async watch(entities, f) {
    const states = await this.states();
    for (const id of entities) { const e = states.find((x) => x.entity_id === id); if (e) f(id, { state: e.state, attributes: e.attributes }); }
    // Demo: the first light toggles, as if the visitor pressed its tile on the Sticky.
    const first = states.find((x) => x.entity_id === entities[0]);
    const nowOn = first?.state !== "on";
    setTimeout(() => f(entities[0], { state: nowOn ? "on" : "off", attributes: nowOn ? { brightness: 200 } : {} }), 5000);
  }
  close() {}
}
