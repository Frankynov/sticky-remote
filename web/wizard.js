// Sticky Remote setup: install the firmware, set Wi-Fi, pick the player and
// lights from Home Assistant, add the Sticky to HA. Five steps; each moves on
// by itself once its job is done. A Sticky that's already set up gets a short
// menu instead. Static page, no build step. ?demo runs it against a pretend
// Sticky and Home Assistant.

import { StickySerial, flash, DemoSticky, demoFlash } from "./sticky-serial.js";
import { HAClient, DemoHA } from "./ha.js";
import { renderRemote, renderImage, drawIcon } from "./device-preview.js";

// ?demo: pretend Sticky and pretend Home Assistant. ?demo=sticky: pretend
// Sticky with your real Home Assistant. ?demo=update: the pretend Sticky
// already runs an older, set-up Sticky Remote.
const DEMO = new URLSearchParams(location.search).has("demo");
const DEMO_MODE = new URLSearchParams(location.search).get("demo");
const DEMO_HA = DEMO && DEMO_MODE !== "sticky";
const STEPS = ["Your Sticky", "Wi-Fi", "Home Assistant", "Your remote", "Finish"];
const WIFI = 1, HA = 2, REMOTE = 3, FINISH = 4;
const ICONS = { bulb: "Bulb", ceiling: "Ceiling light", floor: "Floor lamp", star: "Star" };

const S = {
  step: 0,
  manifest: null,
  port: null, serial: null, info: null,
  flow: "setup",          // setup | update | settings | wifi (what a set-up Sticky was plugged in for)
  installPanel: false,    // step 1 shows the installer (a new Sticky, an update, a reset)
  eraseAll: true, updated: false, finished: false,
  wifi: { ssid: "", pass: "", change: false, busy: false },
  ha: { url: localStorage.getItem("sticky.haURL") || "", remember: !!localStorage.getItem("sticky.haURL"),
        token: "", client: null, states: [], manual: false, location: "" },
  settings: {
    player: "", remote: "", player_name: "", app: "", app_label: "",
    light1: "", light1_name: "", light1_icon: "bulb", light2: "", light2_name: "", light2_icon: "bulb",
    room: "", ha_url: "", font_set: "inter", touch_sound: "quiet", sleep_after: "3", sleep_clock: "on",
  },
  timers: [],
  skipped: new Set(),   // steps this visit doesn't need (shown with a dash)
  live: {},             // latest entity states from Home Assistant, for the previews
};

const $ = (sel) => document.querySelector(sel);
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const newer = (a, b) => { // is version a newer than b?
  const pa = String(a).split(".").map(Number), pb = String(b || "0").split(".").map(Number);
  for (let i = 0; i < 3; i++) if ((pa[i] || 0) !== (pb[i] || 0)) return (pa[i] || 0) > (pb[i] || 0);
  return false;
};
const has = (cap) => !!S.info?.caps?.includes(cap);

function stopTimers() { S.timers.forEach(clearInterval); S.timers = []; }
function go(step) { stopTimers(); S.step = step; render(); window.scrollTo({ top: 0, behavior: "smooth" }); }
// The next and previous steps this visit needs.
function fwd() {
  if (S.flow === "wifi" && S.step === WIFI) return finish();   // moving networks ends there
  let n = S.step + 1;
  while (n < FINISH && S.skipped.has(n)) n++;
  go(n);
}
function back() { let n = S.step - 1; while (n > 0 && S.skipped.has(n)) n--; go(n); }

// --- Rendering -----------------------------------------------------------------------
function render() {
  $("#step-list").innerHTML = STEPS.map((t, i) => {
    const cls = i === S.step ? (S.finished ? "done" : "current") : S.skipped.has(i) ? "skipped" : i < S.step ? "done" : "";
    return `<li class="${cls}"${cls === "skipped" ? ' title="Not needed this time"' : ""}>${esc(t)}</li>`;
  }).join("");
  const left = STEPS.filter((_, i) => !S.skipped.has(i));
  const at = left.indexOf(STEPS[S.step]);
  $("#step-bar").innerHTML = `<span>Step ${at + 1} of ${left.length}</span><b>${esc(STEPS[S.step])}</b>
    <i style="width:${Math.round(100 * (at + 1) / left.length)}%"></i>`;
  const view = VIEWS[S.step]();
  $("#panel").innerHTML = view.html;
  view.bind?.();
  // A new step: keyboard and screen-reader users start at its title.
  if (S.step !== S.shown) {
    const h = $("#panel h2");
    if (h && S.shown !== undefined) { h.tabIndex = -1; h.focus({ preventScroll: true }); }
    S.shown = S.step;
  }
}

const actions = (hasBack, next, nextLabel = "Continue", nextDisabled = false) => `
  <div class="actions">
    ${hasBack ? `<button id="back">Back</button>` : ""}
    <span class="spacer"></span>
    ${next ? `<button class="primary" id="next" ${nextDisabled ? "disabled" : ""}>${esc(nextLabel)}</button>` : ""}
  </div>`;
function bindNav(onNext) {
  $("#back")?.addEventListener("click", back);
  $("#next")?.addEventListener("click", onNext || fwd);
}
const portRow = (primary) => `
  <div class="actions" style="margin-top:${primary ? 0 : 20}px">
    <button class="${primary ? "primary big" : ""}" id="pick">${S.port ? "Choose Another Port" : "Connect the Sticky"}</button>
    <label><input type="checkbox" id="allports"> Show every port</label>
  </div>
  <p id="pick-status" class="note" hidden></p>`;

const VIEWS = [
  // 0 · Your Sticky: welcome and connect, then install, or the menu of a set-up Sticky.
  () => {
    const i = S.info, fw = S.manifest?.version;
    const update = i && fw && newer(fw, i.fw);
    const bindPort = () => $("#pick")?.addEventListener("click", connect);

    if (!S.port) {
      const ok = DEMO || "serial" in navigator;
      return {
        html: `
          <div class="welcome">
            <div>
              <h2>Set up your Sticky</h2>
              <p class="lead">About five minutes. Plug the Sticky into this computer with a USB-C <b>data</b> cable, then connect it here. Its port is usually called “USB Single Serial” or “wchusbserial”.</p>
              ${ok ? portRow(true) : `<p class="note bad">This browser can't reach USB devices. Open this page in Chrome or Edge on a computer.</p>`}
              <h3>You'll also need</h3>
              <ul>
                <li>The Wi-Fi network for the Sticky (2.4 GHz).</li>
                <li>Home Assistant, reachable over <b>https</b>, and a long-lived access token from an admin account (your profile › Security).</li>
              </ul>
              <details><summary>Your data stays yours</summary>
                <ul class="privacy-list">
                  <li><b>Wi-Fi password</b>: sent only to your Sticky, over the USB cable.</li>
                  <li><b>Home Assistant token</b>: sent only to your Home Assistant, kept in memory and forgotten when you close this tab.</li>
                  <li><b>Home Assistant address</b>: remembered in this browser only if you want, and you can forget it.</li>
                  <li><b>This website</b> only serves the page and the firmware file. Nothing you type is sent to it or to its author. No analytics, no cookies, no third-party scripts: the page's security policy makes the browser block anything else.</li>
                </ul></details>
              ${DEMO ? "" : `<p><a href="?demo">Try the demo first (no Sticky needed)</a></p>`}
            </div>
            <div class="hero">
              <figure><div class="device"><canvas id="hero-now" width="480" height="800" aria-label="The Sticky while a film plays"></canvas></div><figcaption>While a film plays</figcaption></figure>
              <figure><div class="device"><canvas id="hero-remote" width="480" height="800" aria-label="The Sticky when nothing plays"></canvas></div><figcaption>Browsing</figcaption></figure>
            </div>
          </div>`,
        bind: () => {
          bindPort();
          renderImage($("#hero-now"), "welcome-now");
          renderImage($("#hero-remote"), "welcome-remote");
        },
      };
    }

    if (S.updated) return {
      html: `
        <h2>Updated</h2>
        <p class="lead">Its Wi-Fi, settings and Home Assistant pairing were kept.</p>
        ${updatedSummary()}
        <div class="actions"><button id="change">Change Its Settings Too</button><span class="spacer"></span><button class="primary" id="finish">Done</button></div>`,
      bind: () => {
        $("#change").addEventListener("click", () => startFlow("settings"));
        $("#finish").addEventListener("click", () => finish());
        if (!(S.info?.wifi?.connected && S.info?.ha?.connected)) {
          S.timers.push(setInterval(async () => {
            const n = await S.serial.info(1).catch(() => null);
            if (!n) return;
            S.info = n;
            stopTimers();   // the re-render starts a new check while something is still missing
            render();
          }, 3000));
        }
      },
    };

    if (S.installPanel) {
      const fresh = !i;
      return {
        html: `
          <h2>${fresh ? "Install Sticky Remote" : update ? `Update to ${esc(fw)}` : `Reinstall ${esc(fw)}`}</h2>
          <p class="lead">${fresh ? `Connected. Sticky Remote ${esc(fw)} replaces the firmware on the Sticky.` : `${esc(i.name)} runs ${esc(i.fw)}.`} Keep the cable plugged in until it's done (about a minute).</p>
          <details ${S.wasConfigured && S.eraseAll ? "open" : ""}><summary>Options</summary>
            <label class="field"><input type="checkbox" id="erase" ${S.eraseAll ? "checked" : ""}> Erase everything first
              <small>${S.wasConfigured
                ? "Not needed for an update. Erasing removes its Wi-Fi, its settings and the key Home Assistant gave it: you'd go through the whole setup and add it to Home Assistant again."
                : "Recommended the first time: clears the old firmware's data."}</small></label>
          </details>
          <p id="erase-warn" class="note warn" ${S.wasConfigured && S.eraseAll ? "" : "hidden"}>This Sticky is set up: erasing means setting it up again from scratch.</p>
          <div class="progress"><div id="bar"></div></div>
          <p id="phase" class="note">Ready.</p>
          <div id="log" class="log" hidden></div>
          <div class="actions">
            <button id="unpick">${i?.configured ? "Back" : "Choose Another Port"}</button>
            ${i && !i.configured ? `<button class="link" id="keepfw">Keep version ${esc(i.fw)}</button>` : ""}
            <span class="spacer"></span><button class="primary" id="go">${fresh ? "Install" : update ? "Update" : "Reinstall"}</button>
          </div>`,
        bind: () => {
          $("#erase").addEventListener("change", (e) => {
            S.eraseAll = e.target.checked;
            $("#erase-warn").hidden = !(S.wasConfigured && S.eraseAll);
          });
          $("#go").addEventListener("click", install);
          $("#keepfw")?.addEventListener("click", () => { S.installPanel = false; fwd(); });
          $("#unpick").addEventListener("click", () => {
            if (i?.configured) { S.installPanel = false; return render(); }
            S.serial?.close(); S.port = null; S.info = null; render();
          });
        },
      };
    }

    // A Sticky that's already set up: say what it is, and ask what to do with it.
    const st = i.settings || {};
    const choice = (flow, title, text, cls = "") => `<button class="choice ${cls}" data-flow="${flow}"><b>${title}</b><small>${text}</small></button>`;
    return {
      html: `
        <h2>Your Sticky</h2>
        <div class="found">
          <b>${esc(i.name)}</b>
          <span>Sticky Remote ${esc(i.fw)}</span>
          <span id="found-status">${statusBadges(i)}</span>
          <small>${esc([st.player, st.light1, st.light2].filter(Boolean).join(" · "))}</small>
        </div>
        <h3>What would you like to do?</h3>
        <div class="choices">
          ${update ? choice("update", `Update to ${esc(fw)}`, "Keeps its Wi-Fi, its settings and its Home Assistant pairing. About a minute.", "recommended") : ""}
          ${choice("settings", "Change what it controls", "Player, app button, lights, labels and icons, typeface, touch sound." + (update ? " Some options may need the update first." : ""))}
          ${choice("wifi", "Move it to another Wi-Fi", "Pick a network from the ones it sees and check that it joins.")}
          ${choice("reset", update ? "Erase and set up from scratch" : "Reinstall and set up from scratch", "Erases its Wi-Fi, settings and Home Assistant pairing, then runs the whole setup.", "quiet")}
        </div>
        ${portRow(false)}`,
      bind: () => {
        bindPort();
        document.querySelectorAll("[data-flow]").forEach((b) => b.addEventListener("click", () => startFlow(b.dataset.flow)));
        // Opening the port often restarts the Sticky, so its first answer comes
        // before it's back on Wi-Fi: keep asking while this menu is open.
        S.timers.push(setInterval(async () => {
          const n = await S.serial.info(1).catch(() => null);
          const box = $("#found-status");
          if (!box) return;
          if (n) S.info = n;
          box.innerHTML = statusBadges(S.info);
        }, 2500));
      },
    };
  },

  // 1 · Wi-Fi: pick a network the Sticky sees, and check right away that it joins.
  () => {
    const i = S.info || {}, w = S.wifi;
    const connected = !!i.wifi?.connected;
    const picking = !connected || w.change;
    return {
      html: `
        <h2>Wi-Fi</h2>
        <p class="lead">The network the Sticky joins. 2.4 GHz only: most home routers offer it, sometimes under its own name.</p>
        ${connected ? `<ul class="checks"><li class="ok">The Sticky is on <b>${esc(i.wifi.ssid || "Wi-Fi")}</b>${i.wifi.ip ? ` · ${esc(i.wifi.ip)}` : ""}.</li></ul>` : ""}
        ${picking ? `
        <div class="net-head"><b>Networks the Sticky sees</b><button class="link" id="rescan">Scan again</button></div>
        <div id="networks" class="networks"></div>
        <div class="row">
          <label class="field"><span>Network name</span><input type="text" id="ssid" value="${esc(w.ssid)}" autocomplete="off" placeholder="Pick one above, or type a hidden network's name"></label>
          <label class="field"><span>Password</span><input type="password" id="pass" value="${esc(w.pass)}" autocomplete="off">
            <small><label><input type="checkbox" id="showpass"> Show</label></small></label>
        </div>
        <div class="actions" style="margin-top:2px"><button class="primary" id="join" ${w.ssid ? "" : "disabled"}>Connect the Sticky</button>
          ${connected ? `<button class="link" id="keep-wifi">Keep ${esc(i.wifi.ssid || "the current network")}</button>` : ""}</div>
        <ul class="checks" id="wifi-checks"></ul>`
        : `<p><button class="link" id="change-wifi">Use another network</button></p>`}
        ${actions(true, connected && !w.change, S.flow === "wifi" ? "Finish" : "Continue")}`,
      bind: () => {
        bindNav();
        $("#change-wifi")?.addEventListener("click", () => { w.change = true; render(); });
        $("#keep-wifi")?.addEventListener("click", () => { w.change = false; render(); });
        if (!picking) return;
        const ready = () => { $("#join").disabled = w.busy || !w.ssid.trim(); };
        $("#ssid").addEventListener("input", (e) => { w.ssid = e.target.value; drawNetworks(); ready(); });
        $("#pass").addEventListener("input", (e) => { w.pass = e.target.value; });
        $("#pass").addEventListener("keydown", (e) => { if (e.key === "Enter" && !$("#join").disabled) joinWifi(); });
        $("#showpass").addEventListener("change", (e) => { $("#pass").type = e.target.checked ? "text" : "password"; });
        $("#join").addEventListener("click", joinWifi);
        $("#rescan").addEventListener("click", () => { S.scanStart = Date.now(); S.info && (S.info.networks = []); S.serial.scan(); drawNetworks(); });
        S.scanStart = Date.now();
        drawNetworks();
        watchNetworks();
      },
    };
  },

  // 2 · Home Assistant
  () => {
    const h = S.ha;
    const connected = !!h.client;
    return {
      html: `
        <h2>Home Assistant</h2>
        <p class="lead">So this page can list your players and lights. The token is used here only: it isn't sent to the Sticky or stored on this server.</p>
        <label class="field"><span>Home Assistant address</span>
          <input type="url" id="haurl" placeholder="https://homeassistant.example.com" value="${esc(h.url)}">
          <small>The https address you use to open Home Assistant.</small></label>
        <label class="field"><input type="checkbox" id="remember" ${h.remember ? "checked" : ""}> Remember this address in this browser
          <small>Stored on this computer only (never the token). Untick to forget it.</small></label>
        <label class="field"><span>Long-lived access token</span>
          <input type="password" id="token" value="${esc(h.token)}" autocomplete="off">
          <small>${h.url ? `<a href="${esc(h.url.replace(/\/+$/, ""))}/profile/security" target="_blank" rel="noopener">Create one in Home Assistant</a>` : "In Home Assistant: your profile › Security"}: “Create token”, from an admin account.</small></label>
        <div class="actions" style="margin-top:4px">
          <button class="primary" id="hacheck">${connected ? `<i class="ic ic-check"></i> Connected` : "Connect"}</button>
          <span id="hastatus" class="${h.error ? "note bad" : "note"}" ${h.error || connected ? "" : "hidden"}>${esc(h.error || (connected ? `Connected to ${h.location} · ${h.states.length} entities` : ""))}</span>
        </div>
        <p><button class="link" id="manual">${h.manual ? "Use Home Assistant's lists instead" : "Can't connect? Enter the entities by hand"}</button></p>
        ${actions(true, connected || h.manual, "Continue")}`,
      bind: () => {
        $("#haurl").addEventListener("input", (e) => { h.url = e.target.value.trim(); h.client = null; });
        $("#token").addEventListener("input", (e) => { h.token = e.target.value.trim(); h.client = null; });
        $("#hacheck").addEventListener("click", connectHA);
        $("#remember").addEventListener("change", (e) => {
          h.remember = e.target.checked;
          if (h.remember && h.url) localStorage.setItem("sticky.haURL", h.url);
          else localStorage.removeItem("sticky.haURL");
        });
        $("#manual").addEventListener("click", () => {
          h.manual = !h.manual; h.error = "";
          if (!h.manual) return render();
          if (!S.settings.ha_url) S.settings.ha_url = h.url.replace(/\/+$/, "");
          fwd();
        });
        bindNav(() => {
          if (!S.settings.ha_url) S.settings.ha_url = h.url.replace(/\/+$/, "");
          fwd();
        });
      },
    };
  },

  // 3 · Your remote: three short cards next to a live picture of the result.
  () => {
    const s = S.settings;
    const manual = S.ha.manual || !S.ha.client;
    const list = (domain) => S.ha.states.filter((e) => e.entity_id.startsWith(domain + "."))
      .sort((a, b) => name(a).localeCompare(name(b)));
    // An entity: a list from Home Assistant, or a text box without it.
    const pick = (key, domain, label, empty = "Choose…") => manual
      ? `<label class="field"><span>${label}</span><input type="text" data-k="${key}" placeholder="${domain}.…" value="${esc(s[key])}"></label>`
      : `<label class="field"><span>${label}</span><select data-k="${key}">
          <option value="">${empty}</option>
          ${list(domain).map((e) => `<option value="${esc(e.entity_id)}" ${e.entity_id === s[key] ? "selected" : ""}>${esc(name(e))}</option>`).join("")}
        </select>${s[key] ? `<small>${esc(s[key])}</small>` : ""}</label>`;
    const text = (key, label, ph = "") => `<label class="field"><span>${label}</span><input type="text" data-k="${key}" value="${esc(s[key])}" placeholder="${esc(ph)}"></label>`;
    const seg = (key, label, opts) => `<div class="field"><span>${label}</span><div class="seg" role="radiogroup" aria-label="${label}">
        ${opts.map(([v, l]) => `<button type="button" data-seg="${key}" data-v="${v}" role="radio" aria-checked="${s[key] === v}" class="${s[key] === v ? "on" : ""}">${l}</button>`).join("")}</div></div>`;
    const icons = (key) => `<div class="field"><span>Icon</span><div class="seg icons" role="radiogroup" aria-label="Icon">
        ${Object.entries(ICONS).map(([k, v]) => `<button type="button" data-seg="${key}" data-v="${k}" role="radio" aria-checked="${s[key] === k}" class="${s[key] === k ? "on" : ""}" title="${v}" aria-label="${v}"><canvas data-icon="${k}" width="52" height="52"></canvas></button>`).join("")}</div></div>`;
    const light = (n) => {
      const k = `light${n}`;
      return `<div class="light">
        ${pick(k, "light", n === 1 ? "Light" : "Second light")}
        ${s[k] ? `<div class="light-opts">${text(`${k}_name`, "Name on the tile")}${icons(`${k}_icon`)}</div>` : ""}
        ${n === 2 ? `<button class="link" id="drop-light">Remove the second light</button>` : ""}</div>`;
    };
    const sources = S.ha.states.find((e) => e.entity_id === s.player)?.attributes?.source_list || [];
    const two = !!s.light2 || S.twoLights;
    return {
      html: `
        <h2>Your remote</h2>
        <p class="lead">Choose what the Sticky controls. The picture shows the result as you go${S.flow === "settings" ? "" : ", and you can change it later from this page"}.</p>
        ${!S.ha.client && S.flow === "settings" ? `<p class="note">Showing what the Sticky uses now. Names, icons, typeface, sound and sleep can be changed here directly.
          To pick other entities from your lists, <button class="link" id="use-ha">connect Home Assistant</button>, or type their ids.</p>` : ""}
        ${S.flow === "settings" && S.manifest?.version && newer(S.manifest.version, S.info?.fw) && !has("touch_sound")
          ? `<p class="note warn">This Sticky runs ${esc(S.info.fw)}. Some options here (touch sound) need version ${esc(S.manifest.version)}: update it first to use them.</p>` : ""}
        <div class="with-device">
          <div class="cards">
            <section class="card">
              <h3>Player</h3>
              ${pick("player", "media_player", "Media player")}
              ${s.player ? `<div class="row">
                ${sources.length && !manual
                  ? `<label class="field"><span>App button opens</span><select data-k="app"><option value="">No app</option>
                      ${sources.map((x) => `<option ${x === s.app ? "selected" : ""}>${esc(x)}</option>`).join("")}</select></label>`
                  : text("app", "App button opens", "Infuse")}
                ${text("player_name", "Name on the power tile", "Apple TV")}
              </div>` : ""}
            </section>
            <section class="card">
              <h3>Lights</h3>
              ${light(1)}
              ${two ? light(2) : s.light1 ? `<button class="link" id="add-light">+ Add a second light</button>` : ""}
            </section>
            <section class="card">
              <h3>Look and sound</h3>
              <div class="row">
                ${text("room", "Room, shown at the top", "Living room")}
                ${seg("font_set", "Typeface", [["inter", "Inter"], ["trmnl", "TRMNL pixel"]])}
              </div>
              ${has("touch_sound") ? seg("touch_sound", "Sound when you tap", [["off", "Off"], ["quiet", "Quiet"], ["normal", "Normal"]]) : ""}
              ${has("sleep_after") ? `${seg("sleep_after", "On battery, sleeps after", [["1", "1 min"], ["3", "3 min"], ["10", "10 min"], ["30", "30 min"], ["never", "Never"]])}
                <p class="hint" id="sleep-hint"></p>` : ""}
              ${has("sleep_clock") ? `${seg("sleep_clock", "While asleep", [["on", "Clock"], ["off", "Dark screen"]])}
                <p class="hint">The clock ticks once a minute, which costs some battery. The dark screen uses the least.</p>` : ""}
            </section>
            <details><summary>Advanced</summary>
              ${pick("remote", "remote", "The player's remote entity (arrows and OK)")}
              <label class="field"><span>Address the Sticky uses to reach Home Assistant</span>
                <input type="url" data-k="ha_url" value="${esc(s.ha_url)}">
                <small>For the artwork. Your https address works; a local one (http://192.168.…:8123) avoids the round trip.</small></label>
            </details>
          </div>
          <aside class="device-col"><div class="device"><canvas id="remote-preview" width="480" height="800" aria-label="Preview of your Sticky"></canvas></div>
            <p class="device-note">Your Sticky, drawn as its firmware draws it${S.ha.client ? ", with your lights as they are now" : ""}.</p></aside>
        </div>
        <p id="remote-error" class="note bad" hidden></p>
        <ul class="checks" id="send-checks"></ul>
        ${actions(true, true, "Save to the Sticky")}`,
      bind: () => {
        const preview = () => renderRemote($("#remote-preview"), previewData());
        const sleepHint = () => {
          const h = $("#sleep-hint");
          if (h) h.textContent = s.sleep_after === "never"
            ? "It stays on until you press the AI button, so the battery drains much faster. Any button wakes it."
            : "The AI button puts it to sleep at once; any button wakes it. Plugged in, it stays awake.";
        };
        sleepHint();
        const ink = getComputedStyle(document.body).color;
        document.querySelectorAll("canvas[data-icon]").forEach((c) => drawIcon(c, c.dataset.icon, ink));
        document.querySelectorAll("[data-seg]").forEach((b) => b.addEventListener("click", () => {
          s[b.dataset.seg] = b.dataset.v;
          document.querySelectorAll(`[data-seg="${b.dataset.seg}"]`).forEach((x) => {
            x.classList.toggle("on", x === b); x.setAttribute("aria-checked", x === b);
          });
          sleepHint();
          preview();
        }));
        $("#use-ha")?.addEventListener("click", () => { S.skipped.delete(HA); go(HA); });
        $("#add-light")?.addEventListener("click", () => { S.twoLights = true; render(); });
        $("#drop-light")?.addEventListener("click", () => { S.twoLights = false; s.light2 = ""; s.light2_name = ""; render(); });
        document.querySelectorAll("[data-k]").forEach((el) => {
          el.addEventListener(el.tagName === "SELECT" ? "change" : "input", () => {
            const k = el.dataset.k;
            s[k] = el.value.trim();
            if (k === "player" && !manual) {
              // The rest follows from the player: its remote, a name, the app if it has Infuse.
              const r = "remote." + s.player.split(".")[1];
              s.remote = S.ha.states.some((e) => e.entity_id === r) ? r : s.remote;
              s.player_name = s.player ? shortName(s.player) : "";
              const src = S.ha.states.find((e) => e.entity_id === s.player)?.attributes?.source_list || [];
              if (!src.includes(s.app)) s.app = src.includes("Infuse") ? "Infuse" : "";
              s.app_label = s.app ? `Open ${s.app}` : "";
              return render();
            }
            if ((k === "light1" || k === "light2") && !manual) {
              s[k + "_name"] = s[k] ? shortName(s[k]) : "";
              if (s[k]) s[k + "_icon"] = guessIcon(s[k]);
              return render();
            }
            if (k === "app") s.app_label = s.app ? `Open ${s.app}` : "";
            preview();
          });
        });
        preview();
        bindNav(() => {
          const err = !s.player ? "Choose a media player."
            : !s.light1 ? "Choose a light."
            : s.light2 && s.light2 === s.light1 ? "The second light is the same as the first: choose another one, or remove it." : "";
          if (err) { $("#remote-error").hidden = false; $("#remote-error").textContent = err; return; }
          if (!s.app_label && s.app) s.app_label = `Open ${s.app}`;
          if (!s.player_name) s.player_name = "Player";
          if (!s.remote) s.remote = "remote." + s.player.split(".")[1];
          $("#remote-error").hidden = true;
          save();
        });
      },
    };
  },

  // 4 · Finish: add it to Home Assistant and try it, as one live checklist; then the end.
  () => {
    const i = S.info || {}, s = S.settings;
    const preview = `<aside class="device-col"><div class="device"><canvas id="final-preview" width="480" height="800" aria-label="Your Sticky"></canvas></div>
        <p class="device-note">This is your Sticky${S.ha.client ? ", live from Home Assistant" : ""}.</p></aside>`;
    const draw = () => { const c = $("#final-preview"); if (c) renderRemote(c, previewData(S.live)); };
    const again = `<div class="actions"><span class="spacer"></span><button class="primary" id="again">Set Up Another Sticky</button></div>`;
    const bindAgain = () => $("#again").addEventListener("click", () => location.reload());

    if (S.finished) {
      const after = has("sleep_after") ? s.sleep_after : "3";
      const sleeps = after === "never" ? "It stays awake until you press the AI button"
        : `On battery it sleeps after ${esc(after)} minute${after === "1" ? "" : "s"}, or when you press the AI button`;
      const text = S.flow === "update" ? `${esc(i.name)} runs Sticky Remote ${esc(i.fw)}, with its Wi-Fi, settings and Home Assistant pairing kept.`
        : S.flow === "wifi" ? `${esc(i.name)} is on <b>${esc(i.wifi?.ssid || "its new network")}</b>. Home Assistant finds it again by itself within a minute.`
        : S.flow === "settings" ? "The Sticky restarted with its new settings."
        : `${sleeps}; any button wakes it. To change what it controls, or to update it, plug it in and open this page again.`;
      const withPreview = S.flow === "setup" || S.flow === "settings";
      return {
        html: `
          <h2>${S.flow === "setup" ? "Your Sticky is ready" : "All done"}</h2>
          <div class="${withPreview ? "with-device" : ""}"><div>
            <p class="big-ok"><i class="ic ic-check"></i></p>
            <p class="lead">${text}</p>
            <p>You can unplug it.</p>
          </div>${withPreview ? preview : ""}</div>
          ${again}`,
        bind: () => { bindAgain(); draw(); S.serial?.close(); },
      };
    }

    const base = S.ha.url.replace(/\/+$/, "");
    const link = (path, label) => base ? `<p><a class="button" href="${esc(base + path)}" target="_blank" rel="noopener">${label}</a></p>` : "";
    const light = esc(s.light1_name || "first light");
    return {
      html: `
        <h2>Add it to Home Assistant</h2>
        <p class="lead">Home Assistant has found <b>${esc(i.name || "the Sticky")}</b> on your network. This page follows along and ticks each step.</p>
        <div class="with-device"><div>
          <ol class="todo">
            <li id="t-add" class="${i.ha?.connected ? "ok" : "now"}"><b>Add the Sticky</b>
              <p>In <b>Settings › Devices & services</b>, under <i>Discovered</i>, click <b>Add</b> on the ESPHome card for ${esc(i.name || "the Sticky")}. Home Assistant gives it its encryption key by itself.</p>
              ${link("/config/integrations/dashboard", "Open Devices & services")}</li>
            <li id="t-allow"><b>Let it control things</b>
              <p>Open that device's <b>Configure</b> (⚙︎) and turn on <b>Allow the device to perform Home Assistant actions</b>. Without it, the Sticky shows your lights but can't switch them.</p>
              ${link("/config/integrations/integration/esphome", "Open ESPHome devices")}</li>
            <li id="t-test"><b>Press the ${light} tile on the Sticky</b>
              <p id="t-note">${S.ha.client ? "This page watches the light and finishes by itself when it changes." : "Did the light change?"}</p>
              ${S.ha.client ? "" : `<p><button id="yes" class="primary">Yes, it changed</button></p>`}</li>
          </ol>
          <p id="t-hint" class="note" hidden>Pressed the tile and nothing happened? The switch in step 2 is the usual cause. Turn it on, then press the tile again.</p>
        </div>${preview}</div>
        <div class="actions"><button id="back">Back</button><span class="spacer"></span><button class="link" id="skip">Finish without testing</button></div>`,
      bind: () => {
        bindNav();
        draw();
        const mark = (id, cls) => { $(id).className = cls; };
        const added = () => { mark("#t-add", "ok"); if ($("#t-allow").className !== "ok") mark("#t-allow", "now"); mark("#t-test", "now"); };
        if (i.ha?.connected) added();
        $("#skip").addEventListener("click", () => finish());
        $("#yes")?.addEventListener("click", () => finish());
        if (DEMO) S.serial.pretendAdded?.();
        S.timers.push(setInterval(async () => {
          const n = await S.serial.info(1).catch(() => null);
          if (!n || S.finished || !$("#t-add")) return;   // answered after the page moved on
          S.info = n;
          if (n.ha?.connected && $("#t-add").className !== "ok") {
            added();
            setTimeout(() => { const h = $("#t-hint"); if (h) h.hidden = false; }, 45000);
          }
        }, 2500));
        if (S.ha.client && !S.watching) {
          S.watching = true;
          let first = null;
          const ids = [s.light1, s.light2, s.player].filter(Boolean);
          S.ha.client.watch(ids, (id, e) => {
            S.live[id] = e;
            draw();
            if (id !== s.light1) return;
            if (first === null) { first = e.state; return; }   // the state before the press
            if (e.state !== first && S.step === FINISH && !S.finished) finish();   // it works: actions are allowed
          }).catch(() => {});
        }
      },
    };
  },
];

// What the Sticky will show: the choices made here, with the entities' state
// (live updates first, then the states listed at connection, else examples).
function previewData(live = {}) {
  const s = S.settings;
  const state = (id) => live[id] || S.ha.states.find((e) => e.entity_id === id);
  const light = (id, label, icon, example) => {
    const e = state(id);
    return e ? { label, icon, on: e.state === "on", brightness: e.attributes?.brightness, offline: e.state === "unavailable" }
             : { label, icon, ...example };
  };
  const lights = [light(s.light1, s.light1_name || "Light", s.light1_icon, { on: false })];
  if (s.light2) lights.push(light(s.light2, s.light2_name || "Light", s.light2_icon, { on: true, brightness: 158 }));
  const now = new Date();
  return {
    set: s.font_set || "inter", room: s.room, clock: `${now.getHours()}:${String(now.getMinutes()).padStart(2, "0")}`,
    lights, player: { label: s.player_name || "Player", on: true }, app: s.app,
  };
}

function updatedSummary() {
  const i = S.info || {};
  return `
    <ul class="checks">
      <li class="ok">Updated to Sticky Remote ${esc(i.fw)} on ${esc(i.name)}.</li>
      <li class="ok">Settings kept: ${esc(i.settings?.player || "")}${i.settings?.light1 ? `, ${esc(i.settings.light1)}` : ""}${i.settings?.light2 ? `, ${esc(i.settings.light2)}` : ""}.</li>
      <li class="${i.wifi?.connected ? "ok" : "wait"}" id="upd-wifi">${i.wifi?.connected ? `Back on Wi-Fi: ${esc(i.wifi.ssid || "")} · ${esc(i.wifi.ip)}` : "Reconnecting to Wi-Fi…"}</li>
      <li class="${i.ha?.connected ? "ok" : "wait"}" id="upd-ha">${i.ha?.connected ? "Home Assistant is connected again." : "Waiting for Home Assistant to reconnect…"}</li>
    </ul>
    <p class="note">The Home Assistant pairing was kept: there's nothing to do there.</p>`;
}

// --- Wi-Fi step -------------------------------------------------------------------------
// The networks the Sticky reports, strongest first, one line per name.
function networks() {
  const best = new Map();
  for (const n of S.info?.networks || []) {
    if (n.ssid && (!best.has(n.ssid) || best.get(n.ssid).rssi < n.rssi)) best.set(n.ssid, n);
  }
  return [...best.values()].sort((a, b) => b.rssi - a.rssi);
}

function drawNetworks() {
  const box = $("#networks");
  if (!box) return;
  const nets = networks();
  if (!nets.length) {
    const long = Date.now() - (S.scanStart || 0) > 15000;
    box.innerHTML = long
      ? `<span class="hint">${has("scan") ? "No network found yet. Move the Sticky closer to the router, or type the name below." : "This firmware version can't list networks here: type the name below."}</span>`
      : `<span class="hint wait">Looking for networks…</span>`;
    return;
  }
  const bars = (rssi) => { const n = rssi > -60 ? 3 : rssi > -75 ? 2 : 1; return `<i class="bars b${n}" title="${n === 3 ? "Strong" : n === 2 ? "Fair" : "Weak"} signal"></i>`; };
  box.innerHTML = nets.map((n) => `<button data-ssid="${esc(n.ssid)}" data-open="${n.secure ? "" : "1"}" class="${n.ssid === S.wifi.ssid ? "chosen" : ""}">
      ${bars(n.rssi)}<span>${esc(n.ssid)}</span>${n.secure ? "" : "<small>open</small>"}</button>`).join("");
  box.querySelectorAll("[data-ssid]").forEach((b) => b.addEventListener("click", () => {
    S.wifi.ssid = b.dataset.ssid;
    $("#ssid").value = S.wifi.ssid;
    $("#join").disabled = S.wifi.busy;
    drawNetworks();
    if (!b.dataset.open) $("#pass").focus();
  }));
}

// Asks the Sticky to scan, then refreshes the list while this step is open.
function watchNetworks() {
  let tick = 0;
  S.serial.scan();
  S.timers.push(setInterval(async () => {
    if (S.wifi.busy) return;
    if (++tick % 5 === 0) S.serial.scan();
    const i = await S.serial.info(1).catch(() => null);
    if (!i || S.wifi.busy) return;
    const was = !!S.info?.wifi?.connected;
    S.info = i;
    if (was !== !!i.wifi?.connected) return render();   // e.g. it joined a saved network meanwhile
    drawNetworks();
  }, 3000));
}

// Sends the network to the Sticky and waits to see it join.
async function joinWifi() {
  const w = S.wifi, checks = $("#wifi-checks");
  const item = (id, text, cls = "wait") => {
    let li = document.getElementById(id);
    if (!li) { li = document.createElement("li"); li.id = id; checks.append(li); }
    li.className = cls; li.innerHTML = text;
  };
  const lock = (on) => {
    w.busy = on;
    for (const id of ["join", "ssid", "pass", "back", "rescan"]) { const el = document.getElementById(id); if (el) el.disabled = on; }
  };
  const ssid = w.ssid.trim();
  checks.innerHTML = "";
  stopTimers();
  lock(true);
  try {
    item("w1", "Sending to the Sticky…");
    await S.serial.set({ wifi: { ssid, pass: w.pass } });
    item("w1", "Saved on the Sticky.", "ok");
    item("w2", `Restarting, then joining <b>${esc(ssid)}</b>…`);
    await sleep(1500);
    let info = await S.serial.waitForBoard(25000);
    if (!info) throw new Error("The Sticky didn't come back after restarting. Check the cable and try again.");
    const end = Date.now() + 40000;
    while (!info?.wifi?.connected && Date.now() < end) { await sleep(2000); info = await S.serial.info(1) || info; }
    S.info = info;
    if (!info.wifi?.connected) {
      item("w2", `The Sticky couldn't join <b>${esc(ssid)}</b>. Check the password (capitals count), and that this is a 2.4 GHz network. Then connect again.`, "bad");
      lock(false);
      S.scanStart = Date.now();
      return watchNetworks();
    }
    w.busy = false; w.change = false; w.pass = "";
    fwd();   // joined: straight to the next step
  } catch (e) {
    item("w9", esc(String(e?.message || e)), "bad");
    lock(false);
  }
}

// Wi-Fi and Home Assistant badges for a set-up Sticky. For a minute after the
// port opened (which may have restarted it), "not yet" reads as "connecting".
function statusBadges(i) {
  const fresh = Date.now() - (S.connectedAt || 0) < 60000;
  const badge = (cls, text) => `<span class="${cls}">${text}</span>`;
  const wifi = i?.wifi?.connected ? badge("good", `Wi-Fi: ${esc(i.wifi.ssid || "connected")}`)
    : fresh ? badge("wait", "Joining Wi-Fi…") : badge("off", "Not on Wi-Fi");
  const ha = i?.ha?.connected ? badge("good", "Home Assistant connected")
    : !i?.ha?.paired ? badge("off", "Not added to Home Assistant")
    : fresh || !i?.wifi?.connected ? badge("wait", "Connecting to Home Assistant…") : badge("off", "Home Assistant not connected");
  return wifi + ha;
}

// A set-up Sticky was plugged in: which steps does this visit need?
function startFlow(flow) {
  S.skipped.clear();
  S.updated = false; S.finished = false;
  S.wifi.change = false;
  if (flow === "update" || flow === "reset") {
    S.flow = flow === "update" ? "update" : "setup";
    S.eraseAll = flow === "reset";
    S.installPanel = true;
    return render();
  }
  S.flow = flow;
  S.installPanel = false;
  if (flow === "wifi") { S.skipped.add(HA); S.skipped.add(REMOTE); S.wifi.change = true; return go(WIFI); }
  // Settings: straight to Your remote, with what the Sticky has now. Home
  // Assistant is optional there (to pick from its lists), unless already connected.
  S.skipped.add(WIFI);
  S.skipped.add(HA);
  go(REMOTE);
}

// The end of a visit: the last step, showing its summary.
function finish() {
  stopTimers();
  S.finished = true;
  for (let n = S.step + 1; n < FINISH; n++) S.skipped.add(n);
  S.step = FINISH;
  render();
  window.scrollTo({ top: 0, behavior: "smooth" });
}

// A likely icon from the light's name.
function guessIcon(id) {
  const t = (shortName(id) + " " + id).toLowerCase();
  if (/ceil|plafon|lustre|luminaire|suspen|pendant/.test(t)) return "ceiling";
  if (/floor|lampadaire|stand|pied/.test(t)) return "floor";
  if (/star|etoile|étoile|deco|guirl|fairy|xmas|noel|noël/.test(t)) return "star";
  return "bulb";
}

// --- Actions ----------------------------------------------------------------------------
const name = (e) => e.attributes?.friendly_name || e.entity_id;
const shortName = (id) => {
  const e = S.ha.states.find((x) => x.entity_id === id);
  return e ? name(e) : id.split(".")[1]?.replace(/_/g, " ") || "";
};

async function connect() {
  const status = $("#pick-status");
  try {
    await S.serial?.close();
    if (DEMO) {
      S.port = { demo: true };
      S.serial = new DemoSticky({ installed: DEMO_MODE === "update", configured: DEMO_MODE === "update" });
    } else {
      const filters = $("#allports").checked ? [] : [{ usbVendorId: 0x1a86 }, { usbVendorId: 0x303a }];
      S.port = await navigator.serial.requestPort({ filters });
      S.serial = new StickySerial(S.port);
      await S.serial.open();
    }
    status.hidden = false; status.className = "note"; status.textContent = "Asking the Sticky…";
    S.info = await S.serial.info(3);
    S.connectedAt = Date.now();
    const i = S.info;
    S.wasConfigured = !!i?.configured;
    S.eraseAll = !i;
    S.updated = false; S.finished = false;
    S.flow = "setup"; S.skipped.clear(); S.wifi.change = false;
    if (i?.settings) Object.assign(S.settings, Object.fromEntries(Object.entries(i.settings).filter(([, v]) => v !== "")));
    // No Sticky Remote yet, or an older one that isn't set up: the installer.
    // Up to date and not set up: straight on. Set up: its menu.
    S.installPanel = !i || (!i.configured && newer(S.manifest?.version, i.fw));
    if (i && !i.configured && !S.installPanel) return fwd();
    render();
  } catch (e) {
    if (e?.name === "NotFoundError") return; // the user closed the port picker
    status.hidden = false; status.className = "note bad";
    status.textContent = e?.message?.includes("open") ? "That port is busy: close other apps using it (Sticky Simulator, a serial monitor) and try again." : String(e?.message || e);
  }
}

async function install() {
  const bar = $("#bar"), phase = $("#phase"), log = $("#log");
  const addLog = (l) => { log.hidden = false; log.textContent += l.trimEnd() + "\n"; log.scrollTop = log.scrollHeight; };
  const lock = (on) => { for (const id of ["go", "unpick", "keepfw", "erase"]) { const el = document.getElementById(id); if (el) el.disabled = on; } };
  lock(true);
  try {
    phase.className = "note";
    phase.textContent = "Downloading the firmware…";
    const data = DEMO ? null : new Uint8Array(await (await fetch(`firmware/${S.manifest.file}`)).arrayBuffer());
    if (data && S.manifest.size && data.length !== S.manifest.size) throw new Error("The firmware download is incomplete. Reload the page and try again.");
    phase.textContent = "Installing… keep the cable plugged in.";
    await S.serial.close();
    const opts = { eraseAll: S.eraseAll, onProgress: (p) => { bar.style.width = `${Math.round(p * 100)}%`; }, onLog: addLog };
    if (DEMO) await demoFlash(opts); else await flash(S.port, data, opts);
    bar.style.width = "100%";
    phase.textContent = "Installed. Waiting for the Sticky to start…";
    if (DEMO) { S.serial.finishFlash(S.manifest.version, S.eraseAll); }
    else { S.serial = new StickySerial(S.port); await S.serial.open(); }
    S.info = await S.serial.waitForBoard(30000);
    if (!S.info) throw new Error("The Sticky didn't answer after installing. Unplug it, plug it back in, and connect it again.");
    S.installPanel = false;
    // An update that kept everything: nothing else to do. Otherwise, on with the setup.
    S.updated = S.wasConfigured && !S.eraseAll && !!S.info.configured;
    if (S.updated) return render();
    S.flow = "setup"; S.skipped.clear();
    fwd();
  } catch (e) {
    phase.className = "note bad";
    phase.textContent = String(e?.message || e);
    lock(false);
  }
}

async function connectHA() {
  const h = S.ha;
  h.error = "";
  const problem = DEMO_HA ? null : HAClient.problem(h.url);
  if (problem) { h.error = problem; return render(); }
  if (!h.token && !DEMO_HA) { h.error = "Paste a long-lived access token."; return render(); }
  $("#hacheck").disabled = true; $("#hacheck").textContent = "Connecting…";
  try {
    h.client?.close();
    const c = DEMO_HA ? new DemoHA() : new HAClient(h.url, h.token);
    await c.connect();
    const [states, cfg] = await Promise.all([c.states(), c.config()]);
    h.client = c; h.states = states; h.location = cfg.location_name || "Home Assistant";
    h.manual = false;
    if (DEMO_HA && !h.url) h.url = "https://demo.home-assistant.invalid";
    if (h.remember && !DEMO_HA) localStorage.setItem("sticky.haURL", h.url);
    if (!S.settings.ha_url) S.settings.ha_url = h.url.replace(/\/+$/, "");
    return fwd();   // connected: straight to the next step
  } catch (e) {
    h.client = null;
    h.error = String(e?.message || e);
  }
  render();
}

// Saves the choices on the Sticky (from Your remote) and moves on once it's back.
async function save() {
  const checks = $("#send-checks");
  const item = (id, text, cls = "wait") => {
    let li = document.getElementById(id);
    if (!li) { li = document.createElement("li"); li.id = id; checks.append(li); }
    li.className = cls; li.innerHTML = text;
  };
  const lock = (on) => { $("#next").disabled = on; $("#back").disabled = on; };
  checks.innerHTML = "";
  lock(true);
  try {
    item("c1", "Saving on the Sticky…");
    await S.serial.set({ settings: S.settings });
    item("c1", "Saved on the Sticky.", "ok");
    item("c2", "Restarting with the new settings…");
    await sleep(1500);
    let info = await S.serial.waitForBoard(25000);
    if (!info) throw new Error("The Sticky didn't come back after restarting. Check the cable, then save again.");
    const end = Date.now() + 45000;
    while (!info?.wifi?.connected && Date.now() < end) { await sleep(2000); info = await S.serial.info(1) || info; }
    S.info = info;
    if (!info.wifi?.connected) throw new Error("Saved, but the Sticky isn't back on Wi-Fi yet. Go back to the Wi-Fi step to check its network.");
    // Already added to Home Assistant (a settings change): that's all. Otherwise, the last step.
    if (info.ha?.paired) return finish();
    go(FINISH);
  } catch (e) {
    item("c9", esc(String(e?.message || e)), "bad");
    lock(false);
  }
}

// --- Start ----------------------------------------------------------------------------
(async () => {
  if (DEMO) {
    $("#demo-badge").hidden = false;
    if (!DEMO_HA) $("#demo-badge").textContent = "Demo Sticky · real Home Assistant";
  }
  try {
    S.manifest = await (await fetch("firmware/manifest.json", { cache: "no-store" })).json();
    $("#fw-line").textContent = `Sticky Remote ${S.manifest.version}`;
  } catch {
    S.manifest = { version: "?" };
    $("#fw-line").textContent = "Firmware manifest missing";
  }
  render();
})();
