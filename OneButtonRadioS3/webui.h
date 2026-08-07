#pragma once
#include <Arduino.h>

/*
 * Both pages are single self-contained strings: no CDN, no external CSS, no
 * fonts, no favicon request. That is deliberate - the radio serves these over
 * the same WiFi link that is feeding the audio stream, so every extra request
 * is airtime stolen from the buffer. One GET, done.
 */

// ---------------------------------------------------------------- setup AP
// Shown when no WiFi could be joined. Collects credentials and reboots.
static const char PAGE_SETUP[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>naani radio setup</title><style>
:root{color-scheme:dark}
body{margin:0;padding:24px;background:#14140f;color:#efece2;
 font:16px/1.55 system-ui,-apple-system,sans-serif}
.w{max-width:420px;margin:0 auto}
h1{font-size:20px;font-weight:500;margin:0 0 4px}
p.sub{color:#9c9a90;margin:0 0 22px;font-size:14px}
label{display:block;font-size:13px;color:#9c9a90;margin:14px 0 5px}
input,select{width:100%;box-sizing:border-box;padding:11px 12px;font-size:16px;
 background:#1f1f18;color:#efece2;border:1px solid #3a3a30;border-radius:8px}
button{width:100%;margin-top:20px;padding:13px;font-size:16px;font-weight:500;
 background:#639922;color:#fff;border:0;border-radius:8px;cursor:pointer}
button:disabled{background:#3a3a30;color:#77756c}
.note{margin-top:18px;font-size:13px;color:#77756c}
.ok{margin-top:18px;padding:12px;background:#27500a;border-radius:8px;font-size:14px}
</style></head><body><div class="w">
<h1>naani radio</h1><p class="sub">No known network. Pick one to join.</p>
<form id="f">
<label for="s">Network</label>
<select id="s"><option value="">scanning…</option></select>
<label for="m">Or type the name</label>
<input id="m" placeholder="network name" autocapitalize="off" autocorrect="off">
<label for="p">Password</label>
<input id="p" type="password" placeholder="leave empty if open">
<button id="b" type="submit">Save and restart</button>
</form>
<div id="done" style="display:none" class="ok">Saved. Restarting — this page will
stop responding. If it does not reconnect, the network was wrong; power-cycle to
get this page back.</div>
<p class="note">2.4 GHz only. This board has no 5 GHz radio.</p>
</div><script>
function load(){fetch('/scan').then(r=>r.json()).then(j=>{
 if(!j.done){setTimeout(load,1200);return}
 const s=document.getElementById('s');s.innerHTML='';
 if(!j.nets.length){s.innerHTML='<option value="">none found</option>';return}
 j.nets.forEach(n=>{const o=document.createElement('option');
  o.value=n.ssid;o.textContent=n.ssid+'  ('+n.rssi+' dBm'+(n.open?', open':'')+')';
  s.appendChild(o)})}).catch(()=>setTimeout(load,2000))}
load();
document.getElementById('f').onsubmit=e=>{e.preventDefault();
 const ssid=document.getElementById('m').value||document.getElementById('s').value;
 if(!ssid){alert('Pick or type a network name');return}
 document.getElementById('b').disabled=true;
 fetch('/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
  body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(document.getElementById('p').value)})
 .then(()=>{document.getElementById('f').style.display='none';
  document.getElementById('done').style.display='block'})}
</script></body></html>)HTML";

// ------------------------------------------------------------- normal mode
// Tone controls, station URL, and live status.
static const char PAGE_MAIN[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>naani radio</title><style>
:root{color-scheme:dark}
body{margin:0;padding:24px;background:#14140f;color:#efece2;
 font:16px/1.55 system-ui,-apple-system,sans-serif}
.w{max-width:460px;margin:0 auto}
h1{font-size:20px;font-weight:500;margin:0 0 2px}
h2{font-size:14px;font-weight:500;margin:26px 0 10px;color:#9c9a90;
 border-bottom:1px solid #2c2c24;padding-bottom:6px}
.title{color:#97c459;font-size:15px;margin:0 0 2px;min-height:22px}
.meta{color:#77756c;font-size:13px;margin:0}
.row{display:flex;align-items:center;gap:12px;margin:12px 0}
.row label{width:58px;font-size:14px;color:#9c9a90}
.row input[type=range]{flex:1;accent-color:#639922}
.row .v{width:56px;text-align:right;font-variant-numeric:tabular-nums;
 font-size:14px;color:#efece2}
input[type=text]{width:100%;box-sizing:border-box;padding:10px 12px;font-size:15px;
 background:#1f1f18;color:#efece2;border:1px solid #3a3a30;border-radius:8px}
button{margin-top:10px;padding:10px 18px;font-size:15px;font-weight:500;
 background:#639922;color:#fff;border:0;border-radius:8px;cursor:pointer}
button.sec{background:#2c2c24;color:#c9c6bb}
.bar{height:6px;background:#2c2c24;border-radius:3px;overflow:hidden;margin-top:6px}
.bar i{display:block;height:100%;background:#639922;width:0;transition:width .4s}
.hint{font-size:12px;color:#77756c;margin:6px 0 0}
</style></head><body><div class="w">
<h1>naani radio</h1>
<p class="title" id="t">…</p>
<p class="meta" id="m">connecting</p>

<h2>Tone</h2>
<div class="row"><label>Bass</label><input type="range" id="b" min="-12" max="12" step="1" value="0"><span class="v" id="bv">0 dB</span></div>
<div class="row"><label>Mid</label><input type="range" id="d" min="-12" max="12" step="1" value="0"><span class="v" id="dv">0 dB</span></div>
<div class="row"><label>Treble</label><input type="range" id="h" min="-12" max="12" step="1" value="0"><span class="v" id="hv">0 dB</span></div>
<button class="sec" onclick="flat()">Flat</button>
<p class="hint">Boosting costs headroom — on a small speaker, cutting a band you
dislike usually sounds better than boosting one you want.</p>

<h2>Buffer</h2>
<div class="bar"><i id="bf"></i></div>
<p class="hint" id="bt">—</p>

<h2>Station</h2>
<input type="text" id="u" placeholder="http://…">
<button onclick="setUrl()">Change station</button>
<p class="hint">Must be a direct stream URL. Throughput is capped by
window/RTT — at 128 kbps the server needs to be under ~300 ms away.</p>
</div><script>
const $=i=>document.getElementById(i);
let touched=false;
function fmt(v){return (v>0?'+':'')+v+' dB'}
function push(){touched=true;
 $('bv').textContent=fmt(+$('b').value);$('dv').textContent=fmt(+$('d').value);
 $('hv').textContent=fmt(+$('h').value);
 fetch('/tone',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
  body:`bass=${$('b').value}&mid=${$('d').value}&treble=${$('h').value}`})}
['b','d','h'].forEach(i=>{$(i).oninput=()=>{
 $(i+'v').textContent=fmt(+$(i).value);touched=true};$(i).onchange=push});
function flat(){$('b').value=0;$('d').value=0;$('h').value=0;push()}
function setUrl(){const u=$('u').value.trim();if(!u)return;
 fetch('/station',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
  body:'url='+encodeURIComponent(u)})}
function poll(){fetch('/status').then(r=>r.json()).then(j=>{
 $('t').textContent=j.title||'—';
 $('m').textContent=`${j.ip} · ${j.rssi} dBm · vol ${j.vol}/100 · ${j.br} kbps`;
 $('bf').style.width=Math.min(100,j.bufpct)+'%';
 $('bt').textContent=`${j.bufpct}% — ${j.bufsec.toFixed(1)} s of audio banked`;
 if(!touched){$('b').value=j.bass;$('d').value=j.mid;$('h').value=j.treble;
  $('bv').textContent=fmt(j.bass);$('dv').textContent=fmt(j.mid);
  $('hv').textContent=fmt(j.treble);}
 if(document.activeElement!==$('u'))$('u').value=j.url;
}).catch(()=>{}).finally(()=>setTimeout(poll,2000))}
poll();
</script></body></html>)HTML";
