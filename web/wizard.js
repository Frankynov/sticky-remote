// Sticky Remote setup wizard: install the firmware, set Wi-Fi, pick the player
// and lights from Home Assistant, send it all over USB, add the Sticky to HA.
// Static page, no build step. ?demo runs it against a pretend Sticky and HA.

import { StickySerial, flash, DemoSticky, demoFlash } from "./sticky-serial.js";
import { HAClient, DemoHA } from "./ha.js";
import { renderRemote, renderImage } from "./device-preview.js";

// ?demo: pretend Sticky and pretend Home Assistant. ?demo=sticky: pretend
// Sticky with your real Home Assistant (to try the HA steps before a Sticky arrives).
const DEMO = new URLSearchParams(location.search).has("demo");
const DEMO_MODE = new URLSearchParams(location.search).get("demo");
const DEMO_HA = DEMO && DEMO_MODE !== "sticky";
// ?demo=update: the demo Sticky already runs an older, set-up Sticky Remote.
const STEPS = ["Welcome", "Connect", "Install", "Wi-Fi", "Home Assistant", "Your remote", "Send", "Add to Home Assistant", "Test", "Done"];
const DONE = STEPS.length - 1;
const ICONS = { bulb: "Bulb", ceiling: "Ceiling light", floor: "Floor lamp", star: "Star" };

const S = {
  step: 0,
  manifest: null,
  port: null, serial: null, info: null,
  install: true, eraseAll: true, installed: false,
  wifi: { ssid: "", pass: "", keep: false },
  ha: { url: localStorage.getItem("sticky.haURL") || "", remember: !!localStorage.getItem("sticky.haURL"),
        token: "", client: null, states: [], manual: false, location: "" },
  settings: {
    player: "", remote: "", player_name: "", app: "", app_label: "",
    light1: "", light1_name: "", light1_icon: "bulb", light2: "", light2_name: "", light2_icon: "bulb",
    room: "", ha_url: "", font_set: "inter",
  },
  timers: [],
  skipped: new Set(),   // steps this flow didn't need (shown with a dash)
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

function stopTimers() { S.timers.forEach(clearInterval); S.timers = []; }
function go(step) { stopTimers(); S.step = step; render(); window.scrollTo({ top: 0, behavior: "smooth" }); }

// --- Rendering -----------------------------------------------------------------------
function render() {
  $("#step-list").innerHTML = STEPS.map((t, i) => {
    const cls = i === S.step ? "current" : S.skipped.has(i) ? "skipped" : i < S.step ? "done" : "";
    return `<li class="${cls}"${cls === "skipped" ? ' title="Not needed this time"' : ""}>${esc(t)}</li>`;
  }).join("");
  const view = VIEWS[S.step]();
  $("#panel").innerHTML = view.html;
  view.bind?.();
}

const actions = (back, next, nextLabel = "Continue", nextDisabled = false) => `
  <div class="actions">
    ${back ? `<button id="back">Back</button>` : ""}
    <span class="spacer"></span>
    ${next ? `<button class="primary" id="next" ${nextDisabled ? "disabled" : ""}>${esc(nextLabel)}</button>` : ""}
  </div>`;
function bindNav(onNext) {
  $("#back")?.addEventListener("click", () => go(S.step - 1));
  $("#next")?.addEventListener("click", onNext || (() => go(S.step + 1)));
}

const VIEWS = [
  // 0 · Welcome
  () => {
    const ok = DEMO || "serial" in navigator;
    return {
      html: `
        <div class="hero">
          <figure><div class="device"><canvas id="hero-now" width="480" height="800" aria-label="The Sticky playing a film"></canvas></div><figcaption>Now playing</figcaption></figure>
          <figure><div class="device"><canvas id="hero-remote" width="480" height="800" aria-label="The Sticky's remote page"></canvas></div><figcaption>Remote</figcaption></figure>
        </div>
        <h2>Set up your Sticky</h2>
        <p class="lead">About five minutes. This page installs Sticky Remote on a Seeed reTerminal Sticky, connects it to your Wi-Fi and lets you choose what it controls in Home Assistant.</p>
        <h3>You'll need</h3>
        <ul>
          <li>The Sticky and a USB-C <b>data</b> cable (some cables only charge).</li>
          <li>Chrome or Edge on a computer: they can talk to USB devices.</li>
          <li>Home Assistant, reachable over <b>https</b>, and a long-lived access token from an admin account (your profile › Security).</li>
          <li>The Wi-Fi network for the Sticky (2.4 GHz).</li>
        </ul>
        <h3>Your data stays yours</h3>
        <ul class="privacy-list">
          <li><b>Wi-Fi password</b>: sent only to your Sticky, over the USB cable.</li>
          <li><b>Home Assistant token</b>: sent only to your Home Assistant, kept in memory and forgotten when you close this tab.</li>
          <li><b>Home Assistant address</b>: remembered in this browser only if you want, and you can forget it.</li>
          <li><b>This website</b> only serves the page and the firmware file. Nothing you type is sent to it or to its author. No analytics, no cookies, no third-party scripts: the page's security policy makes the browser block anything else.</li>
        </ul>
        ${ok ? "" : `<p class="note bad">This browser can't reach USB devices. Open this page in Chrome or Edge on a computer.</p>`}
        <div class="actions">
          ${DEMO ? "" : `<a class="button" href="?demo">Try the demo (no Sticky needed)</a>`}
          <span class="spacer"></span>
          <button class="primary" id="next" ${ok ? "" : "disabled"}>Start</button>
        </div>`,
      bind: () => {
        bindNav();
        renderImage($("#hero-now"), "welcome-now");
        renderImage($("#hero-remote"), "welcome-remote");
      },
    };
  },

  // 1 · Connect
  () => {
    const i = S.info;
    const fw = S.manifest?.version;
    let status = "";
    if (S.port && i) {
      const update = fw && newer(fw, i.fw);
      status = `<p class="note">Found <b>${esc(i.name)}</b> running Sticky Remote ${esc(i.fw)}${i.configured ? ", already set up" : ""}.
        ${update ? ` Version ${esc(fw)} is available.` : ""}
        ${update && i.configured ? " Updating keeps its Wi-Fi, its settings and its Home Assistant pairing: it's done in a minute, no other step." : ""}</p>
        <label class="field"><input type="radio" name="inst" value="keep" ${!S.install ? "checked" : ""}> Keep the installed firmware ${esc(i.fw)} and only change the settings</label>
        <label class="field"><input type="radio" name="inst" value="install" ${S.install ? "checked" : ""}> ${update ? "Update to" : "Reinstall"} ${esc(fw)}</label>`;
    } else if (S.port) {
      status = `<p class="note">Connected. This Sticky doesn't run Sticky Remote yet (or didn't answer): the next step installs it.</p>`;
    }
    return {
      html: `
        <h2>Connect the Sticky</h2>
        <p class="lead">Plug the Sticky into this computer with its USB-C cable, then choose its port. It's usually called “USB Single Serial” or “wchusbserial”.</p>
        <div class="actions" style="margin-top:0">
          <button class="primary" id="pick">${S.port ? "Choose Another Port" : "Choose the Sticky's Port"}</button>
          <label><input type="checkbox" id="allports"> Show every port</label>
        </div>
        <p id="pick-status" class="note" hidden></p>
        ${status}
        ${actions(true, !!S.port, "Continue")}`,
      bind: () => {
        $("#pick").addEventListener("click", connect);
        document.querySelectorAll("input[name=inst]").forEach((r) =>
          r.addEventListener("change", () => { S.install = r.value === "install"; }));
        bindNav(() => {
          if (S.install) S.skipped.delete(2); else S.skipped.add(2);
          go(S.install ? 2 : 3);
        });
      },
    };
  },

  // 2 · Install
  () => ({
    html: `
      <h2>${S.updated ? "Updated" : "Install the firmware"}</h2>
      <p class="lead">${S.updated ? "Its Wi-Fi, settings and Home Assistant pairing were kept."
        : `Sticky Remote ${esc(S.manifest?.version)} replaces the firmware on the Sticky. Keep the cable plugged in until it's done (about a minute).`}</p>
      ${S.updated ? updatedSummary() : `
      <label class="field"><input type="checkbox" id="erase" ${S.eraseAll ? "checked" : ""}> Erase everything first
        <small>${S.wasConfigured
          ? "Not needed for an update. Erasing removes its Wi-Fi, its settings and the key Home Assistant gave it: you'd go through the whole setup and add it to Home Assistant again."
          : `Recommended the first time. Clears the old firmware's data${S.info ? ", and this Sticky's saved Wi-Fi and settings" : ""}.`}</small></label>
      <p id="erase-warn" class="note warn" ${S.wasConfigured && S.eraseAll ? "" : "hidden"}>This Sticky is set up: erasing means setting it up again from scratch.</p>
      <div class="progress"><div id="bar" style="width:${S.installed ? 100 : 0}%"></div></div>
      <p id="phase" class="note">${S.installed ? "Installed." : "Ready."}</p>
      <div id="log" class="log" hidden></div>`}
      <div class="actions">
        ${S.updated ? `<button id="change">Change Its Settings Too</button><span class="spacer"></span><button class="primary" id="finish">Done</button>`
          : `<button id="back">Back</button><span class="spacer"></span>
             ${S.installed ? `<button class="primary" id="next">Continue</button>` : `<button class="primary" id="go">Install</button>`}`}
      </div>`,
    bind: () => {
      $("#erase")?.addEventListener("change", (e) => {
        S.eraseAll = e.target.checked;
        $("#erase-warn").hidden = !(S.wasConfigured && S.eraseAll);
      });
      $("#go")?.addEventListener("click", install);
      $("#change")?.addEventListener("click", () => { S.updated = false; S.wifi.keep = true; go(3); });
      if (S.updated && !(S.info?.wifi?.connected && S.info?.ha?.connected)) {
        S.timers.push(setInterval(async () => {
          const i = await S.serial.info(1).catch(() => null);
          if (!i) return;
          S.info = i;
          stopTimers();   // the re-render starts a new check while something is still missing
          render();
        }, 3000));
      }
      $("#finish")?.addEventListener("click", () => {
        stopTimers(); S.serial?.close();
        for (let i = 3; i < DONE; i++) S.skipped.add(i);   // nothing else was needed
        go(DONE);
      });
      bindNav(() => go(1 + 2));
    },
  }),

  // 3 · Wi-Fi
  () => {
    const i = S.info || {};
    const nets = (i.networks || []).filter((n) => n.ssid).sort((a, b) => b.rssi - a.rssi);
    const connected = i.wifi?.connected;
    return {
      html: `
        <h2>Wi-Fi</h2>
        <p class="lead">The network the Sticky joins. 2.4 GHz only: most home routers offer it, sometimes under its own name.</p>
        ${connected ? `<label class="field"><input type="checkbox" id="keep" ${S.wifi.keep ? "checked" : ""}> Keep its current network, <b>${esc(i.wifi.ssid || "")}</b></label>` : ""}
        <div id="wifi-fields" ${S.wifi.keep ? "hidden" : ""}>
          ${nets.length ? `<div class="networks">${nets.map((n) =>
            `<button data-ssid="${esc(n.ssid)}" class="${n.ssid === S.wifi.ssid ? "chosen" : ""}">${esc(n.ssid)}${n.secure ? " 🔒" : ""}</button>`).join("")}</div>
            <small style="color:var(--mute)">Networks the Sticky can see.</small>` : ""}
          <div class="row">
            <label class="field"><span>Network name</span><input type="text" id="ssid" value="${esc(S.wifi.ssid)}" autocomplete="off"></label>
            <label class="field"><span>Password</span><input type="password" id="pass" value="${esc(S.wifi.pass)}" autocomplete="off"></label>
          </div>
        </div>
        ${actions(true, true, "Continue", !S.wifi.keep && !S.wifi.ssid)}`,
      bind: () => {
        const update = () => { $("#next").disabled = !S.wifi.keep && !S.wifi.ssid.trim(); };
        $("#keep")?.addEventListener("change", (e) => { S.wifi.keep = e.target.checked; $("#wifi-fields").hidden = S.wifi.keep; update(); });
        $("#ssid").addEventListener("input", (e) => { S.wifi.ssid = e.target.value; update(); });
        $("#pass").addEventListener("input", (e) => { S.wifi.pass = e.target.value; });
        document.querySelectorAll("[data-ssid]").forEach((b) => b.addEventListener("click", () => {
          S.wifi.ssid = b.dataset.ssid; $("#ssid").value = S.wifi.ssid;
          document.querySelectorAll("[data-ssid]").forEach((x) => x.classList.toggle("chosen", x === b));
          $("#pass").focus(); update();
        }));
        bindNav(() => go(S.install ? 4 : 4));
      },
    };
  },

  // 4 · Home Assistant
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
          <button class="primary" id="hacheck">${connected ? "Connected ✓" : "Connect"}</button>
          <span id="hastatus" class="${h.error ? "note bad" : "note"}" ${h.error || connected ? "" : "hidden"}>${esc(h.error || (connected ? `Connected to ${h.location} · ${h.states.length} entities` : ""))}</span>
        </div>
        <p><button class="link" id="manual">${h.manual ? "Use Home Assistant's lists instead" : "Can't connect? Enter the entities by hand"}</button></p>
        ${actions(true, true, "Continue", !connected && !h.manual)}`,
      bind: () => {
        $("#haurl").addEventListener("input", (e) => { h.url = e.target.value.trim(); h.client = null; });
        $("#token").addEventListener("input", (e) => { h.token = e.target.value.trim(); h.client = null; });
        $("#hacheck").addEventListener("click", connectHA);
        $("#remember").addEventListener("change", (e) => {
          h.remember = e.target.checked;
          if (h.remember && h.url) localStorage.setItem("sticky.haURL", h.url);
          else localStorage.removeItem("sticky.haURL");
        });
        $("#manual").addEventListener("click", () => { h.manual = !h.manual; h.error = ""; render(); });
        bindNav(() => {
          if (!S.settings.ha_url) S.settings.ha_url = h.url.replace(/\/+$/, "");
          go(5);
        });
      },
    };
  },

  // 5 · Your remote
  () => {
    const s = S.settings;
    const list = (domain) => S.ha.states.filter((e) => e.entity_id.startsWith(domain + "."))
      .sort((a, b) => name(a).localeCompare(name(b)));
    const manual = S.ha.manual || !S.ha.client;
    const pick = (key, domain, label, optional) => manual
      ? `<label class="field"><span>${label}</span><input type="text" data-k="${key}" placeholder="${optional ? "none" : domain + ".…"}" value="${esc(s[key])}"></label>`
      : `<label class="field"><span>${label}</span><select data-k="${key}">
          ${optional ? `<option value="">None: one light tile</option>` : `<option value="">Choose…</option>`}
          ${list(domain).map((e) => `<option value="${esc(e.entity_id)}" ${e.entity_id === s[key] ? "selected" : ""}>${esc(name(e))} · ${esc(e.entity_id)}</option>`).join("")}
        </select></label>`;
    const text = (key, label, ph = "") => `<label class="field"><span>${label}</span><input type="text" data-k="${key}" value="${esc(s[key])}" placeholder="${esc(ph)}"></label>`;
    const icon = (key) => `<label class="field"><span>Icon</span><select data-k="${key}">
        ${Object.entries(ICONS).map(([k, v]) => `<option value="${k}" ${s[key] === k ? "selected" : ""}>${v}</option>`).join("")}</select></label>`;
    const sources = S.ha.states.find((e) => e.entity_id === s.player)?.attributes?.source_list || [];
    return {
      html: `
        <h2>Your remote</h2>
        <p class="lead">What this Sticky controls. You can change it later by running this page again.</p>
        <div class="with-device"><div>
        <h3>Player</h3>
        <div class="row">${pick("player", "media_player", "Media player")}${pick("remote", "remote", "Its remote (arrows and OK)")}</div>
        <div class="row">
          ${sources.length && !manual
            ? `<label class="field"><span>App button opens</span><select data-k="app"><option value="">No app button</option>
                ${sources.map((x) => `<option ${x === s.app ? "selected" : ""}>${esc(x)}</option>`).join("")}</select></label>`
            : text("app", "App button opens", "Infuse")}
          ${text("player_name", "Power tile label", "Apple TV")}
        </div>
        <h3>Lights (top row)</h3>
        <div class="row3">${pick("light1", "light", "Light 1")}${text("light1_name", "Label")}${icon("light1_icon")}</div>
        <div class="row3">${pick("light2", "light", "Light 2", true)}${s.light2 ? text("light2_name", "Label") + icon("light2_icon") : ""}</div>
        <h3>Look</h3>
        <div class="row">
          ${text("room", "Room (shown at the top)", "Living room")}
          <label class="field"><span>Typeface</span><select data-k="font_set">
            <option value="inter" ${s.font_set === "inter" ? "selected" : ""}>Inter Bold</option>
            <option value="trmnl" ${s.font_set === "trmnl" ? "selected" : ""}>TRMNL (e-paper pixel fonts)</option></select></label>
        </div>
        <details><summary>Advanced</summary>
          <label class="field"><span>Address the Sticky uses to reach Home Assistant</span>
            <input type="url" data-k="ha_url" value="${esc(s.ha_url)}">
            <small>For the artwork. Your https address works; a local one (http://192.168.…:8123) avoids the round trip.</small></label>
        </details>
        </div>
        <aside class="device-col"><div class="device"><canvas id="remote-preview" width="480" height="800" aria-label="Preview of your Sticky"></canvas></div>
          <p class="device-note">Drawn by the firmware's own rules, with ${S.ha.client ? "your lights' current state" : "example states"}.</p></aside>
        </div>
        <p id="remote-error" class="note bad" hidden></p>
        ${actions(true, true, "Continue")}`,
      bind: () => {
        const preview = () => renderRemote($("#remote-preview"), previewData());
        document.querySelectorAll("[data-k]").forEach((el) => {
          el.addEventListener(el.tagName === "SELECT" ? "change" : "input", () => {
            const k = el.dataset.k;
            s[k] = el.value.trim();
            if (k === "player" && !manual) {
              const r = "remote." + s.player.split(".")[1];
              if (S.ha.states.some((e) => e.entity_id === r)) s.remote = r;
              if (!s.player_name) s.player_name = shortName(s.player);
              const src = S.ha.states.find((e) => e.entity_id === s.player)?.attributes?.source_list || [];
              if (!s.app && src.includes("Infuse")) s.app = "Infuse";
              return render();
            }
            if ((k === "light1" || k === "light2") && !manual) {
              s[k + "_name"] = s[k] ? shortName(s[k]) : "";
              return render();
            }
            if (k === "app") s.app_label = s.app ? `Open ${s.app}` : "";
            preview();
          });
        });
        preview();
        bindNav(() => {
          const err = !s.player ? "Choose a media player."
            : !s.light1 ? "Choose at least one light."
            : s.light2 && s.light2 === s.light1 ? "Light 2 is the same as Light 1: choose another light, or None." : "";
          if (err) { $("#remote-error").hidden = false; $("#remote-error").textContent = err; return; }
          if (!s.app_label && s.app) s.app_label = `Open ${s.app}`;
          if (!s.player_name) s.player_name = "Player";
          go(6);
        });
      },
    };
  },

  // 6 · Send
  () => {
    const s = S.settings;
    return {
      html: `
        <h2>Send to the Sticky</h2>
        <p class="lead">Everything goes over the cable. The Sticky saves it, restarts and joins Wi-Fi.</p>
        <dl class="kv">
          <dt>Wi-Fi</dt><dd>${S.wifi.keep ? "unchanged" : esc(S.wifi.ssid)}</dd>
          <dt>Player</dt><dd>${esc(s.player)}${s.app ? ` · opens ${esc(s.app)}` : ""}</dd>
          <dt>Lights</dt><dd>${esc(s.light1_name)} (${esc(s.light1)})${s.light2 ? `, ${esc(s.light2_name)} (${esc(s.light2)})` : ""}</dd>
          <dt>Room</dt><dd>${esc(s.room || "–")} · ${s.font_set === "trmnl" ? "TRMNL" : "Inter"}</dd>
        </dl>
        <ul class="checks" id="send-checks"></ul>
        <p id="send-error" class="note bad" hidden></p>
        <div class="actions"><button id="back">Back</button><span class="spacer"></span>
          <button class="primary" id="go">Send</button><button class="primary" id="next" hidden>Continue</button></div>`,
      bind: () => { bindNav(); $("#go").addEventListener("click", send); },
    };
  },

  // 7 · Add to Home Assistant
  () => {
    const base = S.ha.url.replace(/\/+$/, "");
    const link = (path, label) => base ? `<a class="button" href="${esc(base + path)}" target="_blank" rel="noopener">${label}</a>` : "";
    return {
      html: `
        <h2>Add it to Home Assistant</h2>
        <p class="lead">Home Assistant has found <b>${esc(S.info?.name || "the Sticky")}</b> on your network. Two clicks there:</p>
        <ol>
          <li><b>Settings › Devices & services</b>: under <i>Discovered</i>, click <b>Add</b> on the ESPHome card for ${esc(S.info?.name || "the Sticky")}. Home Assistant gives it its encryption key by itself.</li>
          <li>Then open that device's <b>Configure</b> (⚙︎) and turn on <b>Allow the device to perform Home Assistant actions</b>. Without it, the Sticky can show your lights but can't switch them.</li>
        </ol>
        <div class="actions" style="margin-top:4px">${link("/config/integrations/dashboard", "Open Devices & services")}${link("/config/integrations/integration/esphome", "Open ESPHome devices")}</div>
        <ul class="checks">
          <li id="chk-added" class="wait">Waiting for Home Assistant to connect to the Sticky…</li>
          <li id="chk-actions"><label><input type="checkbox" id="actions-ok"> I turned on “Allow the device to perform Home Assistant actions”</label></li>
        </ul>
        ${actions(true, true, "Continue", true)}`,
      bind: () => {
        bindNav(() => go(8));
        let added = false;
        const update = () => { $("#next").disabled = !(added && $("#actions-ok").checked); };
        $("#actions-ok").addEventListener("change", update);
        if (DEMO) S.serial.pretendAdded?.();
        S.timers.push(setInterval(async () => {
          const i = await S.serial.info(1).catch(() => null);
          if (!i) return;
          S.info = i;
          if (i.ha?.connected && !added) {
            added = true;
            $("#chk-added").className = "ok";
            $("#chk-added").textContent = "Home Assistant is connected to the Sticky.";
            update();
          }
        }, 2500));
      },
    };
  },

  // 8 · Test
  () => ({
    html: `
      <h2>Try it</h2>
      <div class="with-device"><div>
      <p class="lead">Press the <b>${esc(S.settings.light1_name || "first light")}</b> tile on the Sticky.</p>
      <ul class="checks"><li id="chk-light" class="wait">${S.ha.client ? "Watching the light in Home Assistant…" : "Did the light change?"}</li></ul>
      </div>
      <aside class="device-col"><div class="device"><canvas id="final-preview" width="480" height="800" aria-label="Your Sticky"></canvas></div>
        <p class="device-note">This is your Sticky${S.ha.client ? ", live from Home Assistant" : ""}.</p></aside>
      </div>
      <div class="actions"><button id="back">Back</button><span class="spacer"></span>
        ${S.ha.client ? "" : `<button id="no">No</button><button class="primary" id="yes">Yes, it changed</button>`}
        <button class="primary" id="next" hidden>Finish</button></div>`,
    bind: () => {
      bindNav();
      const ok = () => {
        if (S.step !== DONE - 1) return;
        $("#chk-light").className = "ok";
        $("#chk-light").textContent = `${S.settings.light1_name || "The light"} changed: the Sticky works.`;
        $("#next").hidden = false;
        $("#yes")?.remove(); $("#no")?.remove();
        S.serial?.close();
      };
      $("#yes")?.addEventListener("click", ok);
      $("#no")?.addEventListener("click", () => {
        $("#chk-light").className = "bad";
        $("#chk-light").innerHTML = `Nothing happened. Check that <b>Allow the device to perform Home Assistant actions</b> is on for this Sticky in Home Assistant, then press the tile again.`;
      });
      // Redraws whichever "Your Sticky" preview is on screen (here, then on Done).
      const draw = () => { const c = $("#final-preview"); if (c) renderRemote(c, previewData(S.live)); };
      draw();
      if (S.ha.client && !S.watching) {
        S.watching = true;
        let first = null;
        const ids = [S.settings.light1, S.settings.light2, S.settings.player].filter(Boolean);
        S.ha.client.watch(ids, (id, e) => {
          S.live[id] = e;
          draw();
          if (id !== S.settings.light1) return;
          if (first === null) { first = e.state; return; }   // the state before the press
          if (e.state !== first) ok();
        }).catch(() => {});
      }
    },
  }),

  // 9 · Done
  () => {
    const i = S.info || {};
    if (S.updated) return {
      html: `
        <h2>All done</h2>
        <p class="lead">${esc(i.name)} runs Sticky Remote ${esc(i.fw)}, with its Wi-Fi, settings and Home Assistant pairing kept.</p>
        <p class="big-ok">✓</p>
        <p>You can unplug it.</p>
        <div class="actions"><span class="spacer"></span><button class="primary" id="again">Set Up Another Sticky</button></div>`,
      bind: () => $("#again").addEventListener("click", () => location.reload()),
    };
    return {
      html: `
        <h2>Your Sticky is ready</h2>
        <div class="with-device"><div>
          <p class="big-ok">✓</p>
          <p>You can unplug it. On battery it sleeps after 3 minutes; any button wakes it.</p>
          <p>To change what it controls, or to update it later, plug it in and open this page again: an update keeps everything.</p>
        </div>
        <aside class="device-col"><div class="device"><canvas id="final-preview" width="480" height="800" aria-label="Your Sticky"></canvas></div>
          <p class="device-note">This is your Sticky${S.ha.client ? ", live from Home Assistant" : ""}.</p></aside>
        </div>
        <div class="actions"><span class="spacer"></span><button class="primary" id="again">Set Up Another Sticky</button></div>`,
      bind: () => {
        $("#again").addEventListener("click", () => location.reload());
        renderRemote($("#final-preview"), previewData(S.live));
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
    S.install = !S.info || newer(S.manifest?.version, S.info.fw);
    S.eraseAll = !S.info;
    S.wasConfigured = !!S.info?.configured;
    S.updated = false;
    if (S.info?.settings) Object.assign(S.settings, Object.fromEntries(
      Object.entries(S.info.settings).filter(([, v]) => v !== "")));
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
  $("#go").disabled = true; $("#back").disabled = true;
  try {
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
    if (!S.info) throw new Error("The Sticky didn't answer after installing. Unplug it, plug it back in, and choose its port again (step 2).");
    S.installed = true;
    S.wifi.keep = false;
    // An update that kept everything: nothing else to do.
    S.updated = S.wasConfigured && !S.eraseAll && !!S.info.configured;
    phase.textContent = `Installed Sticky Remote ${S.info.fw} on ${S.info.name}.`;
    render();
  } catch (e) {
    phase.className = "note bad";
    phase.textContent = String(e?.message || e);
    $("#back").disabled = false;
    $("#go").disabled = false;
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
  } catch (e) {
    h.client = null;
    h.error = String(e?.message || e);
  }
  render();
}

async function send() {
  const checks = $("#send-checks"), err = $("#send-error");
  const item = (id, text, cls = "wait") => {
    let li = document.getElementById(id);
    if (!li) { li = document.createElement("li"); li.id = id; checks.append(li); }
    li.className = cls; li.innerHTML = text;
  };
  $("#go").disabled = true; $("#back").disabled = true; err.hidden = true;
  try {
    item("c1", "Sending…");
    await S.serial.set({ settings: S.settings, wifi: S.wifi.keep ? null : { ssid: S.wifi.ssid, pass: S.wifi.pass } });
    item("c1", "Saved on the Sticky.", "ok");
    item("c2", "Restarting…");
    await sleep(1500);
    const back = await S.serial.waitForBoard(25000);
    if (!back) throw new Error("The Sticky didn't come back after restarting. Check the cable, then send again.");
    item("c2", "Restarted with the new settings.", "ok");
    item("c3", "Joining Wi-Fi…");
    let info = back;
    const end = Date.now() + 45000;
    while (!info?.wifi?.connected && Date.now() < end) { await sleep(2000); info = await S.serial.info(1) || info; }
    S.info = info;
    if (!info?.wifi?.connected) {
      item("c3", `Couldn't join <b>${esc(S.wifi.ssid)}</b>. Check the password, and that it's a 2.4 GHz network.`, "bad");
      $("#back").disabled = false; $("#go").disabled = false; $("#go").textContent = "Send Again";
      return;
    }
    item("c3", `On Wi-Fi: ${esc(info.wifi.ssid || "")} · ${esc(info.wifi.ip)}`, "ok");
    $("#go").hidden = true; $("#next").hidden = false; $("#back").disabled = false;
  } catch (e) {
    err.hidden = false; err.textContent = String(e?.message || e);
    $("#back").disabled = false; $("#go").disabled = false;
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
