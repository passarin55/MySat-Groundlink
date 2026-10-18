"""
dashboard - a local web console for the MySat Groundlink earth station.

Standard library only: a threaded HTTP server on 127.0.0.1 serving a single
page that polls /api/state. Started with `--dash` on the command line or
`/dash [port]` from the station console.

Bound to loopback on purpose - it exposes uplink commanding, which has no
business listening on a network interface.
"""

from __future__ import annotations

import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

import mskproto as msk

_station = None
_server = None


PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MySat Ground Control</title>
<style>
  :root{
    --bg:#f4f6f9; --panel:#ffffff; --ink:#141922; --muted:#5b6675;
    --line:#dde3ea; --accent:#1d6fe0; --ok:#1a8a4a; --warn:#b8770a; --bad:#c02b2b;
    --mono:ui-monospace,SFMono-Regular,"SF Mono",Menlo,Consolas,monospace;
  }
  @media (prefers-color-scheme:dark){
    :root:not([data-theme="light"]){
      --bg:#0e1218; --panel:#161c25; --ink:#e6ecf3; --muted:#93a1b3;
      --line:#26303c; --accent:#5aa3ff; --ok:#3fcf7a; --warn:#e0a33a; --bad:#ff6b6b;
    }
  }
  :root[data-theme="dark"]{
    --bg:#0e1218; --panel:#161c25; --ink:#e6ecf3; --muted:#93a1b3;
    --line:#26303c; --accent:#5aa3ff; --ok:#3fcf7a; --warn:#e0a33a; --bad:#ff6b6b;
  }
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--ink);
       font:14px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
  header{display:flex;flex-wrap:wrap;gap:12px;align-items:baseline;
         padding:14px 20px;border-bottom:1px solid var(--line);background:var(--panel)}
  h1{font-size:16px;margin:0;letter-spacing:.02em}
  .sub{color:var(--muted);font-size:12px;font-family:var(--mono)}
  .dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--bad);
       margin-right:6px;vertical-align:middle}
  .dot.live{background:var(--ok)}
  main{display:grid;gap:14px;padding:16px 20px;
       grid-template-columns:repeat(auto-fit,minmax(310px,1fr));max-width:1500px}
  section{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:14px 16px}
  h2{font-size:11px;text-transform:uppercase;letter-spacing:.09em;color:var(--muted);
     margin:0 0 10px}
  .kv{display:grid;grid-template-columns:auto 1fr;gap:4px 14px;font-family:var(--mono);font-size:13px}
  .kv b{font-weight:500;color:var(--muted)}
  .kv span{text-align:right}
  .big{font-family:var(--mono);font-size:26px;letter-spacing:-.01em}
  .chips{display:flex;flex-wrap:wrap;gap:6px}
  .chip{font-family:var(--mono);font-size:11px;padding:2px 8px;border-radius:20px;
        border:1px solid var(--line);color:var(--muted)}
  .chip.on{color:var(--ok);border-color:var(--ok)}
  .chip.off{color:var(--bad);border-color:var(--bad)}
  svg{display:block;width:100%;height:auto}
  .log{font-family:var(--mono);font-size:12px;max-height:230px;overflow:auto}
  .log div{padding:2px 0;border-bottom:1px solid var(--line);white-space:pre-wrap;word-break:break-word}
  .lvl-WARN{color:var(--warn)} .lvl-ERROR{color:var(--bad)} .lvl-INFO{color:var(--ok)}
  form{display:flex;gap:8px;margin-bottom:10px}
  input[type=text]{flex:1;min-width:0;padding:8px 10px;border-radius:7px;
        border:1px solid var(--line);background:var(--bg);color:var(--ink);font-family:var(--mono)}
  button{padding:8px 14px;border-radius:7px;border:1px solid var(--accent);
         background:var(--accent);color:#fff;cursor:pointer;font-weight:600}
  button.ghost{background:transparent;color:var(--accent)}
  .quick{display:flex;flex-wrap:wrap;gap:6px}
  .quick button{font-size:12px;padding:5px 10px;font-weight:500}
  .bar{height:7px;border-radius:4px;background:var(--line);overflow:hidden;margin-top:6px}
  .bar i{display:block;height:100%;background:var(--accent)}
  footer{padding:10px 20px 30px;color:var(--muted);font-size:12px}
  .hint{margin:14px 20px -4px;padding:11px 14px;border-radius:9px;
        border:1px solid var(--warn);background:color-mix(in srgb,var(--warn) 12%,transparent);
        color:var(--ink);font-size:13px;max-width:1500px}
  .hint b{color:var(--warn)}
  .hint code{font-family:var(--mono);background:var(--bg);padding:1px 5px;border-radius:4px}
  .hint button{margin-left:10px;font-size:12px;padding:4px 10px}
</style>
</head>
<body>
<header>
  <h1><span id="dot" class="dot"></span>MySat Ground Control</h1>
  <div class="sub" id="linkinfo">connecting...</div>
</header>

<div id="hint" style="display:none"></div>

<main>
  <section>
    <h2>Link</h2>
    <div class="kv" id="link"></div>
  </section>

  <section>
    <h2>Power</h2>
    <div class="big" id="vbat">--</div>
    <div class="kv" id="power"></div>
  </section>

  <section>
    <h2>Environment</h2>
    <div class="kv" id="env"></div>
  </section>

  <section>
    <h2>Attitude</h2>
    <svg viewBox="0 0 300 150" id="att"></svg>
    <div class="kv" id="attkv"></div>
  </section>

  <section>
    <h2>Sun trackers</h2>
    <svg viewBox="0 0 300 220" id="sun"></svg>
  </section>

  <section>
    <h2>Battery history</h2>
    <svg viewBox="0 0 300 130" id="hist"></svg>
  </section>

  <section>
    <h2>Subsystems</h2>
    <div class="chips" id="health"></div>
    <h2 style="margin-top:14px">Radio controller</h2>
    <div class="kv" id="radio"></div>
  </section>

  <section style="grid-column:1/-1">
    <h2>Telecommand</h2>
    <form id="tcform">
      <input type="text" id="tcinput" placeholder="ping | solar deploy | mode stream | snap down | img 3"
             autocomplete="off" spellcheck="false">
      <button type="submit">Uplink</button>
    </form>
    <div class="quick" id="quick"></div>
    <div id="imgbar"></div>
  </section>

  <section style="grid-column:1/-1">
    <h2>Events</h2>
    <div class="log" id="events"></div>
  </section>
</main>

<footer>MSK-1 link. Telemetry is logged to CSV and every frame to the session log
in the station's data directory.</footer>

<script>
const QUICK = ["ping","tm","sys","mode stream","mode beacon","solar deploy","solar retract",
               "led on","led off","snap","snap down","imglist","rfstat","safe"];
const $ = id => document.getElementById(id);

function kv(el, pairs){
  el.innerHTML = pairs.map(([k,v]) => `<b>${k}</b><span>${v ?? "--"}</span>`).join("");
}
function fmt(v, digits, unit){
  if(v === null || v === undefined || v === "") return "--";
  const n = Number(v);
  return (Number.isFinite(n) ? n.toFixed(digits) : v) + (unit ? " " + unit : "");
}

function drawAttitude(t){
  const roll = Number(t.roll)||0, pitch = Number(t.pitch)||0;
  const cx=150, cy=75, r=58;
  const rad = roll*Math.PI/180;
  const off = Math.max(-r, Math.min(r, pitch*1.1));
  const dx = Math.cos(rad)*r, dy = Math.sin(rad)*r;
  $("att").innerHTML = `
    <defs><clipPath id="c"><circle cx="${cx}" cy="${cy}" r="${r}"/></clipPath></defs>
    <circle cx="${cx}" cy="${cy}" r="${r}" fill="var(--bg)" stroke="var(--line)"/>
    <g clip-path="url(#c)" transform="rotate(${-roll} ${cx} ${cy}) translate(0 ${off})">
      <rect x="${cx-r}" y="${cy-r*2}" width="${r*2}" height="${r*2}" fill="var(--accent)" opacity=".22"/>
      <rect x="${cx-r}" y="${cy}" width="${r*2}" height="${r*2}" fill="var(--muted)" opacity=".28"/>
      <line x1="${cx-r}" y1="${cy}" x2="${cx+r}" y2="${cy}" stroke="var(--ink)" stroke-width="1.5"/>
    </g>
    <line x1="${cx-dx*0}" y1="${cy}" x2="${cx}" y2="${cy}" stroke="none"/>
    <circle cx="${cx}" cy="${cy}" r="3" fill="var(--ink)"/>
    <text x="${cx+r+8}" y="${cy-40}" font-size="10" fill="var(--muted)"
          font-family="monospace">yaw ${fmt(t.yaw,0,"°")}</text>`;
}

function drawSun(t){
  const v = [t.ph1,t.ph2,t.ph3,t.ph4].map(x => Math.max(0, Number(x)||0));
  const max = Math.max(1, ...v);
  const cx=150, cy=110, R=78;
  // ph1 left(-X) ph2 back(-Y) ph3 right(+X) ph4 front(+Y), per console.h
  const dirs = [[-1,0,"L"],[0,1,"B"],[1,0,"R"],[0,-1,"F"]];
  let vx=0, vy=0, bars="";
  dirs.forEach(([dx,dy,label], i) => {
    const f = v[i]/max;
    vx += dx*f; vy += dy*f;
    const x = cx+dx*R, y = cy+dy*R;
    bars += `<circle cx="${x}" cy="${y}" r="${6+14*f}" fill="var(--accent)" opacity="${.18+.55*f}"/>
             <text x="${x}" y="${y+3}" font-size="9" text-anchor="middle"
                   fill="var(--ink)" font-family="monospace">${label}</text>`;
  });
  const len = Math.hypot(vx,vy);
  const arrow = len > 0.04
    ? `<line x1="${cx}" y1="${cy}" x2="${cx+vx*R*0.85}" y2="${cy+vy*R*0.85}"
             stroke="var(--warn)" stroke-width="2.5" stroke-linecap="round"/>`
    : `<text x="${cx}" y="${cy+34}" font-size="10" text-anchor="middle"
             fill="var(--muted)" font-family="monospace">eclipse / no sun</text>`;
  $("sun").innerHTML = `
    <line x1="${cx-R-14}" y1="${cy}" x2="${cx+R+14}" y2="${cy}" stroke="var(--line)"/>
    <line x1="${cx}" y1="${cy-R-14}" x2="${cx}" y2="${cy+R+14}" stroke="var(--line)"/>
    ${bars}${arrow}
    <circle cx="${cx}" cy="${cy}" r="3" fill="var(--ink)"/>
    <text x="6" y="14" font-size="10" fill="var(--muted)" font-family="monospace">
      peak ${max.toFixed(0)} counts</text>`;
}

function drawHistory(history){
  const pts = history.map(h => Number(h.battery_v)).filter(Number.isFinite);
  if(pts.length < 2){ $("hist").innerHTML =
    `<text x="8" y="22" font-size="11" fill="var(--muted)" font-family="monospace">
       waiting for telemetry</text>`; return; }
  const lo = Math.min(...pts), hi = Math.max(...pts);
  const span = (hi-lo) || 0.1;
  const path = pts.map((v,i) =>
    `${i ? "L" : "M"}${(i/(pts.length-1)*288+6).toFixed(1)},${(116-((v-lo)/span)*100).toFixed(1)}`
  ).join(" ");
  $("hist").innerHTML = `
    <path d="${path}" fill="none" stroke="var(--accent)" stroke-width="2"/>
    <text x="6" y="12" font-size="10" fill="var(--muted)" font-family="monospace">${hi.toFixed(3)} V</text>
    <text x="6" y="127" font-size="10" fill="var(--muted)" font-family="monospace">${lo.toFixed(3)} V</text>`;
}

async function refresh(){
  let s;
  try{ s = await (await fetch("/api/state")).json(); }
  catch(e){ $("dot").className = "dot"; return; }

  const st = s.stats, tm = s.telemetry || {}, bc = s.beacon || {};
  const fresh = st.age_last_frame !== null && st.age_last_frame < 15;
  $("dot").className = "dot" + (fresh ? " live" : "");
  $("linkinfo").textContent = `${s.link} — addressing ${s.callsign}`;

  kv($("link"), [
    ["frames ok", st.frames_ok], ["frames bad", st.frames_bad],
    ["error rate", (st.frame_error_rate*100).toFixed(1)+" %"],
    ["last frame", st.age_last_frame === null ? "never" : st.age_last_frame+" s ago"],
    ["last beacon", st.age_last_beacon === null ? "never" : Math.round(st.age_last_beacon)+" s ago"],
    ["TC sent / acked", st.telecommands_sent + " / " + st.acks],
    ["mode", bc.mode_name || "--"],
  ]);

  $("vbat").textContent = fmt(tm.battery_v, 3, "V");
  kv($("power"), [
    ["battery current", fmt(tm.battery_ma,1,"mA")],
    ["solar bus", fmt(tm.solar_v,3,"V")],
    ["solar left", fmt(tm.solar_ma_left,1,"mA")],
    ["solar right", fmt(tm.solar_ma_right,1,"mA")],
    ["panels", tm.solar_deployed ? "DEPLOYED" : "stowed"],
    ["STAR LED", tm.star_led ? "on" : "off"],
  ]);

  kv($("env"), [
    ["temperature", fmt(tm.temperature,2,"°C")],
    ["humidity", fmt(tm.humidity,1,"%RH")],
    ["pressure", fmt(tm.pressure,2,"hPa")],
    ["gas", fmt(tm.gas_resistance,1,"kΩ")],
    ["IAQ", fmt(tm.iaq,0) + " (acc " + (tm.iaq_accuracy ?? "-") + ")"],
    ["UTC", tm.utc || "--"],
    ["uptime", tm.uptime_s ? tm.uptime_s + " s" : "--"],
  ]);

  drawAttitude(tm); drawSun(tm); drawHistory(s.history || []);
  kv($("attkv"), [["roll", fmt(tm.roll,1,"°")], ["pitch", fmt(tm.pitch,1,"°")],
                  ["yaw", fmt(tm.yaw,1,"°")]]);

  const SENSORS = ["bme680","mpu","ads1015","ina3221","ds3231","camera","rcu"];
  $("health").innerHTML = Object.entries(bc.health || {})
    .map(([k,v]) => SENSORS.includes(k)
        ? `<span class="chip ${v?"on":"off"}">${k}</span>`      // presence: red when missing
        : `<span class="chip ${v?"on":""}">${k}</span>`)        // state: grey when inactive
    .join("") || `<span class="chip">no beacon yet</span>`;

  const r = s.radio || {};
  kv($("radio"), [
    ["channel", r.rf_channel ?? "--"], ["tx power", r.rf_txpower ?? "--"],
    ["FU mode", r.rf_fu_mode ?? "--"], ["baud", r.rf_baud ?? "--"],
    ["HC-12 resets", r.rf_resets ?? "--"],
    ["watchdog", r.wdt_remaining_s !== undefined ? r.wdt_remaining_s + " s" : "--"],
    ["last error", r.last_error_text || "--"],
  ]);

  // The single most confusing state: beacons arriving, every sensor panel
  // empty. BEACON mode transmits only $BC frames, which carry uptime, battery
  // and health flags - no sensor data at all. Say so instead of showing "--".
  const noTelemetry = !tm || tm.temperature === undefined || tm.temperature === null;
  const mode = bc.mode_name;
  const h = $("hint");
  if(noTelemetry && (mode === "BEACON" || mode === "OFF")){
    h.style.display = "";
    h.innerHTML = `<b>No telemetry is being transmitted.</b> The satellite is in
      <code>${mode}</code> mode, which sends beacons only — uptime, battery and
      health flags, but no sensor readings. Switch it to streaming to populate
      these panels.
      <button data-cmd="mode stream">Send “mode stream”</button>`;
  } else if(noTelemetry){
    h.style.display = "";
    h.innerHTML = `<b>Waiting for the first telemetry frame…</b> mode is
      <code>${mode || "?"}</code>.`;
  } else {
    h.style.display = "none";
  }

  $("imgbar").innerHTML = s.image
    ? `<div class="sub">image #${s.image.id}: ${s.image.received}/${s.image.total} chunks</div>
       <div class="bar"><i style="width:${(s.image.progress*100).toFixed(1)}%"></i></div>` : "";

  $("events").innerHTML = (s.events || []).slice().reverse().map(e =>
    `<div class="lvl-${e.level}">${new Date(e.t*1000).toLocaleTimeString()}  [${e.level}]  ${e.text}</div>`
  ).join("") || "<div>no events yet</div>";
}

async function uplink(text){
  if(!text.trim()) return;
  const box = $("events");
  const res = await fetch("/api/command", {
    method:"POST", headers:{"Content-Type":"application/json"},
    body: JSON.stringify({command:text})
  }).then(r => r.json()).catch(() => ({status:"ERR", text:"dashboard could not reach the station"}));
  box.insertAdjacentHTML("afterbegin",
    `<div class="lvl-${res.status === "OK" ? "INFO" : "WARN"}">` +
    `${new Date().toLocaleTimeString()}  [uplink ${text}]  ${res.status}: ${res.text}</div>`);
}

$("quick").innerHTML = QUICK.map(c =>
  `<button class="ghost" data-cmd="${c}">${c}</button>`).join("");
$("quick").addEventListener("click", e => {
  if(e.target.dataset.cmd) uplink(e.target.dataset.cmd);
});
$("hint").addEventListener("click", e => {
  if(e.target.dataset.cmd) uplink(e.target.dataset.cmd);
});
$("tcform").addEventListener("submit", e => {
  e.preventDefault();
  uplink($("tcinput").value);
  $("tcinput").value = "";
});

refresh();
setInterval(refresh, 1000);
</script>
</body>
</html>
"""


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):        # keep the station console clean
        pass

    def _send(self, code: int, body: bytes, content_type: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path).path
        if path in ("/", "/index.html"):
            self._send(200, PAGE.encode("utf-8"), "text/html; charset=utf-8")
        elif path == "/api/state":
            payload = json.dumps(_station.snapshot(), default=str).encode("utf-8")
            self._send(200, payload, "application/json")
        else:
            self._send(404, b"not found", "text/plain")

    def do_POST(self):
        if urlparse(self.path).path != "/api/command":
            self._send(404, b"not found", "text/plain")
            return

        length = int(self.headers.get("Content-Length", "0"))
        try:
            request = json.loads(self.rfile.read(length) or b"{}")
        except ValueError:
            self._send(400, b'{"status":"ERR","text":"bad json"}', "application/json")
            return

        parts = str(request.get("command", "")).split()
        if not parts:
            self._send(400, b'{"status":"ERR","text":"empty command"}', "application/json")
            return

        command, args = parts[0], parts[1:]
        if command.upper() not in msk.COMMANDS:
            body = json.dumps({"status": "ERR",
                               "text": f"'{command}' is not a known telecommand"})
            self._send(200, body.encode(), "application/json")
            return

        ack = _station.send(command, *args)
        result = ({"status": ack.get(2), "text": ack.get(3)} if ack
                  else {"status": "TIMEOUT", "text": "no acknowledgement from the satellite"})
        self._send(200, json.dumps(result).encode(), "application/json")


def serve(station, port: int = 8800, host: str = "127.0.0.1") -> None:
    """Starts the dashboard on a daemon thread. Safe to call twice."""
    global _station, _server
    _station = station
    if _server is not None:
        print(f"  dashboard already running: http://{host}:{port}/")
        return
    try:
        _server = ThreadingHTTPServer((host, port), Handler)
    except OSError as exc:
        print(f"  dashboard could not bind {host}:{port} - {exc}")
        return
    threading.Thread(target=_server.serve_forever, daemon=True).start()
    print(f"  dashboard: http://{host}:{port}/")
