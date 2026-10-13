// Chamber web dashboard (web UI overhaul, phase 2). Served by the chamber itself as /0.js
// (web_server js_include), so it works with no internet. It replaces ESPHome's stock page and
// mirrors the iOS app: the same palette, tabs, health rules and safety confirmations.
// It speaks only the documented LAN API (docs/device-api.md): SSE /events plus POST commands.
(() => {
  'use strict';

  // ---- Style: the app's Theme palette (apple/AutoMyco/ShroomChamber/App/Theme.swift), light and dark ----
  const CSS = `
:root{--bg:#F3EDE2;--card:#FFFBF5;--stroke:#E6DCCB;--bark:#4E3B2C;--soft:#8A735E;--moss:#5C7A37;
--terra:#BF5F36;--water:#3B7A8A;--spore:#B98319;--danger:#AE3B2A;--track:#E9E0D1;--uvc:#6B45B0;--on:#fff;color-scheme:light dark}
@media (prefers-color-scheme:dark){:root{--bg:#16130F;--card:#241E18;--stroke:#3A3128;--bark:#EADBC6;--soft:#B4A08A;
--moss:#A3C373;--terra:#EC9466;--water:#82C0CF;--spore:#E7BC5E;--danger:#EF7A64;--track:#352D25;--uvc:#B99CF0;--on:#16130F}}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--bark);
font:17px/1.35 -apple-system,BlinkMacSystemFont,"SF Pro Text","Segoe UI",Roboto,system-ui,sans-serif;-webkit-text-size-adjust:100%}
main{max-width:640px;margin:0 auto;padding:16px 16px 120px}
header{max-width:640px;margin:0 auto;padding:20px 16px 0;display:flex;align-items:flex-end;justify-content:space-between;gap:12px}
h1{font-size:34px;line-height:1.1;margin:0;font-weight:800;letter-spacing:-.02em}
.pill{display:inline-flex;align-items:center;gap:6px;padding:6px 12px;border-radius:99px;background:var(--card);
border:1px solid var(--stroke);font-size:14px;font-weight:600;color:var(--moss);white-space:nowrap}
.pill i{width:8px;height:8px;border-radius:50%;background:currentColor}
.pill.off{color:var(--spore)}
.card{background:var(--card);border:1px solid var(--stroke);border-radius:22px;padding:18px;margin:0 0 14px}
.label{font-size:13px;font-weight:600;letter-spacing:.08em;text-transform:uppercase;color:var(--soft);margin:22px 4px 8px;display:flex;gap:8px;align-items:center}
.card .label{margin:0 0 6px}
.row{display:flex;align-items:center;justify-content:space-between;gap:12px;min-height:44px}
.row+.row{border-top:1px solid var(--stroke);margin-top:8px;padding-top:8px}
.grow{display:flex;align-items:center;gap:14px;margin:4px 0 14px}
.cap{width:64px;height:64px;border-radius:50%;background:var(--card);border:1px solid var(--stroke);display:grid;place-items:center;flex:none}
.grow .who{flex:1;min-width:0}
.grow small{color:var(--soft);font-weight:600;font-size:14px}
.grow b{display:block;font-size:24px;line-height:1.15;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.chip{display:inline-block;margin-top:4px;padding:2px 10px;border-radius:99px;font-size:13px;font-weight:600;
color:var(--c,var(--moss));background:color-mix(in srgb,var(--c,var(--moss)) 16%,transparent)}
.day{flex:none;text-align:center;padding:8px 16px;border-radius:18px;background:color-mix(in srgb,var(--moss) 14%,transparent);
border:1px solid color-mix(in srgb,var(--moss) 30%,transparent)}
.day small{display:block;font-size:12px;letter-spacing:.1em;font-weight:700;color:var(--soft)}
.day b{font-size:30px;line-height:1;color:var(--moss)}
.health{display:flex;gap:14px;align-items:flex-start}
.health .dot{flex:none;width:36px;height:36px;border-radius:50%;display:grid;place-items:center;color:var(--c);
background:color-mix(in srgb,var(--c) 18%,transparent);font-weight:800}
.health b{font-size:20px}
.health p,.note{margin:2px 0 0;color:var(--soft);font-size:15px}
.big{font-size:52px;font-weight:700;letter-spacing:-.03em;line-height:1;font-variant-numeric:tabular-nums}
.big sup{font-size:20px;color:var(--soft);font-weight:600;margin-left:2px;vertical-align:top;position:relative;top:6px}
.target{text-align:right;font-size:15px;color:var(--soft)}
.target b{display:block;font-size:17px;color:var(--c,var(--moss))}
.spark{display:block;width:100%;height:44px;margin-top:10px}
.banner{display:flex;gap:12px;align-items:center;border-radius:22px;padding:14px 16px;margin:0 0 14px;
background:color-mix(in srgb,var(--c) 14%,var(--card));border:1px solid color-mix(in srgb,var(--c) 45%,transparent)}
.banner b{display:block}.banner div{flex:1}.banner span{font-size:15px;color:var(--soft)}
button,.btn{font:inherit;font-weight:600;border:0;border-radius:99px;padding:10px 16px;cursor:pointer;
background:color-mix(in srgb,var(--moss) 16%,transparent);color:var(--moss);min-height:40px}
button.fill{background:var(--moss);color:var(--on)}
button.warn{background:color-mix(in srgb,var(--danger) 14%,transparent);color:var(--danger)}
button.uv{background:var(--uvc);color:var(--on)}
button:disabled{opacity:.45;cursor:default}
.btns{display:flex;flex-wrap:wrap;gap:8px;margin-top:12px}
.sw{appearance:none;-webkit-appearance:none;width:51px;height:31px;border-radius:99px;background:var(--track);position:relative;
flex:none;cursor:pointer;transition:background .2s;margin:0}
.sw:after{content:"";position:absolute;top:2px;left:2px;width:27px;height:27px;border-radius:50%;background:#fff;
box-shadow:0 2px 4px #0003;transition:transform .2s}
.sw:checked{background:var(--moss)}.sw:checked:after{transform:translateX(20px)}
.sw.uv:checked{background:var(--uvc)}
.sw:disabled{opacity:.45}
input[type=range]{width:100%;accent-color:var(--moss);margin:8px 0 2px;height:28px}
select,input[type=text]{font:inherit;color:var(--bark);background:var(--bg);border:1px solid var(--stroke);border-radius:12px;padding:8px 10px;max-width:60%}
input[type=text]{max-width:none;width:100%}
.val{font-variant-numeric:tabular-nums;font-weight:600;color:var(--moss)}
.val.dim{color:var(--soft)}
.sub{display:block;font-size:14px;color:var(--soft)}
.sub.warn{color:var(--spore)}
.step{display:flex;align-items:center;gap:6px}
.step button{width:40px;padding:0;font-size:22px}
.step .val{min-width:64px;text-align:center}
.tiles{display:grid;grid-template-columns:1fr 1fr;gap:14px}
.tiles .card{margin:0}
.swatches{display:flex;flex-wrap:wrap;gap:10px;margin-top:10px}
.swatches button{width:40px;height:40px;padding:0;border:2px solid var(--stroke)}
.glow{height:10px;border-radius:99px;margin:6px 0 4px}
.log{font:13px/1.5 ui-monospace,Menlo,monospace;white-space:pre-wrap;color:var(--soft);margin:0;max-height:260px;overflow:auto}
.uvcard{border-color:color-mix(in srgb,var(--uvc) 45%,transparent)}
.uvcard .label{color:var(--uvc)}
details summary{cursor:pointer;color:var(--soft);font-weight:600;padding:6px 4px;list-style:none}
details summary::-webkit-details-marker{display:none}
nav{position:fixed;left:50%;transform:translateX(-50%);bottom:max(12px,env(safe-area-inset-bottom));width:min(560px,calc(100% - 24px));
display:flex;justify-content:space-around;padding:6px;border-radius:99px;background:color-mix(in srgb,var(--card) 88%,transparent);
border:1px solid var(--stroke);-webkit-backdrop-filter:blur(16px);backdrop-filter:blur(16px);box-shadow:0 8px 30px #0002;z-index:5}
nav a{flex:1;display:flex;flex-direction:column;align-items:center;gap:2px;padding:6px 0;border-radius:99px;color:var(--bark);
text-decoration:none;font-size:12px;font-weight:600}
nav a.on{color:var(--moss);background:color-mix(in srgb,var(--moss) 12%,transparent)}
nav svg,.label svg{width:22px;height:22px}
.label svg{width:16px;height:16px}
#toast{position:fixed;left:50%;bottom:100px;transform:translate(-50%,20px);opacity:0;transition:.25s;background:var(--bark);color:var(--bg);
padding:10px 16px;border-radius:99px;font-size:15px;font-weight:600;pointer-events:none;z-index:6;max-width:calc(100% - 32px)}
#toast.show{opacity:1;transform:translate(-50%,0)}
@media (max-width:380px){.big{font-size:44px}.tiles{grid-template-columns:1fr}}
`;

  // ---- Icons (simple strokes, close to the app's SF Symbols) ----
  const I = {
    home: '<path d="M4 20V10l8-6 8 6v10h-5v-6H9v6z"/>',
    grow: '<path d="M5 19c0-8 5-13 15-14-1 10-6 15-14 15M5 19c3-4 6-7 10-9"/>',
    lights: '<path d="M9 18h6M10 21h4M12 3a6 6 0 0 0-3.5 10.9c.6.5 1 1.2 1 2V16h5v-.1c0-.8.4-1.5 1-2A6 6 0 0 0 12 3z"/>',
    controls: '<path d="M4 7h10M18 7h2M4 17h4M12 17h8"/><circle cx="16" cy="7" r="2"/><circle cx="10" cy="17" r="2"/>',
    device: '<rect x="6" y="6" width="12" height="12" rx="2"/><path d="M9 2v4M15 2v4M9 18v4M15 18v4M2 9h4M2 15h4M18 9h4M18 15h4"/>',
    temp: '<path d="M10 14V5a2 2 0 0 1 4 0v9a4 4 0 1 1-4 0z"/>',
    drop: '<path d="M12 3s6 7 6 11a6 6 0 0 1-12 0c0-4 6-11 6-11z"/>',
    air: '<path d="M3 8h11a3 3 0 1 0-3-3M3 16h15a3 3 0 1 1-3 3M3 12h17"/>',
    uv: '<circle cx="12" cy="12" r="4"/><path d="M12 2v3M12 19v3M2 12h3M19 12h3M5 5l2 2M17 17l2 2M5 19l2-2M17 7l2-2"/>',
  };
  const icon = (k) => `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">${I[k]}</svg>`;

  // ---- Entity state from SSE, keyed "domain/Name" ----
  const E = {};
  const hist = { Temperature: [], Humidity: [], CO2: [] };
  let title = 'Chamber';
  let live = false;
  let everConnected = false;

  const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);
  const get = (k) => E[k];
  const has = (k) => k in E;
  const num = (k) => { const v = E[k]?.value; return typeof v === 'number' && isFinite(v) ? v : null; };
  const on = (k) => { const e = E[k]; return !!e && (e.value === true || e.state === 'ON'); };
  const text = (k) => { const s = E[k]?.state; return s && s !== 'NA' && s !== 'unknown' ? s : ''; };
  const fmt = (v, d = 0) => (v == null ? '—' : v.toFixed(d));

  function keyOf(d) {
    const id = String(d.id || '');
    const domain = id.includes('/') ? id.split('/')[0] : id.split('-')[0];
    const name = d.name || (id.includes('/') ? id.slice(domain.length + 1) : id.slice(domain.length + 1));
    return [domain, name];
  }

  function connect() {
    const es = new EventSource('/events');
    es.addEventListener('open', () => { live = true; everConnected = true; schedule(); });
    es.addEventListener('error', () => { live = false; schedule(); });
    es.addEventListener('ping', (ev) => {
      live = true;
      try { const c = JSON.parse(ev.data); if (c && c.title) { title = c.title; document.title = c.title; } } catch (_) { /* plain ping */ }
      schedule();
    });
    es.addEventListener('state', (ev) => {
      let d;
      try { d = JSON.parse(ev.data); } catch (_) { return; }
      const [domain, name] = keyOf(d);
      const k = `${domain}/${name}`;
      E[k] = Object.assign(E[k] || { domain, name }, d, { domain, name });
      if (domain === 'sensor' && hist[name] && typeof d.value === 'number' && isFinite(d.value)) {
        const h = hist[name];
        h.push(d.value);
        if (h.length > 180) h.shift();
      }
      schedule();
    });
  }

  // ---- Commands ----
  let toastTimer;
  function toast(msg, ms = 2200) {
    const t = document.getElementById('toast');
    t.textContent = msg;
    t.classList.add('show');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => t.classList.remove('show'), ms);
  }
  async function cmd(k, action, params) {
    const [domain, ...rest] = k.split('/');
    const name = rest.join('/');
    const qs = params ? '?' + new URLSearchParams(params).toString() : '';
    try {
      // Empty body so the request carries Content-Length: 0 (the device answers 411 without it).
      const r = await fetch(`/${domain}/${encodeURIComponent(name)}/${action}${qs}`,
        { method: 'POST', body: '', headers: { 'X-Requested-With': 'dashboard' } });
      if (r.status === 403 || r.status === 503) {
        // UV-C on and Camera Reset Login need the paired app's X-AutoMyco-Key, which a browser doesn't
        // have (403), or the chamber is starting up (503): show the chamber's reason, not "couldn't reach".
        const why = await r.text();
        if (why.includes('X-AutoMyco-Key') || why.includes('starting up')) {
          lastHTML = ''; render();   // nothing changed, so no state event: put the switch back
          toast(why, 6000);
          return;
        }
      }
      if (!r.ok) throw new Error(r.status);
    } catch (e) {
      toast(`Couldn't reach the chamber (${name})`);
    }
  }

  // ---- Shared bits ----
  const STATES = {
    idle: ['Idle', 'moss'], humidifying: ['Humidifying', 'water'], topping_up: ['Topping up', 'water'],
    drying: ['Drying', 'moss'], purging: ['Purging', 'moss'], heating: ['Heating', 'terra'],
    fresh_air: ['Fresh air', 'moss'], low_water: ['Low water', 'spore'], sensor_drying: ['Sensor drying', 'soft'],
    sensor_error: ['Sensor error', 'danger'], paused: ['Paused', 'soft'],
  };
  const TINTS = {
    "Shiitake": '#7A5236', "Lion's Mane": '#D9C7A5', "Reishi": '#9C3D22', "Pink Oyster": '#D86C8A',
    "Golden Oyster": '#D7A220', "Blue Oyster": '#5B7896', "Chestnut": '#7A5236', "King Trumpet": '#B8996F',
    "Italian Oyster": '#B8996F', "Snow Oyster": '#D9C7A5', "Black King": '#3B3330', "Pioppino": '#7A5236', "Nameko": '#D7A220',
  };
  const mushroom = (c) => `<svg viewBox="0 0 40 40" width="40" height="40"><path d="M4 20a16 13 0 0 1 32 0z" fill="${c}"/><rect x="15" y="20" width="10" height="13" rx="4" fill="${c}" opacity=".75"/></svg>`;

  function sparkline(arr, color) {
    if (arr.length < 2) return '';
    const lo = Math.min(...arr), hi = Math.max(...arr), span = hi - lo || 1;
    const pts = arr.map((v, i) => `${(i / (arr.length - 1)) * 300},${40 - ((v - lo) / span) * 36 - 2}`).join(' ');
    return `<svg class="spark" viewBox="0 0 300 40" preserveAspectRatio="none"><polyline points="${pts}" fill="none" stroke="var(--${color})" stroke-width="2" vector-effect="non-scaling-stroke" stroke-linejoin="round"/></svg>`;
  }

  function sw(k, opts = {}) {
    if (!has(k)) return '';
    return `<input type="checkbox" class="sw${opts.uv ? ' uv' : ''}" data-sw="${esc(k)}" ${on(k) ? 'checked' : ''} ${opts.disabled ? 'disabled' : ''} aria-label="${esc(k.split('/')[1])}">`;
  }
  function switchRow(k, label, sub, opts) {
    if (!has(k)) return '';
    return `<div class="row"><div>${esc(label)}${sub ? `<span class="sub${opts?.warnSub ? ' warn' : ''}">${esc(sub)}</span>` : ''}</div>${sw(k, opts)}</div>`;
  }
  // Load checks (docs/device-api.md): `binary_sensor/<Load> Not Detected` ON = switched on, drew no current.
  const LOADS = [['Fan 1', 'Fan 1 (exhaust)'], ['Fan 2', 'Fan 2 (intake)'], ['Blower', 'Blower'], ['Atomizer', 'Atomizer'], ['Heater', 'Heater']];
  const missing = (name) => on(`binary_sensor/${name} Not Detected`);
  const MISSING = 'Not detected: it drew no current. Check its plug.';
  function fanRow(k, label) {
    if (!has(k)) return '';
    const e = get(k);
    const lvl = on(k) ? (e.speed_level ?? 100) : 0;
    const gone = missing(k.split('/')[1]);
    return `<div class="row" style="flex-wrap:wrap"><div>${esc(label)}${gone ? `<span class="sub warn">${MISSING}</span>` : ''}</div><span class="val${lvl ? '' : ' dim'}">${lvl ? lvl + '%' : 'Off'}</span>
<input type="range" min="0" max="100" step="5" value="${lvl}" data-fan="${esc(k)}" aria-label="${esc(label)} speed"></div>`;
  }
  function stepper(k, label, unit, sub) {
    if (!has(k)) return '';
    const e = get(k);
    const v = num(k);
    const step = e.step || 1;
    const d = step < 1 ? 1 : 0;
    return `<div class="row"><div>${esc(label)}${sub ? `<span class="sub">${esc(sub)}</span>` : ''}</div><div class="step">
<button data-num="${esc(k)}" data-d="${-step}" aria-label="Decrease ${esc(label)}">−</button>
<span class="val">${fmt(v, d)}${unit ? ' ' + esc(unit) : ''}</span>
<button data-num="${esc(k)}" data-d="${step}" aria-label="Increase ${esc(label)}">+</button></div></div>`;
  }
  function selectRow(k, label) {
    if (!has(k)) return '';
    const e = get(k);
    const opts = (e.option || []).map((o) => `<option ${o === e.state ? 'selected' : ''}>${esc(o)}</option>`).join('');
    return `<div class="row"><div>${esc(label)}</div><select data-sel="${esc(k)}" aria-label="${esc(label)}">${opts}</select></div>`;
  }
  const section = (ic, name) => `<div class="label">${ic ? icon(ic) : ''}${esc(name)}</div>`;

  // ---- Health: same rules as the app's ChamberHealth ----
  function health() {
    const a = [];
    const state = text('text_sensor/Grow State');
    const cc = has('switch/Climate Control') ? on('switch/Climate Control') : true;
    if (!live) a.push(['warning', everConnected ? 'Reconnecting' : 'Connecting', 'Showing the last readings received. The chamber keeps running on its own.']);
    const fault = state === 'sensor_error' || (has('sensor/Temperature') && num('sensor/Temperature') == null && state !== 'sensor_drying');
    if (fault) a.push(['critical', 'Sensor fault', "The temperature and humidity sensor isn't reporting. The heater is cut off for safety until it recovers."]);
    if (on('binary_sensor/Low Water') || state === 'low_water') a.push(['critical', 'Low water', "Refill the reservoir. Misting is paused until it's topped up."]);
    const gone = LOADS.filter(([n]) => missing(n)).map(([, t]) => t);
    if (gone.length) {
      const one = gone.length === 1;
      a.push(['warning', one ? `${gone[0]} not detected` : `${gone.length} loads not detected`,
        `${gone.join(', ')} ${one ? 'was' : 'were'} switched on but drew no current, so ${one ? 'it' : 'they'} may be unplugged or broken. Check ${one ? 'its plug' : 'their plugs'} on the main board, then run the self-test.`]);
    }
    if (!cc || state === 'paused') a.push(['info', 'Climate Control paused', 'The atomizer, heater and fans stay exactly as you set them.']);
    if (!fault && cc) {
      const t = num('sensor/Temperature'), tt = num('number/Temperature Setpoint');
      const h = num('sensor/Humidity'), ht = num('number/Humidity Setpoint');
      if (t != null && tt != null) {
        if (t > tt + 4) a.push(['warning', 'Warmer than target', `${fmt(t - tt, 1)}°F above ${fmt(tt)}°F. The chamber can't cool below room temperature.`]);
        else if (t < tt - 3) a.push(['warning', 'Cooler than target', `${fmt(tt - t, 1)}°F below ${fmt(tt)}°F.`]);
      }
      if (h != null && ht != null) {
        if (h < ht - 10) a.push(['warning', 'Drier than target', `${fmt(ht - h)}% below ${fmt(ht)}%RH.`]);
        else if (h > ht + 8) a.push(['warning', 'More humid than target', `${fmt(h - ht)}% above ${fmt(ht)}%RH.`]);
      }
    }
    const rank = { critical: 2, warning: 1, info: 0 };
    a.sort((x, y) => rank[y[0]] - rank[x[0]]);
    return a;
  }

  function waterBanner() {
    const hard = on('switch/Atomizer Hard Stop');
    const empty = on('binary_sensor/Tank Empty Alert');
    const low = on('binary_sensor/Low Water');
    if (!hard && !empty && !low) return '';
    const [t, s] = hard ? ['Tank empty? Misting stopped', 'Refill the reservoir, then clear the Atomizer Hard Stop.']
      : empty ? ['Tank looks empty', "Misting isn't raising humidity. Refill the reservoir."]
      : ['Low water', "Refill the reservoir. Misting is paused until it's topped up."];
    return `<div class="banner" style="--c:var(--spore)">${icon('drop')}<div><b>${t}</b><span>${s}</span></div>${hard ? '<button class="fill" data-clear-hard>Clear</button>' : ''}</div>`;
  }

  // ---- Views ----
  function viewHome() {
    const preset = text('select/Grow Preset');
    const name = text('text/Grow Name');
    const st = STATES[text('text_sensor/Grow State')];
    const day = num('sensor/Grow Day');
    const alerts = health();
    const lvl = alerts[0]?.[0];
    const hc = lvl === 'critical' ? 'danger' : lvl === 'warning' ? 'spore' : 'moss';
    const head = lvl === 'critical' ? 'Needs attention' : lvl === 'warning' ? 'Keep an eye on it' : 'All good';
    const detail = alerts.length ? alerts.map((x) => x[1] + '. ' + x[2]).join(' ') : 'Temperature and humidity are on target.';

    const tile = (k, label, ic, unit, target, color, d) => {
      if (!has(k)) return '';
      const v = num(k), tv = target ? num(target) : null;
      let status = '';
      if (v != null && tv != null) {
        const off = v - tv;
        status = Math.abs(off) <= (k.endsWith('Temperature') ? 2 : 5) ? 'On target' : off > 0 ? 'Above target' : 'Below target';
      }
      return `<div class="card"><div class="label">${icon(ic)}${label}</div><div class="row">
<div class="big">${fmt(v, d)}<sup>${unit}</sup></div>
${tv != null ? `<div class="target" style="--c:var(--${status === 'On target' ? 'moss' : 'spore'})"><b>${status}</b>Target ${fmt(tv)} ${unit}</div>` : ''}</div>
${sparkline(hist[k.split('/')[1]] || [], color)}</div>`;
    };

    const mist = text('text_sensor/Mist Status');
    return `${waterBanner()}
<div class="grow"><div class="cap">${mushroom(TINTS[preset] || '#B8996F')}</div>
<div class="who"><small>${esc(name || 'No grow running')}</small><b>${esc(preset || '—')}</b>
${st ? `<span class="chip" style="--c:var(--${st[1]})">${st[0]}</span>` : ''}</div>
${day != null ? `<div class="day"><small>DAY</small><b>${fmt(day)}</b></div>` : ''}</div>
<div class="card health" style="--c:var(--${hc})"><div class="dot">${lvl === 'critical' || lvl === 'warning' ? '!' : '✓'}</div>
<div><b>${head}</b><p>${esc(detail)}</p></div></div>
${tile('sensor/Temperature', 'Temperature', 'temp', '°F', 'number/Temperature Setpoint', 'terra', 1)}
${tile('sensor/Humidity', 'Humidity', 'drop', '%', 'number/Humidity Setpoint', 'water', 0)}
${has('sensor/CO2') && num('sensor/CO2') != null ? tile('sensor/CO2', 'CO₂', 'air', 'ppm', null, 'moss', 0) : ''}
<div class="card"><div class="label">${icon('air')}Right now</div>
${mist ? `<div class="row"><div>Mister</div><span class="val dim">${esc(mist)}</span></div>` : ''}
<div class="row"><div>Fans</div><span class="val dim">${['fan/Fan 1', 'fan/Fan 2', 'fan/Blower'].filter(has).map((k) => `${k.split('/')[1].replace('Fan ', 'F')} ${on(k) ? (get(k).speed_level ?? 100) + '%' : 'off'}`).join(' · ')}</span></div>
<div class="row"><div>Heater</div><span class="val dim">${on('switch/Heater') ? 'On' : 'Off'}</span></div>
${has('button/Fresh Air Now') ? '<div class="btns"><button data-press="button/Fresh Air Now">Fresh air now</button></div>' : ''}</div>`;
  }

  function viewGrow() {
    const growing = num('sensor/Grow Day') != null;
    return `${waterBanner()}
${section('grow', 'Grow')}
<div class="card">${selectRow('select/Grow Preset', 'Species')}
${has('text/Grow Name') ? `<div class="row" style="flex-wrap:wrap"><div style="width:100%">Grow name</div><input type="text" maxlength="40" data-text="text/Grow Name" value="${esc(text('text/Grow Name'))}" placeholder="e.g. Blue Oyster #3"></div>` : ''}
<div class="btns">${growing
      ? `${has('button/End Grow') ? '<button class="warn" data-press="button/End Grow" data-confirm="End this grow? The day counter stops and the grow is archived.">End grow</button>' : ''}`
      : `${has('button/Start Grow') ? '<button class="fill" data-press="button/Start Grow">Start grow</button>' : ''}`}</div>
<p class="note">${growing ? `Day ${fmt(num('sensor/Grow Day'))} of this grow.` : 'Starting a grow begins the day counter and turns Climate Control on.'}</p></div>
${section('temp', 'Targets')}
<div class="card">${stepper('number/Humidity Setpoint', 'Humidity', '%')}
${stepper('number/Temperature Setpoint', 'Temperature', '°F', "The heater warms; it can't cool below the room.")}
${stepper('number/Light Hours', 'Light hours', 'h', 'Grow light on from 08:00')}
${stepper('number/Fresh Air Interval', 'Fresh air every', 'min')}
${stepper('number/Fresh Air Duration', 'Fresh air for', 's')}
<p class="note">Changing a target switches Species to Custom.</p></div>
<details><summary>Advanced climate ›</summary><div class="card">
${stepper('number/Mist Burst', 'Mist burst', 's')}
${stepper('number/RH Soft Ceiling', 'Stop misting above', '%')}
${stepper('number/RH Soft Release', 'Resume misting below', '%')}
${selectRow('select/Fresh Air Mode', 'Fresh air mode')}
${selectRow('select/Intake Fan', 'Intake fan')}
${selectRow('select/Water Sensing', 'Water sensing')}</div></details>`;
  }

  const SWATCHES = [[255, 255, 255], [255, 214, 170], [255, 160, 60], [255, 60, 40], [255, 90, 160], [150, 80, 255], [60, 120, 255], [60, 220, 200], [90, 220, 90]];
  function lightCard(k, label, sub) {
    if (!has(k)) return '';
    const e = get(k);
    const c = e.color || {};
    const rgb = [c.r, c.g, c.b].map((x) => Math.max(0, Math.min(255, Number(x) || 0))).join(',');
    const lit = on(k);
    const pct = Math.round(((e.brightness ?? 255) / 255) * 100);
    const effects = e.effects || e.effect_list || [];
    return `<div class="card"><div class="row"><div><b>${esc(label)}</b><span class="sub">${esc(sub)}</span></div>${sw(k)}</div>
<div class="glow" style="background:${lit ? `rgb(${e.color ? rgb : '255,255,255'})` : 'var(--track)'};opacity:${lit ? 0.35 + pct / 160 : 1}"></div>
<div class="row" style="flex-wrap:wrap"><div>Brightness</div><span class="val${lit ? '' : ' dim'}">${lit ? pct + '%' : 'Off'}</span>
<input type="range" min="0" max="100" step="5" value="${lit ? pct : 0}" data-bri="${esc(k)}" aria-label="${esc(label)} brightness"></div>
<div class="swatches">${SWATCHES.map(([r, g, b]) => `<button style="background:rgb(${r},${g},${b})" data-rgb="${esc(k)}" data-c="${r},${g},${b}" aria-label="Colour ${r},${g},${b}"></button>`).join('')}</div>
${effects.length > 1 ? `<div class="row" style="margin-top:8px"><div>Effect</div><select data-effect="${esc(k)}" aria-label="${esc(label)} effect">${effects.map((x) => `<option ${x === (e.effect || 'None') ? 'selected' : ''}>${esc(x)}</option>`).join('')}</select></div>` : ''}</div>`;
  }
  function viewLights() {
    return `${section('lights', 'Lights')}
${lightCard('light/Top Light', 'Grow light', `Light Hours schedule: ${fmt(num('number/Light Hours'))} h a day from 08:00`)}
${lightCard('light/Bottom Light', 'Base rim', 'Accent strip around the base')}`;
  }

  function viewControls() {
    const cc = on('switch/Climate Control');
    const hard = on('switch/Atomizer Hard Stop');
    const low = on('binary_sensor/Low Water');
    const uvOn = on('switch/UV-C Lamp');
    const rem = num('sensor/UV-C Remaining');
    return `${waterBanner()}
<div class="card health" style="--c:var(--${cc ? 'moss' : 'spore'})"><div class="dot">${cc ? '✓' : 'Ⅱ'}</div>
<div class="row" style="flex:1;align-items:flex-start"><div><b>Climate Control is ${cc ? 'on' : 'off'}</b>
<p>${cc ? "Switching the atomizer, heater or a fan by hand pauses Climate Control, so it won't undo your change." : 'The atomizer, heater and fans stay exactly as you set them.'}</p></div>${sw('switch/Climate Control')}</div></div>
${section('temp', 'Climate')}
<div class="card">${switchRow('switch/Atomizer', 'Atomizer', hard ? 'Blocked: Atomizer Hard Stop' : low ? 'Blocked: low water' : missing('Atomizer') ? MISSING : '', { warnSub: true })}
${switchRow('switch/Heater', 'Heater', missing('Heater') ? MISSING : '', { warnSub: true })}</div>
${section('air', 'Air')}
<div class="card">${fanRow('fan/Fan 1', 'Fan 1 (exhaust)')}${fanRow('fan/Fan 2', 'Fan 2 (intake)')}${fanRow('fan/Blower', 'Blower')}</div>
${has('switch/UV-C Lamp') ? `${section('uv', 'UV-C lamp')}
<div class="card uvcard"><div class="row"><div>UV-C lamp<span class="sub${low ? ' warn' : ''}">${low ? 'Unavailable while the water is low' : uvOn ? `On · switches off in ${Math.floor((rem || 0) / 60)}:${String(Math.round(rem || 0) % 60).padStart(2, '0')}` : 'Eye and skin hazard. Close the chamber first.'}</span></div>
${sw('switch/UV-C Lamp', { uv: true, disabled: low && !uvOn })}</div>
${stepper('number/UV-C Auto-Off', 'Auto-off after', 'min')}</div>` : ''}
${has('switch/Camera Power') ? `${section('', 'Camera')}<div class="card">${switchRow('switch/Camera Power', 'Camera power')}</div>` : ''}`;
  }

  function viewDevice() {
    const fw = text('text_sensor/Firmware Version');
    const log = text('text_sensor/Event Log');
    const st = text('text_sensor/Self-Test Result');
    const r = (label, k, d, unit) => has(k) ? `<div class="row"><div>${label}</div><span class="val dim">${fmt(num(k), d)} ${unit}</span></div>` : '';
    const all = Object.keys(E).sort().map((k) => {
      const e = E[k];
      return `<div class="row"><div style="min-width:0">${esc(e.name)}<span class="sub">${esc(e.domain)}</span></div><span class="val dim" style="text-align:right;word-break:break-word">${esc(e.state ?? (e.value === true ? 'ON' : e.value === false ? 'OFF' : e.value ?? ''))}</span></div>`;
    }).join('');
    return `${section('device', 'Chamber')}
<div class="card"><div class="row"><div>Firmware</div><span class="val dim">${esc(fw || '—')}</span></div>
${r('Wi-Fi signal', 'sensor/WiFi Signal', 0, 'dBm')}${r('Supply', 'sensor/Supply Voltage', 1, 'V')}
${r('Load', 'sensor/Load Power', 1, 'W')}
${has('binary_sensor/Water Probe') ? `<div class="row"><div>Water probe</div><span class="val dim">${on('binary_sensor/Water Probe') ? 'Wet' : 'Dry'}</span></div>` : ''}</div>
${section('', 'Self-test')}
<div class="card"><p class="note" style="margin:0">Runs each load briefly and checks its current draw. The UV-C lamp is never tested.</p>
${st ? `<pre class="log" style="margin-top:10px">${esc(st.replace(/;\s*/g, '\n'))}</pre>` : ''}
<div class="btns"><button data-press="button/Run Self-Test">Run self-test</button></div>
${switchRow('switch/Boot Self-Test', 'Test after every restart', 'About a minute, heater at its low stage. Between tests the chamber watches each load the controller switches on.')}</div>
${has('switch/Anonymous Check-in') ? `${section('', 'Help the project')}
<div class="card">${switchRow('switch/Anonymous Check-in', 'Anonymous check-in', 'Off unless you turn it on. Once a day the chamber sends automyco.com a random ID it made itself and its firmware version, so we can count chambers still growing. Turning it off stops it; the chamber keeps the ID so it is not counted twice.')}
${has('button/Reset Check-in ID') ? `<div class="btns"><button data-press="button/Reset Check-in ID" data-confirm="Erase the check-in ID? If check-in is on, the chamber carries on under a new random ID.">Reset check-in ID</button></div><p class="note">Erases the ID. If check-in is on, it carries on under a new random one.</p>` : ''}</div>` : ''}
${log ? `${section('', 'Activity')}<div class="card"><pre class="log">${esc(log)}</pre></div>` : ''}
${section('', 'Maintenance')}
<div class="card">${switchRow('switch/Atomizer Hard Stop', 'Atomizer hard stop', 'Blocks all misting. Turn off after refilling.')}
${switchRow('switch/Water Lockout Override', 'Water lockout override', 'Bench only. Turns itself off after 10 min.')}
<div class="btns">${has('button/Sensor Dry Pulse') ? '<button data-press="button/Sensor Dry Pulse">Sensor dry pulse</button>' : ''}
${has('button/Calibrate CO2 (fresh air)') ? '<button data-press="button/Calibrate CO2 (fresh air)" data-confirm="Calibrate CO₂ to fresh air? The sensor must have been in outdoor-fresh air for at least 3 minutes.">Calibrate CO₂</button>' : ''}
<button class="warn" data-press="button/Restart" data-confirm="Restart the chamber? It's back in about 10 seconds.">Restart</button></div></div>
<details><summary>All entities (${Object.keys(E).length}) ›</summary><div class="card">${all}</div></details>`;
  }

  const TABS = [['home', 'Dashboard', viewHome], ['grow', 'Grow', viewGrow], ['lights', 'Lights', viewLights],
    ['controls', 'Controls', viewControls], ['device', 'Device', viewDevice]];
  const tab = () => TABS.find((t) => '#' + t[0] === location.hash) || TABS[0];

  // ---- Render: rebuild the tab's HTML, but never under a finger or a focused field ----
  let dragging = false;
  let pending = false;
  let lastHTML = '';
  function schedule() {
    if (pending) return;
    pending = true;
    setTimeout(() => { pending = false; render(); }, 150);
  }
  function busy() {
    const a = document.activeElement;
    return dragging || (a && a.closest('main') && (a.tagName === 'SELECT' || a.type === 'text'));
  }
  function render() {
    const [id, name, view] = tab();
    document.getElementById('title').textContent = id === 'home' ? 'Chamber' : name;
    const p = document.getElementById('conn');
    p.className = 'pill' + (live ? '' : ' off');
    p.innerHTML = `<i></i>${live ? 'Live' : everConnected ? 'Reconnecting' : 'Connecting'}`;
    document.querySelectorAll('nav a').forEach((a) => a.classList.toggle('on', a.hash === '#' + id));
    if (busy()) { schedule(); return; }
    const html = view();
    if (html !== lastHTML) {
      lastHTML = html;
      const m = document.querySelector('main');
      const open = [...m.querySelectorAll('details')].map((d) => d.open);
      m.innerHTML = html;
      m.querySelectorAll('details').forEach((d, i) => { d.open = !!open[i]; });
    }
  }

  // ---- Events (delegated) ----
  function bind() {
    const m = document.querySelector('main');
    m.addEventListener('pointerdown', (e) => { if (e.target.type === 'range') dragging = true; });
    document.addEventListener('pointerup', () => { if (dragging) { dragging = false; schedule(); } });
    document.addEventListener('pointercancel', () => { dragging = false; });

    m.addEventListener('click', (e) => {
      const b = e.target.closest('button');
      if (!b) return;
      if (b.dataset.confirm && !confirm(b.dataset.confirm)) return;
      if (b.dataset.press) { cmd(b.dataset.press, 'press'); toast(b.textContent.trim()); }
      else if (b.dataset.num) {
        const k = b.dataset.num, e2 = get(k);
        const v = Math.min(e2.max_value ?? Infinity, Math.max(e2.min_value ?? -Infinity, (num(k) ?? 0) + Number(b.dataset.d)));
        e2.value = v; e2.state = String(v);   // optimistic, so repeated taps add up
        render();
        cmd(k, 'set', { value: v });
      } else if (b.dataset.rgb) {
        const [r, g, bl] = b.dataset.c.split(',');
        const e2 = get(b.dataset.rgb);
        cmd(b.dataset.rgb, 'turn_on', { r, g, b: bl, brightness: on(b.dataset.rgb) ? (e2.brightness ?? 255) : 200, effect: 'None' });
      } else if ('clearHard' in b.dataset) {
        cmd('switch/Atomizer Hard Stop', 'turn_off'); toast('Hard stop cleared');
      }
    });

    m.addEventListener('change', (e) => {
      const t = e.target, d = t.dataset;
      if (d.sw) {
        const k = d.sw;
        if (k === 'switch/UV-C Lamp' && t.checked) {
          const mins = fmt(num('number/UV-C Auto-Off'));
          if (!confirm(`Turn on the UV-C lamp?\n\nUV-C light burns eyes and skin. Close the chamber and keep it closed. The lamp switches itself off after ${mins} min.`)) { t.checked = false; return; }
        }
        cmd(k, t.checked ? 'turn_on' : 'turn_off');
      } else if (d.fan) {
        const v = Number(t.value);
        cmd(d.fan, v ? 'turn_on' : 'turn_off', v ? { speed_level: v } : null);
      } else if (d.bri) {
        const v = Number(t.value);
        cmd(d.bri, v ? 'turn_on' : 'turn_off', v ? { brightness: Math.round((v / 100) * 255) } : null);
      } else if (d.sel) {
        cmd(d.sel, 'set', { option: t.value }); t.blur();
      } else if (d.effect) {
        cmd(d.effect, 'turn_on', { effect: t.value }); t.blur();
      } else if (d.text) {
        cmd(d.text, 'set', { value: t.value.slice(0, 40) }); toast('Grow name saved');
      }
    });
    m.addEventListener('input', (e) => {
      const t = e.target;
      if (t.type !== 'range') return;
      const v = t.closest('.row').querySelector('.val');
      if (v) { v.textContent = Number(t.value) ? t.value + '%' : 'Off'; v.classList.toggle('dim', !Number(t.value)); }
    });
    m.addEventListener('keydown', (e) => { if (e.key === 'Enter' && e.target.type === 'text') e.target.blur(); });
    window.addEventListener('hashchange', () => { lastHTML = ''; window.scrollTo(0, 0); render(); });
  }

  // ---- Boot: replace the stock page ----
  function boot() {
    document.querySelector('esp-app')?.remove();
    const meta = document.createElement('meta');
    meta.name = 'viewport';
    meta.content = 'width=device-width,initial-scale=1,viewport-fit=cover';
    document.head.appendChild(meta);
    const theme = document.createElement('meta');
    theme.name = 'theme-color';
    theme.content = matchMedia('(prefers-color-scheme: dark)').matches ? '#16130F' : '#F3EDE2';
    document.head.appendChild(theme);
    const style = document.createElement('style');
    style.textContent = CSS;
    document.head.appendChild(style);
    document.title = title;
    document.body.innerHTML = `<header><h1 id="title">Chamber</h1><span class="pill off" id="conn"><i></i>Connecting</span></header>
<main></main>
<nav>${TABS.map(([id, name]) => `<a href="#${id}">${icon(id)}${name}</a>`).join('')}</nav>
<div id="toast" role="status" aria-live="polite"></div>`;
    bind();
    render();
    connect();
    setInterval(schedule, 1000);   // keeps the UV-C countdown and connection pill fresh
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot);
  else boot();
})();
