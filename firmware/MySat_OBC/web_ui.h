/*
 * web_ui.h - the satellite's own mission-control page.
 *
 * Entirely self-contained: no CDN, no bootstrap.css, no images from LittleFS.
 * The stock Web GUI needed ~200 kB of assets uploaded to the filesystem and
 * simply showed a broken page if you had not done that. This one is a single
 * PROGMEM string, so it works on a freshly flashed board.
 *
 * Served with server.send_P() so the page streams straight out of flash rather
 * than being copied into a String first - at this size a heap copy would be a
 * meaningful chunk of the ESP32's free RAM.
 *
 * The radio remains the primary link. This page is a convenience for when the
 * satellite is on Wi-Fi and you want a richer view; nothing here changes how
 * MSK-1 behaves, and the HTTP handlers only log through logDebug(), which is
 * muted whenever the radio needs the UART.
 */
#ifndef WEB_UI_H
#define WEB_UI_H

#include <pgmspace.h>

static const char MYSAT_UI[] PROGMEM = R"MYSATUI(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MySat Mission Control</title>
<style>
  :root{
    --bg:#080b12; --panel:#111823; --panel2:#0d141e; --ink:#e8eef7; --muted:#8496ad;
    --line:#1d2836; --accent:#4da3ff; --ok:#3ddc84; --warn:#ffb020; --bad:#ff5c5c;
    --sun:#ffd166; --mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
  }
  *{box-sizing:border-box}
  html,body{margin:0;background:var(--bg);color:var(--ink);
    font:14px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
  a{color:var(--accent)}
  header{display:flex;flex-wrap:wrap;gap:10px 22px;align-items:baseline;
    padding:12px 18px;background:linear-gradient(180deg,#141d2b,#0d141e);
    border-bottom:1px solid var(--line);position:sticky;top:0;z-index:5}
  h1{margin:0;font-size:15px;letter-spacing:.14em;text-transform:uppercase}
  .sub{font-family:var(--mono);font-size:12px;color:var(--muted)}
  .dot{width:9px;height:9px;border-radius:50%;background:var(--bad);display:inline-block;
    margin-right:7px;vertical-align:middle;box-shadow:0 0 8px currentColor}
  .dot.live{background:var(--ok);color:var(--ok)}
  main{display:grid;gap:12px;padding:12px 18px 40px;max-width:1600px;
    grid-template-columns:repeat(auto-fit,minmax(300px,1fr))}
  .wide{grid-column:1/-1}
  @media(min-width:1100px){ .cube{grid-column:span 2;grid-row:span 2} }
  section{background:var(--panel);border:1px solid var(--line);border-radius:11px;padding:13px 15px}
  h2{margin:0 0 11px;font-size:10.5px;letter-spacing:.15em;text-transform:uppercase;color:var(--muted)}
  .kv{display:grid;grid-template-columns:auto 1fr;gap:5px 12px;font-family:var(--mono);font-size:13px}
  .kv b{font-weight:400;color:var(--muted)}
  .kv span{text-align:right;font-variant-numeric:tabular-nums}
  .big{font-family:var(--mono);font-size:30px;line-height:1.1;font-variant-numeric:tabular-nums}
  .unit{font-size:14px;color:var(--muted);margin-left:3px}
  canvas{display:block;width:100%;height:auto;border-radius:8px;background:
    radial-gradient(ellipse at 50% 35%,#16202f 0%,#080b12 72%)}
  svg{display:block;width:100%;height:auto}
  .bar{height:8px;border-radius:5px;background:#0a1017;overflow:hidden;margin-top:6px;
    border:1px solid var(--line)}
  .bar i{display:block;height:100%;transition:width .4s}
  .chips{display:flex;flex-wrap:wrap;gap:5px;margin-top:4px}
  .chip{font-family:var(--mono);font-size:10.5px;padding:2px 8px;border-radius:20px;
    border:1px solid var(--line);color:var(--muted)}
  .chip.on{color:var(--ok);border-color:#1e5c3a;background:#0d1f17}
  .chip.off{color:var(--bad);border-color:#5c1e1e;background:#1f0d0d}
  button{font:inherit;font-size:12.5px;padding:7px 12px;border-radius:8px;cursor:pointer;
    border:1px solid var(--line);background:#16202f;color:var(--ink);transition:.15s}
  button:hover{border-color:var(--accent);color:var(--accent)}
  button:disabled{opacity:.45;cursor:default}
  button.pri{background:var(--accent);border-color:var(--accent);color:#04101f;font-weight:600}
  .row{display:flex;flex-wrap:wrap;gap:7px}
  .note{font-size:12px;color:var(--muted);margin-top:8px}
  .warnbox{border:1px solid var(--warn);background:#1d1608;color:var(--ink);
    border-radius:9px;padding:9px 12px;font-size:12.5px;margin-top:9px}
  img.shot{width:100%;border-radius:8px;border:1px solid var(--line);display:block}
  .thumbs{display:flex;gap:6px;flex-wrap:wrap;margin-top:8px}
  .thumbs button{padding:4px 9px;font-family:var(--mono);font-size:11px}
  ul.files{list-style:none;margin:0;padding:0;font-family:var(--mono);font-size:12px;
    max-height:190px;overflow:auto}
  ul.files li{display:flex;justify-content:space-between;gap:10px;padding:4px 0;
    border-bottom:1px solid var(--line)}
  .toast{position:fixed;right:16px;bottom:16px;background:var(--panel);border:1px solid var(--line);
    border-left:3px solid var(--accent);border-radius:9px;padding:10px 14px;font-size:13px;
    max-width:360px;opacity:0;transform:translateY(8px);transition:.25s;pointer-events:none;z-index:9}
  .toast.show{opacity:1;transform:none}
</style>
</head>
<body>

<header>
  <h1><span id="dot" class="dot"></span><span id="callsign">MYSAT</span></h1>
  <div class="sub" id="clock">--</div>
  <div class="sub" id="uptime">--</div>
  <div class="sub" id="fw">MSK-1 &middot; radio link primary</div>
</header>

<main>
  <section class="cube">
    <h2>Attitude &amp; configuration</h2>
    <canvas id="view" width="900" height="620"></canvas>
    <div class="kv" style="margin-top:10px">
      <b>roll</b><span id="a-roll">--</span>
      <b>pitch</b><span id="a-pitch">--</span>
      <b>yaw</b><span id="a-yaw">--</span>
      <b>solar panels</b><span id="a-panels">--</span>
      <b>STAR LED</b><span id="a-led">--</span>
    </div>
    <div id="calwarn"></div>
  </section>

  <section>
    <h2>Power</h2>
    <div class="big"><span id="p-v">--</span><span class="unit">V</span></div>
    <div class="bar"><i id="p-bar" style="width:0%;background:var(--ok)"></i></div>
    <div class="kv" style="margin-top:10px">
      <b>battery current</b><span id="p-i">--</span>
      <b>solar bus</b><span id="p-sv">--</span>
      <b>solar left</b><span id="p-sl">--</span>
      <b>solar right</b><span id="p-sr">--</span>
      <b>free heap</b><span id="p-heap">--</span>
    </div>
  </section>

  <section>
    <h2>Environment</h2>
    <div class="big"><span id="e-t">--</span><span class="unit">&deg;C</span></div>
    <div class="kv" style="margin-top:10px">
      <b>humidity</b><span id="e-h">--</span>
      <b>pressure</b><span id="e-p">--</span>
      <b>gas resistance</b><span id="e-g">--</span>
      <b>IAQ</b><span id="e-iaq">--</span>
    </div>
    <div class="bar"><i id="e-bar" style="width:0%;background:var(--ok)"></i></div>
    <div class="note" id="e-note"></div>
  </section>

  <section>
    <h2>Sun trackers</h2>
    <svg id="sun" viewBox="0 0 300 250"></svg>
  </section>

  <section>
    <h2>History</h2>
    <svg id="hist" viewBox="0 0 300 150"></svg>
    <div class="note">battery (blue) &middot; temperature (amber) &middot; IAQ (green)</div>
  </section>

  <section>
    <h2>Subsystems</h2>
    <div class="chips" id="health"></div>
    <h2 style="margin-top:14px">Commands</h2>
    <div class="row">
      <button onclick="act('/light_on','STAR LED toggled')">Toggle STAR LED</button>
      <button onclick="act('/motor_on','solar panels moving')">Move solar panels</button>
    </div>
    <div class="note">The radio link stays authoritative. Anything done here is also
      reachable as an MSK-1 telecommand.</div>
  </section>

  <section>
    <h2>Camera</h2>
    <div class="row"><button class="pri" id="snap">Capture image</button></div>
    <div class="thumbs" id="thumbs"></div>
    <div id="shotwrap" style="margin-top:9px"></div>
  </section>

  <section>
    <h2>Mission data</h2>
    <ul class="files" id="logs"><li>loading…</li></ul>
    <div class="row" style="margin-top:9px">
      <button onclick="location.href='/download_event_log'">Download event log</button>
      <button onclick="loadLogs()">Refresh</button>
    </div>
  </section>
</main>

<div class="toast" id="toast"></div>

<script>
const $ = i => document.getElementById(i);
const hist = { v: [], t: [], q: [] };
let latest = {};

function toast(msg){
  const t = $("toast"); t.textContent = msg; t.classList.add("show");
  clearTimeout(t._h); t._h = setTimeout(() => t.classList.remove("show"), 3200);
}
async function act(url, msg){
  try { await fetch(url); toast(msg); } catch(e){ toast("command failed"); }
}
const num = (v, d) => (v === null || v === undefined || v === "" || Number.isNaN(Number(v)))
  ? null : Number(Number(v).toFixed(d));
const show = (v, d, u) => v === null ? "--" : v.toFixed(d) + (u ? " " + u : "");

/* ---------------------------------------------------------------- 3D view --
   A small software renderer: rotate the model by the reported attitude,
   sort faces back-to-front, shade by the angle to the sun vector that the
   photodiodes imply. No external 3D library - this has to fit in flash and
   run on whatever browser is to hand.                                        */
const cv = $("view"), cx = cv.getContext("2d");
let spin = 0, panelAnim = 0;

function rotate(p, roll, pitch, yaw){
  const d = Math.PI / 180, cr = Math.cos(roll*d), sr = Math.sin(roll*d);
  const cp = Math.cos(pitch*d), sp = Math.sin(pitch*d);
  const cy = Math.cos(yaw*d),  sy = Math.sin(yaw*d);
  let [x,y,z] = p;
  [y,z] = [y*cr - z*sr, y*sr + z*cr];      // roll  about X
  [x,z] = [x*cp + z*sp, -x*sp + z*cp];     // pitch about Y
  [x,y] = [x*cy - y*sy, x*sy + y*cy];      // yaw   about Z
  return [x,y,z];
}
function project(p, w, h, scale){
  const d = 5.2, f = scale / (d - p[2]);
  return [w/2 + p[0]*f, h/2 - p[1]*f];
}
function faceNormal(a,b,c){
  const u=[b[0]-a[0],b[1]-a[1],b[2]-a[2]], v=[c[0]-a[0],c[1]-a[1],c[2]-a[2]];
  const n=[u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]];
  const L=Math.hypot(...n)||1; return n.map(k=>k/L);
}

function draw(){
  const W = cv.width, H = cv.height, S = 210;
  cx.clearRect(0,0,W,H);

  // starfield - stable, seeded by index rather than random each frame
  cx.fillStyle = "#ffffff";
  for(let i=0;i<90;i++){
    const sx=(i*7919%W), sy=(i*104729%H), a=((i*37)%50)/220;
    cx.globalAlpha=a; cx.fillRect(sx,sy,1.4,1.4);
  }
  cx.globalAlpha=1;

  const hasAtt = latest.roll !== null && latest.roll !== undefined;
  const roll  = hasAtt ? latest.roll  : 0;
  const pitch = hasAtt ? latest.pitch : 0;
  const yaw   = hasAtt ? latest.yaw   : spin;
  if(!hasAtt) spin = (spin + 0.35) % 360;

  const target = latest.motor_state ? 1 : 0;
  panelAnim += (target - panelAnim) * 0.06;

  // sun direction from the four photodiodes (body frame)
  const p1=latest.ph1||0, p2=latest.ph2||0, p3=latest.ph3||0, p4=latest.ph4||0;
  const mx = Math.max(1, p1, p2, p3, p4);
  let sunV = [(p3-p1)/mx, (p4-p2)/mx, 0.55];
  const sl = Math.hypot(...sunV)||1; sunV = sunV.map(v=>v/sl);

  const faces = [];
  const B = 0.5;
  const body = [[-B,-B,-B],[B,-B,-B],[B,B,-B],[-B,B,-B],
                [-B,-B, B],[B,-B, B],[B,B, B],[-B,B, B]];
  const quads = [[0,1,2,3],[5,4,7,6],[4,0,3,7],[1,5,6,2],[3,2,6,7],[4,5,1,0]];
  quads.forEach((q,i)=>{
    const pts = q.map(k=>rotate(body[k], roll,pitch,yaw));
    faces.push({pts, base:[ "#33465e","#33465e","#3c5270","#3c5270","#46617f","#2b3b50" ][i], kind:"body"});
  });

  // solar panels, hinged on +/-X, folded against the body when stowed
  [-1,1].forEach(side=>{
    const ang = panelAnim * Math.PI/2;           // 0 = stowed, 90deg = deployed
    const L = 1.25, hx = side*B;
    const corner = (ty,tz) => {
      const ex = Math.sin(ang)*L*side, ez = Math.cos(ang)*L;
      return [hx + ex*tz, ty*B, ez*tz - (1-panelAnim)*0.0];
    };
    const raw = [corner(-1,0), corner(1,0), corner(1,1), corner(-1,1)];
    const pts = raw.map(p=>rotate(p, roll,pitch,yaw));
    faces.push({pts, base:"#123a6b", kind:"panel"});
  });

  faces.forEach(f=>{
    f.depth = f.pts.reduce((s,p)=>s+p[2],0)/f.pts.length;
    const n = faceNormal(f.pts[0],f.pts[1],f.pts[2]);
    f.lit = Math.max(0.16, n[0]*sunV[0]+n[1]*sunV[1]+n[2]*sunV[2]);
  });
  faces.sort((a,b)=>a.depth-b.depth);

  faces.forEach(f=>{
    const scr = f.pts.map(p=>project(p,W,H,S));
    cx.beginPath(); cx.moveTo(scr[0][0],scr[0][1]);
    scr.slice(1).forEach(p=>cx.lineTo(p[0],p[1])); cx.closePath();
    const c = f.base, L = 0.35 + f.lit*0.85;
    cx.fillStyle = shade(c, L);
    cx.fill();
    cx.strokeStyle = f.kind==="panel" ? "#2f7fd8" : "#5d7896";
    cx.lineWidth = 1.2; cx.stroke();
    if(f.kind==="panel" && f.lit>0.3){
      cx.strokeStyle="rgba(120,190,255,.35)"; cx.lineWidth=.8;
      for(let g=1;g<4;g++){
        const a=scr[0], b=scr[3], c2=scr[1], d2=scr[2], t=g/4;
        cx.beginPath();
        cx.moveTo(a[0]+(b[0]-a[0])*t, a[1]+(b[1]-a[1])*t);
        cx.lineTo(c2[0]+(d2[0]-c2[0])*t, c2[1]+(d2[1]-c2[1])*t);
        cx.stroke();
      }
    }
  });

  // STAR LED
  if(latest.star_led){
    const p = project(rotate([0,0,B+0.02],roll,pitch,yaw),W,H,S);
    const g = cx.createRadialGradient(p[0],p[1],0,p[0],p[1],46);
    g.addColorStop(0,"rgba(255,246,210,.95)"); g.addColorStop(1,"rgba(255,220,120,0)");
    cx.fillStyle=g; cx.beginPath(); cx.arc(p[0],p[1],46,0,7); cx.fill();
  }

  // sun arrow
  const sunEnd = project(rotate(sunV.map(v=>v*2.1),roll,pitch,yaw),W,H,S);
  const org = project([0,0,0],W,H,S);
  if(Math.hypot((p3-p1)/mx,(p4-p2)/mx) > 0.05){
    cx.strokeStyle="rgba(255,209,102,.85)"; cx.lineWidth=2.4;
    cx.beginPath(); cx.moveTo(org[0],org[1]); cx.lineTo(sunEnd[0],sunEnd[1]); cx.stroke();
    cx.fillStyle="#ffd166";
    cx.beginPath(); cx.arc(sunEnd[0],sunEnd[1],6,0,7); cx.fill();
    cx.font="12px monospace"; cx.fillText("sun", sunEnd[0]+9, sunEnd[1]+4);
  }

  cx.fillStyle="#6f8199"; cx.font="12px monospace";
  cx.fillText(hasAtt ? "attitude: live" : "attitude unavailable - demo rotation", 12, 20);
  cx.fillText(latest.motor_state ? "panels: DEPLOYED" : "panels: stowed", 12, 38);
}
function shade(hex, k){
  const n=parseInt(hex.slice(1),16);
  const r=Math.min(255,((n>>16)&255)*k)|0, g=Math.min(255,((n>>8)&255)*k)|0, b=Math.min(255,(n&255)*k)|0;
  return `rgb(${r},${g},${b})`;
}

/* --------------------------------------------------------------- sun radar */
function drawSun(){
  const p=[latest.ph1||0,latest.ph2||0,latest.ph3||0,latest.ph4||0];
  const mx=Math.max(1,...p), C=150, Y=125, R=82;
  const dirs=[[-1,0,"L"],[0,1,"B"],[1,0,"R"],[0,-1,"F"]];
  let vx=0,vy=0,marks="";
  dirs.forEach(([dx,dy,lab],i)=>{
    const f=p[i]/mx; vx+=dx*f; vy+=dy*f;
    const x=C+dx*R, y=Y+dy*R;
    marks+=`<circle cx="${x}" cy="${y}" r="${7+15*f}" fill="#ffd166" opacity="${.14+.6*f}"/>
      <text x="${x}" y="${y+4}" font-size="10" text-anchor="middle" fill="#e8eef7"
        font-family="monospace">${lab}</text>
      <text x="${x}" y="${y+26}" font-size="9" text-anchor="middle" fill="#8496ad"
        font-family="monospace">${p[i].toFixed(0)}</text>`;
  });
  const len=Math.hypot(vx,vy);
  const arrow = len>0.05
    ? `<line x1="${C}" y1="${Y}" x2="${C+vx*R*.8}" y2="${Y+vy*R*.8}" stroke="#ffd166"
         stroke-width="3" stroke-linecap="round"/>`
    : `<text x="${C}" y="${Y+5}" font-size="11" text-anchor="middle" fill="#8496ad"
         font-family="monospace">no directional sun</text>`;
  $("sun").innerHTML=`<line x1="${C-R-22}" y1="${Y}" x2="${C+R+22}" y2="${Y}" stroke="#1d2836"/>
    <line x1="${C}" y1="${Y-R-22}" x2="${C}" y2="${Y+R+22}" stroke="#1d2836"/>
    ${marks}${arrow}<circle cx="${C}" cy="${Y}" r="3" fill="#e8eef7"/>`;
}

/* ------------------------------------------------------------------ charts */
function spark(vals, colour, lo, hi, o){
  if(vals.length<2) return "";
  const span=(hi-lo)||1;
  return `<path fill="none" stroke="${colour}" stroke-width="1.8" opacity="${o}" d="${
    vals.map((v,i)=>`${i?"L":"M"}${(i/(vals.length-1)*288+6).toFixed(1)},${
      (140-((v-lo)/span)*126).toFixed(1)}`).join(" ")}"/>`;
}
function drawHist(){
  const f=a=>a.filter(Number.isFinite);
  const v=f(hist.v), t=f(hist.t), q=f(hist.q);
  let s="";
  if(v.length>1) s+=spark(v,"#4da3ff",Math.min(...v)-.02,Math.max(...v)+.02,1);
  if(t.length>1) s+=spark(t,"#ffb020",Math.min(...t)-.5,Math.max(...t)+.5,.85);
  if(q.length>1) s+=spark(q,"#3ddc84",0,Math.max(120,...q),.75);
  $("hist").innerHTML = s || `<text x="8" y="24" font-size="11" fill="#8496ad"
    font-family="monospace">collecting…</text>`;
}

/* ------------------------------------------------------------- data update */
async function poll(){
  let d;
  try { d = await (await fetch("/get_data",{cache:"no-store"})).json(); }
  catch(e){ $("dot").className="dot"; return; }
  $("dot").className="dot live";

  // att_valid distinguishes a genuinely level satellite from an uncalibrated
  // MPU; the firmware reports 0/0/0 for both.
  const attOk = d.att_valid === undefined ? (d.roll !== 0 || d.pitch !== 0 || d.yaw !== 0)
                                          : !!d.att_valid;
  latest = {
    roll: attOk?num(d.roll,1):null, pitch: attOk?num(d.pitch,1):null, yaw: attOk?num(d.yaw,1):null,
    ph1:num(d.ph1,0), ph2:num(d.ph2,0), ph3:num(d.ph3,0), ph4:num(d.ph4,0),
    motor_state:!!d.motor_state, star_led:!!d.star_led
  };

  $("callsign").textContent = d.callSign || "MYSAT";
  const p2=n=>String(n).padStart(2,"0");
  $("clock").textContent = d.year_ ? `${d.year_}-${p2(d.month_)}-${p2(d.day_)} ${p2(d.hour_)}:${p2(d.minute_)}:${p2(d.second_)} UTC` : "RTC unset";
  if(d.uptime_s!==undefined){
    const u=d.uptime_s; $("uptime").textContent=`up ${Math.floor(u/3600)}h ${Math.floor(u%3600/60)}m ${u%60}s`;
  }

  const bv=num(d.battery_v,3);
  $("p-v").textContent = bv===null?"--":bv.toFixed(3);
  const pct = bv===null?0:Math.max(0,Math.min(100,(bv-3.2)/(4.2-3.2)*100));
  const pb=$("p-bar"); pb.style.width=pct+"%";
  pb.style.background = pct>55?"var(--ok)":pct>25?"var(--warn)":"var(--bad)";
  $("p-i").textContent  = show(num(d.battery_c,1),1,"mA");
  $("p-sv").textContent = show(num(d.solarPanels_v,3),3,"V");
  $("p-sl").textContent = show(num(d.solar_l,1),1,"mA");
  $("p-sr").textContent = show(num(d.solar_r,1),1,"mA");
  $("p-heap").textContent = d.heap!==undefined ? (d.heap/1024).toFixed(0)+" kB" : "--";
  if(d.reset_reason){
    const bad = d.reset_reason==="BROWNOUT" || d.reset_reason==="PANIC";
    $("fw").innerHTML = "MSK-1 &middot; radio link primary &middot; last reset: " +
      (bad ? `<span style="color:var(--warn)">${d.reset_reason}</span>` : d.reset_reason);
  }

  const tC=num(d.temperature,2), iaq=num(d.iaq_score,1);
  $("e-t").textContent  = tC===null?"--":tC.toFixed(2);
  $("e-h").textContent  = show(num(d.humidity,2),2,"%RH");
  $("e-p").textContent  = show(num(d.pressure,2),2,"hPa");
  $("e-g").textContent  = show(num(d.gas_resistance,1),1,"kOhm");
  const acc=d.iaq_accuracy;
  $("e-iaq").textContent = iaq===null?"--":`${iaq.toFixed(0)} (acc ${acc})`;
  const eb=$("e-bar");
  eb.style.width = iaq===null?"0%":Math.min(100,iaq/300*100)+"%";
  eb.style.background = iaq===null?"var(--muted)":iaq<50?"var(--ok)":iaq<150?"var(--warn)":"var(--bad)";
  $("e-note").textContent = acc===0
    ? "IAQ warming up - the gas baseline needs about five minutes."
    : acc===1 ? "IAQ baseline still settling (accuracy 1 of 3)." : "";

  $("a-roll").textContent  = latest.roll===null?"--":latest.roll.toFixed(1)+" deg";
  $("a-pitch").textContent = latest.pitch===null?"--":latest.pitch.toFixed(1)+" deg";
  $("a-yaw").textContent   = latest.yaw===null?"--":latest.yaw.toFixed(1)+" deg";
  $("a-panels").textContent= latest.motor_state?"deployed":"stowed";
  $("a-led").textContent   = latest.star_led?"on":"off";
  $("calwarn").innerHTML = latest.roll===null
    ? `<div class="warnbox"><b>Attitude unavailable.</b> The MPU has not been
       calibrated, so roll/pitch/yaw are not transmitted. Send the
       <code>CAL</code> telecommand (or type <code>Calibrate</code> on the console)
       with the satellite flat and still for five seconds.</div>` : "";

  const H=d.health||{};
  const flags={bme680:d.temperature!==0||d.pressure!==0, camera:!!d.camera_ready,
               logging:d.logging_state===1, wifi:true};
  $("health").innerHTML = Object.entries({
    "BME680":flags.bme680, "camera":flags.camera,
    "mission log":flags.logging, "wi-fi":true,
    "panels":latest.motor_state, "STAR LED":latest.star_led
  }).map(([k,v])=>`<span class="chip ${v?"on":"off"}">${k}</span>`).join("");

  if(bv!==null){ hist.v.push(bv); if(hist.v.length>150) hist.v.shift(); }
  if(tC!==null){ hist.t.push(tC); if(hist.t.length>150) hist.t.shift(); }
  if(iaq!==null){ hist.q.push(iaq); if(hist.q.length>150) hist.q.shift(); }
  drawHist(); drawSun();
}

/* ------------------------------------------------------------------ camera */
$("snap").onclick = async () => {
  const b=$("snap"); b.disabled=true; b.textContent="Capturing…";
  try{
    const r=await fetch("/get_photo");
    if(!r.ok) throw new Error(await r.text());
    const j=await r.json();
    $("shotwrap").innerHTML=`<img class="shot" src="data:image/jpeg;base64,${j.image}">
      <div class="note">#${j.id} &middot; ${j.timestamp}</div>`;
    toast("image #"+j.id+" captured");
    loadThumbs();
  }catch(e){ toast("capture failed: "+e.message); }
  b.disabled=false; b.textContent="Capture image";
};
async function loadThumbs(){
  try{
    const j=await (await fetch("/get_photo_list")).json();
    $("thumbs").innerHTML = (j.ids||[]).length
      ? j.ids.map(i=>`<button onclick="showShot(${i})">#${i}</button>`).join("")
      : `<span class="note">no stored images</span>`;
  }catch(e){}
}
async function showShot(id){
  try{
    const j=await (await fetch("/get_photo_by_id?id="+id)).json();
    $("shotwrap").innerHTML=`<img class="shot" src="data:image/jpeg;base64,${j.image}">
      <div class="note">#${j.id} &middot; ${j.timestamp}</div>`;
  }catch(e){ toast("could not load image"); }
}

/* -------------------------------------------------------------------- logs */
async function loadLogs(){
  try{
    const j=await (await fetch("/get_log_list")).json();
    const f=j.files||[];
    $("logs").innerHTML = f.length ? f.map(x=>{
      const nm = x.name || x;
      const sz = x.size!==undefined ? (x.size/1024).toFixed(1)+" kB" : "";
      return `<li><a href="/download_log?file=${encodeURIComponent(nm)}">${nm}</a><span>${sz}</span></li>`;
    }).join("") : "<li>no mission files</li>";
  }catch(e){ $("logs").innerHTML="<li>unavailable</li>"; }
}

function frame(){ draw(); requestAnimationFrame(frame); }
poll(); loadThumbs(); loadLogs(); frame();
setInterval(poll, 1000);
</script>
</body>
</html>
)MYSATUI";

#endif  // WEB_UI_H
