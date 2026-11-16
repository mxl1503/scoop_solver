const RANKS = "23456789TJQKA";
const SUITS = "cdhs";
const GLYPH = { c: "♣", d: "♦", h: "♥", s: "♠" };
const PAIR_A = [0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3, 4];
const PAIR_B = [1, 2, 3, 4, 5, 2, 3, 4, 5, 3, 4, 5, 4, 5, 5];
const HDR = 64;
const REC = 119;

const PERMS = [];
(function gen(p, used) {
  if (p.length === 4) {
    PERMS.push(p.slice());
    return;
  }
  for (let s = 0; s < 4; s++) {
    if (used[s]) continue;
    used[s] = 1;
    p.push(s);
    gen(p, used);
    p.pop();
    used[s] = 0;
  }
})([], [0, 0, 0, 0]);

const params = new URLSearchParams(location.search);
const live = location.hostname === "localhost" || location.hostname === "127.0.0.1";
const dataRoot = params.get("data") || (live ? "../data/sixmax" : "../data/pages");
const FILTER_KEYS = {
  "f-pair": "pair",
  "f-high": "high",
  "f-pairrank": "pairrank",
  "f-has": "has",
  "f-suits": "suits",
  "f-he": "he",
  "f-plo": "plo",
  "f-tophe": "tophe",
  "f-place": "place",
  "f-call": "call",
  "f-sort": "sort",
};

let buf, u8, dv, ncanon = 0, completed = 0, deals = 0, boards = 0;
let summary = null;
const LOOKUP_PROMPT = "Choose or type six cards, then select Show splits.";

const $ = (id) => document.getElementById(id);

function cardStr(id) {
  return RANKS[id >> 2] + SUITS[id & 3];
}
function cardHtml(id) {
  const s = SUITS[id & 3];
  return `<span class="card"><span class="r">${RANKS[id >> 2]}</span><span class="s s-${s}">${GLYPH[s]}</span></span>`;
}
function cmpArr(a, b) {
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}
function lexMin(cards) {
  let best = null;
  let bestPerm = null;
  let stab = 0;
  for (const p of PERMS) {
    const m = cards.map((c) => (c & ~3) | p[c & 3]);
    m.sort((a, b) => a - b);
    if (!best) {
      best = m;
      bestPerm = p;
      stab = 1;
      continue;
    }
    const c = cmpArr(m, best);
    if (c < 0) {
      best = m;
      bestPerm = p;
      stab = 1;
    } else if (c === 0) stab++;
  }
  return { asc: best, perm: bestPerm, orbit: 24 / stab };
}
function parseText(text) {
  let t = "";
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (ch === " " || ch === "," || ch === "-" || ch === "[" || ch === "]") continue;
    if (ch === "1" && text[i + 1] === "0") {
      t += "T";
      i++;
      continue;
    }
    t += ch;
  }
  if (t.length % 2) throw new Error("Card text is incomplete.");
  const out = [];
  for (let i = 0; i < t.length; i += 2) {
    const r = RANKS.indexOf(t[i].toUpperCase());
    const s = SUITS.indexOf(t[i + 1].toLowerCase());
    if (r < 0 || s < 0) throw new Error("Bad card " + t.slice(i, i + 2));
    out.push(r * 4 + s);
  }
  return out;
}
function fmtCards(ids) {
  return ids.map(cardStr).join("");
}
function pct(x) {
  return (100 * x).toFixed(1) + "%";
}
function bb(x) {
  const n = Number(x);
  if (!Number.isFinite(n)) return "—";
  return (n >= 0 ? "+" : "") + n.toFixed(3);
}
function cls(x) {
  return x >= 0 ? "pos" : "neg";
}

function counts(ids) {
  const c = Array(13).fill(0);
  for (const id of ids) c[id >> 2]++;
  return c;
}
function sig(nums) {
  return nums.filter((x) => x > 0).sort((a, b) => b - a).join("-");
}
function heShape(a, b) {
  if ((a >> 2) === (b >> 2)) return "pocket pair";
  return (a & 3) === (b & 3) ? "suited" : "offsuit";
}
function ploSuit(ids) {
  const c = [0, 0, 0, 0];
  for (const id of ids) c[id & 3]++;
  const s = sig(c);
  if (s === "2-2") return "double-suited";
  if (s === "2-1-1") return "single-suited";
  if (s === "1-1-1-1") return "rainbow";
  if (s === "3-1") return "three-flush";
  return "monotone";
}
function splitOf(hand, q) {
  const he = [hand[PAIR_A[q]], hand[PAIR_B[q]]];
  const plo = [];
  for (let i = 0; i < 6; i++) if (i !== PAIR_A[q] && i !== PAIR_B[q]) plo.push(hand[i]);
  return { he, plo };
}

function readKey(i) {
  const o = HDR + i * REC;
  return [0, 1, 2, 3, 4, 5].map((b) => u8[o + b]);
}
function findKey(key) {
  let lo = 0, hi = ncanon;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    const c = cmpArr(readKey(mid), key);
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  if (lo < ncanon && cmpArr(readKey(lo), key) === 0) return lo;
  return -1;
}
function readRec(i) {
  const o = HDR + i * REC;
  const cards = readKey(i);
  const orbit = u8[o + 6];
  const dcount = u8[o + 7];
  const ev = [], scoop = [], lose = [], eq = [];
  for (let s = 0; s < 15; s++) {
    ev.push(dv.getInt16(o + 8 + 2 * s, true) / 1000);
    scoop.push(dv.getUint16(o + 38 + 2 * s, true) / 10000);
    lose.push(dv.getUint16(o + 68 + 2 * s, true) / 10000);
    eq.push(u8[o + 98 + s] / 255);
  }
  const es = eq.reduce((a, b) => a + b, 0) || 1;
  for (let s = 0; s < 15; s++) eq[s] /= es;
  return {
    cards, orbit, deals: dcount, ev, scoop, lose, eq,
    nash: dv.getInt16(o + 113, true) / 1000,
    se: dv.getInt16(o + 115, true) / 1000,
    value: dv.getInt16(o + 117, true) / 1000,
  };
}

function setStatus(text) {
  $("status").textContent = text;
}
function statusLine() {
  if (!buf) return "Dataset not loaded.";
  const pctDone = ncanon ? (100 * completed) / ncanon : 0;
  const tail = completed >= ncanon ? "complete" : pctDone.toFixed(1) + "% solved";
  return `${completed.toLocaleString()} / ${ncanon.toLocaleString()} hands · ${deals} deals × ${boards} boards · ${tail}`;
}
function mb(n) {
  return (n / 1048576).toFixed(n >= 10 * 1048576 ? 0 : 1) + " MB";
}
function setProgress(frac, text) {
  const bar = $("progress");
  bar.hidden = false;
  $("progress-bar").style.width = Math.max(3, Math.min(100, frac * 100)).toFixed(1) + "%";
  if (text) setStatus(text);
}
function hideProgress() {
  $("progress").hidden = true;
}
function rememberHand(text) {
  const u = new URL(location.href);
  if (text) u.searchParams.set("hand", text);
  else u.searchParams.delete("hand");
  history.replaceState(null, "", u);
}
function filterValue(id) {
  const active = $(id).querySelectorAll('button[aria-pressed="true"]');
  return [...active].map((button) => button.dataset.value).filter(Boolean).join(",");
}
function selectFilters(group, values) {
  const buttons = [...group.querySelectorAll("button[data-value]")];
  const matching = buttons.filter((button) => values.includes(button.dataset.value));
  const selected = matching.length ? matching : [buttons[0]];
  for (const button of buttons) button.setAttribute("aria-pressed", selected.includes(button));
}
function writeFilters() {
  const u = new URL(location.href);
  for (const [id, key] of Object.entries(FILTER_KEYS)) {
    const value = filterValue(id);
    if (!value || (id === "f-sort" && value === "strong")) u.searchParams.delete(key);
    else u.searchParams.set(key, value);
  }
  history.replaceState(null, "", u);
}
function readFilters() {
  for (const [id, key] of Object.entries(FILTER_KEYS)) {
    const value = params.get(key);
    if (value !== null) selectFilters($(id), id === "f-has" ? value.split(",") : [value]);
  }
}
function hasFilterQuery() {
  return Object.values(FILTER_KEYS).some((key) => params.get(key));
}

async function fetchBytes(url, label) {
  const res = await fetch(url);
  if (res.status === 404) return null;
  if (!res.ok) throw new Error("Could not read " + url + " (" + res.status + ")");
  const total = Number(res.headers.get("content-length")) || 0;
  if (!res.body || !res.body.getReader) {
    const bytes = new Uint8Array(await res.arrayBuffer());
    setProgress(1, label + " " + mb(bytes.length));
    return bytes;
  }
  const reader = res.body.getReader();
  const parts = [];
  let got = 0;
  while (true) {
    const step = await reader.read();
    if (step.done) break;
    parts.push(step.value);
    got += step.value.byteLength;
    setProgress(total ? got / total : 0, total ? `${label} ${mb(got)} / ${mb(total)}` : `${label} ${mb(got)}`);
  }
  const out = new Uint8Array(got);
  let at = 0;
  for (const part of parts) {
    out.set(part, at);
    at += part.byteLength;
  }
  return out;
}
async function maybeGunzip(bytes) {
  if (bytes.length < 2 || bytes[0] !== 0x1f || bytes[1] !== 0x8b) return bytes;
  if (typeof DecompressionStream === "undefined") {
    throw new Error("This browser cannot decompress the split book.");
  }
  const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("gzip"));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}
function concatBytes(parts) {
  let n = 0;
  for (const part of parts) n += part.length;
  const out = new Uint8Array(n);
  let at = 0;
  for (const part of parts) {
    out.set(part, at);
    at += part.length;
  }
  return out;
}
function installBytes(bytes) {
  u8 = bytes;
  buf = bytes.buffer;
  dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const magic = String.fromCharCode(...u8.subarray(0, 8));
  if (magic !== "SCOOP6H1") throw new Error("This file is not a scoop split book.");
  const rec = dv.getUint16(24, true);
  if (rec !== REC) throw new Error("Record size " + rec + " does not match this page.");
  ncanon = dv.getUint32(8, true);
  completed = dv.getUint32(12, true);
  deals = dv.getUint16(16, true);
  boards = dv.getUint16(18, true);
  hideProgress();
  setStatus(statusLine());
}

async function loadSummary() {
  try {
    const res = await fetch(dataRoot + "/summary.json" + (live ? "?t=" + Date.now() : ""));
    if (!res.ok) return;
    summary = JSON.parse(await res.text());
    if ($("tend").hidden === false) renderTend();
  } catch {
    /* The split table can still load without the summary. */
  }
}

async function loadManifest(q) {
  const res = await fetch(dataRoot + "/manifest.json" + q);
  if (!res.ok) return false;
  const spec = await res.json();
  if (!spec.chunks || !spec.chunks.length) return false;
  const raws = [];
  for (let i = 0; i < spec.chunks.length; i++) {
    const bytes = await fetchBytes(dataRoot + "/" + spec.chunks[i] + q, `Part ${i + 1} of ${spec.chunks.length}`);
    if (!bytes) throw new Error("missing piece " + spec.chunks[i]);
    raws.push(await maybeGunzip(bytes));
  }
  installBytes(concatBytes(raws));
  return true;
}

async function loadBook() {
  const q = live && params.get("pack") !== "1" ? "?t=" + Date.now() : "";
  const packed = params.get("pack") === "1" || !live;
  if (packed && params.get("pack") !== "0") {
    if (await loadManifest(q)) return;
    const gz = await fetchBytes(dataRoot + "/hands.bin.gz" + q, "Loading the split book…");
    if (gz) {
      installBytes(await maybeGunzip(gz));
      return;
    }
  }
  const bin = await fetchBytes(dataRoot + "/hands.bin" + q, "Loading the split book…");
  if (!bin) throw new Error("missing");
  installBytes(bin);
}

function buildGrid() {
  const grid = $("grid");
  grid.innerHTML = "";
  for (let s = 0; s < 4; s++) {
    for (let r = 12; r >= 0; r--) {
      const id = r * 4 + s;
      const b = document.createElement("button");
      b.type = "button";
      b.dataset.id = String(id);
      b.setAttribute("aria-label", RANKS[r] + " of " + ["clubs", "diamonds", "hearts", "spades"][s]);
      b.innerHTML = `<span class="r">${RANKS[r]}</span><span class="s s-${SUITS[s]}">${GLYPH[SUITS[s]]}</span>`;
      b.addEventListener("click", () => toggle(id));
      grid.appendChild(b);
    }
  }
}
function selectedIds() {
  try {
    return parseText($("text").value);
  } catch {
    return [];
  }
}
function syncGrid() {
  const ids = new Set(selectedIds());
  const full = ids.size >= 6;
  $("entry-note").textContent = ids.size ? ids.size + " of 6 cards" : "";
  for (const b of $("grid").querySelectorAll("button")) {
    const id = Number(b.dataset.id);
    const on = ids.has(id);
    b.classList.toggle("on", on);
    b.setAttribute("aria-pressed", on);
    b.disabled = full && !on;
  }
}
function toggle(id) {
  const ids = selectedIds();
  const i = ids.indexOf(id);
  if (i >= 0) ids.splice(i, 1);
  else if (ids.length < 6) ids.push(id);
  $("text").value = fmtCards(ids);
  syncGrid();
}

function showHand(text) {
  $("text").value = text;
  syncGrid();
  $("tab-lookup").click();
  $("entry").requestSubmit();
}

function showLookupMessage(message) {
  const text = document.createElement("p");
  text.className = "empty";
  text.textContent = message;
  $("result").replaceChildren(text);
}
function isSmallLead(gap, se) {
  return gap < Math.max(20, 2 * se);
}
function renderResult(input) {
  if (input.length !== 6) {
    showLookupMessage(`Enter exactly six cards. You have ${input.length}.`);
    return;
  }
  if (new Set(input).size !== 6) {
    showLookupMessage("Choose six distinct cards.");
    return;
  }
  rememberHand(fmtCards(input));
  if (!buf) {
    showLookupMessage("The split book is unavailable right now.");
    return;
  }
  const { asc, perm } = lexMin(input);
  const desc = asc.slice().reverse();
  const inv = [0, 0, 0, 0];
  for (let s = 0; s < 4; s++) inv[perm[s]] = s;
  const shown = desc.map((c) => (c & ~3) | inv[c & 3]);
  const idx = findKey(desc);
  if (idx < 0 || !u8[HDR + idx * REC + 7]) {
    showLookupMessage("Splits for " + fmtCards(input) + " are not available yet.");
    return;
  }
  const rec = readRec(idx);
  const order = [...Array(15).keys()].sort((a, b) => rec.ev[b] - rec.ev[a]);
  const best = order[0];
  const second = order[1];
  const gap = rec.ev[best] - rec.ev[second];
  const bestSplit = splitOf(shown, best);
  const close = isSmallLead(Math.round(gap * 1000), Math.round(rec.se * 1000));
  const maxAbs = Math.max(0.02, ...rec.ev.map((x) => Math.abs(x)));
  const rows = order.map((s, rank) => {
    const sp = splitOf(shown, s);
    const chop = Math.max(0, 1 - rec.scoop[s] - rec.lose[s]);
    const w = (Math.abs(rec.ev[s]) / maxAbs) * 50;
    const side = rec.ev[s] >= 0 ? "pos" : "neg";
    return `<tr class="${s === best ? "best" : ""}">
      <td>${rank + 1}</td>
      <td>${sp.he.map(cardHtml).join("")}</td>
      <td>${sp.plo.map(cardHtml).join("")}</td>
      <td>${heShape(sp.he[0], sp.he[1])} / ${ploSuit(sp.plo)}</td>
      <td class="num ${cls(rec.ev[s])}">${bb(rec.ev[s])}</td>
      <td class="num">${pct(rec.eq[s])}</td>
      <td class="num">${pct(rec.scoop[s])}</td>
      <td class="num">${pct(chop)}</td>
      <td class="num">${pct(rec.lose[s])}</td>
      <td><div class="bar"><i class="${side}" style="width:${w.toFixed(1)}%"></i></div></td>
    </tr>`;
  }).join("");
  $("result").innerHTML = `
    <div class="kicker-row">
      <p class="kicker">${rec.deals} opponent deals × ${boards} boards</p>
      <button type="button" id="copy-link" class="ghost">Copy link</button>
    </div>
    <h2>Highest estimated EV split</h2>
    <div class="rec">
      <div class="split-groups">
         <div><span class="pipe">Hold’em</span><div class="cards">${bestSplit.he.map(cardHtml).join("")}</div></div>
         <div><span class="pipe">PLO</span><div class="cards">${bestSplit.plo.map(cardHtml).join("")}</div></div>
       </div>
      <div class="evbig"><b class="${cls(rec.ev[best])}">${bb(rec.ev[best])}</b><span>EV in BB · SE ${rec.se.toFixed(3)}</span></div>
    </div>
    <p class="fine">${close ? "Small lead over " + bestSplitLabel(shown, second) : "Ahead of " + bestSplitLabel(shown, second)} by ${bb(gap)} BB. Mean NashConv ${rec.nash.toFixed(3)} BB.</p>
    <div class="table-wrap"><table>
      <thead><tr>
        <th></th><th>Hold’em</th><th>PLO</th><th>Shape</th>
        <th class="num">EV (BB)</th><th class="num">Eq. use</th><th class="num">Scoop</th><th class="num">Chop</th><th class="num">Lose</th><th></th>
      </tr></thead>
      <tbody>${rows}</tbody>
    </table></div>`;
}
function bestSplitLabel(hand, s) {
  const sp = splitOf(hand, s);
  return sp.he.map(cardStr).join("");
}

function tendencyTable(title, note, heading, columns, rows) {
  const labels = columns.map(([label]) => `<th scope="col" class="num">${label}</th>`).join("");
  const body = rows.map((row) => {
    const cells = columns.map(([, key]) => `<td class="num">${pct(row[key] ?? 0)}</td>`).join("");
    return `<tr><th scope="row">${row.label}</th>${cells}</tr>`;
  }).join("");
  return `<section class="panel"><h2>${title}</h2><p class="fine">${note}</p>
    <div class="table-wrap"><table>
      <thead><tr><th scope="col">${heading}</th>${labels}</tr></thead>
      <tbody>${body}</tbody>
    </table></div></section>`;
}

function renderTend() {
  const host = $("tend-body");
  if (!summary || !summary.completed) {
    host.innerHTML = '<p class="panel empty">Tendencies are unavailable.</p>';
    return;
  }
  const s = summary;
  const metrics = [
    ["Highest rank in Hold’em", pct(s.topInHoldem), "At least one card of that rank"],
    ["Pair-first heuristic gap", bb(s.heurLoss) + " BB", "EV below the best fixed split"],
    ["Random split gap", bb(s.randLoss) + " BB", "EV below the best fixed split"],
    ["Mean NashConv", Number(s.meanNashConv).toFixed(3) + " BB", "Total deviation gain across six seats on sampled boards"],
  ];
  const metricHtml = metrics.map(([label, value, note]) =>
    `<div class="metric"><span>${label}</span><b>${value}</b><span>${note}</span></div>`).join("");
  const paired = (s.pairing || []).filter((row) => row.pairHe != null);
  const examples = (title, hands = []) => `<section class="panel"><h2>${title}</h2>${hands.slice(0, 6).map((hand) => `
    <button type="button" class="handbtn" data-cards="${hand.cards}">
      <span>${hand.cards}<br><span class="fine">${hand.best}</span></span>
      <b class="${cls(hand.ev)}">${bb(hand.ev)} BB</b>
    </button>`).join("")}</section>`;
  host.innerHTML = `
    <section class="panel">
      <h2>Six-max split tendencies</h2>
      <p class="fine">The solver samples five random opponent hands for each starting hand. All players choose approximate equilibrium splits with every hole card known. Frequencies weight each suit pattern by the number of actual hands it represents.</p>
      ${s.finished ? "" : `<p class="fine">Partial catalogue · ${Number(s.completed).toLocaleString()} of ${Number(s.canonical).toLocaleString()} suit patterns solved.</p>`}
    </section>
    <div class="metrics">${metricHtml}</div>
    ${tendencyTable("Which Hold’em shapes get picked?", "Compare the equilibrium choice with picking one of the 15 splits uniformly. The pair-first heuristic takes the highest pocket pair available, or the two highest cards if there is no pair.", "Hold’em shape",
      [["Equilibrium", "share"], ["Random split", "available"]], s.heShape || [])}
    ${tendencyTable("Where does the highest repeated rank go?", "Within each hand type, ‘Pocket pair in Hold’em’ means two cards of that rank go to Hold’em. ‘Across both’ means one goes to Hold’em and the rest go to PLO.", "Six-card hand",
      [["Hand frequency", "share"], ["Pocket pair in Hold’em", "pairHe"], ["All in PLO", "pairPlo"], ["Across both", "pairSplit"]], paired)}
    ${tendencyTable("How do six-card suits affect the split?", "Each choice rate is conditional on the six-card suit pattern in that row. Suited Hold’em and double-suited PLO can occur in the same split.", "Six-card suits",
      [["Hand frequency", "share"], ["Suited Hold’em", "heSuited"], ["Double-suited PLO", "ds"]], s.suits || [])}
    ${tendencyTable("Which PLO shapes get picked?", "Each cell is a share of all equilibrium choices. The total column adds across pairing types.", "PLO suits",
      [["Unpaired", "unpaired"], ["One pair", "one pair"], ["Two pair", "two pair"], ["Trips+", "trips+"], ["Total", "total"]], (s.plo || []).map((row) => ({ ...row, label: row.suits })))}
    <div class="two">${examples("Highest estimated EV", s.strongest)}${examples("Lowest estimated EV", s.weakest)}</div>
    <p class="fine">The examples are ranked by estimated EV of the best fixed split. Close rankings can change with a fresh sample.</p>`;
  host.querySelectorAll(".handbtn").forEach((button) => button.addEventListener("click", () => showHand(button.dataset.cards)));
}

const hand6 = new Uint8Array(6);
const cnt = new Uint8Array(13);
const sc = new Uint8Array(4);
const ps = new Uint8Array(4);

function sig4(arr) {
  const v = [arr[0], arr[1], arr[2], arr[3]];
  v.sort((a, b) => b - a);
  let s = "";
  for (const x of v) if (x) s += (s ? "-" : "") + x;
  return s;
}
function pairName() {
  let pairs = 0, trips = 0, quads = 0;
  for (let r = 0; r < 13; r++) {
    const c = cnt[r];
    if (c === 2) pairs++;
    else if (c === 3) trips++;
    else if (c === 4) quads++;
  }
  if (quads && pairs) return "quads + pair";
  if (quads) return "quads";
  if (trips === 2) return "two trips";
  if (trips && pairs) return "trips + pair";
  if (trips) return "trips";
  if (pairs === 3) return "three pair";
  if (pairs === 2) return "two pair";
  if (pairs === 1) return "one pair";
  return "no pair";
}
function ploName() {
  const s = sig4(ps);
  if (s === "2-2") return "double-suited";
  if (s === "2-1-1") return "single-suited";
  if (s === "1-1-1-1") return "rainbow";
  if (s === "3-1") return "three-flush";
  return "monotone";
}
function bestOf(o) {
  let best = 0;
  let bestEv = dv.getInt16(o + 8, true);
  let second = -32768;
  for (let s = 1; s < 15; s++) {
    const ev = dv.getInt16(o + 8 + 2 * s, true);
    if (ev > bestEv) {
      second = bestEv;
      bestEv = ev;
      best = s;
    } else if (ev > second) second = ev;
  }
  return { best, bestEv, second };
}

let browseIdx = new Uint32Array(0);
let browseGap = new Int16Array(0);
let browseOrder = new Uint32Array(0);
let browseN = 0;
let browseShown = 40;
let browseDirty = true;
let browseTimer = 0;

function runBrowse() {
  browseDirty = false;
  const host = $("browse-list");
  const count = $("f-count");
  if (!buf) {
    count.textContent = "";
    host.innerHTML = `<p class="empty">The dataset is not loaded yet.</p>`;
    return;
  }
  const wantPair = filterValue("f-pair");
  const wantHigh = filterValue("f-high");
  const wantPairRank = filterValue("f-pairrank");
  const wantHas = filterValue("f-has");
  const wantSuits = filterValue("f-suits");
  const wantHe = filterValue("f-he");
  const wantPlo = filterValue("f-plo");
  const wantTop = filterValue("f-tophe");
  const wantPlace = filterValue("f-place");
  const wantCall = filterValue("f-call");
  const sort = filterValue("f-sort");
  const highN = wantHigh === "" ? -1 : Number(wantHigh);
  const pairN = wantPairRank === "" ? -1 : Number(wantPairRank);
  const hasRanks = wantHas ? wantHas.split(",").map(Number) : [];
  const needPlay = wantHe || wantPlo || wantTop || wantPlace;

  let cap = 8192;
  let idx = new Uint32Array(cap);
  let evA = new Int16Array(cap);
  let gapA = new Int16Array(cap);
  let n = 0;
  let weight = 0;
  for (let i = 0; i < ncanon; i++) {
    const o = HDR + i * REC;
    if (u8[o + 7] === 0) continue;
    cnt.fill(0);
    sc.fill(0);
    let hi = 0;
    let topPair = -1;
    for (let k = 0; k < 6; k++) {
      const c = u8[o + k];
      hand6[k] = c;
      const r = c >> 2;
      cnt[r]++;
      sc[c & 3]++;
      if (r > hi) hi = r;
    }
    if (highN >= 0 && hi !== highN) continue;
    if (hasRanks.some((r) => cnt[r] === 0)) continue;
    for (let r = 12; r >= 0; r--) if (cnt[r] >= 2) { topPair = r; break; }
    if (pairN >= 0 && topPair !== pairN) continue;
    if (wantPair && pairName() !== wantPair) continue;
    if (wantSuits && sig4(sc) !== wantSuits) continue;

    const pick = bestOf(o);
    const gap = pick.bestEv - pick.second;
    const se = dv.getInt16(o + 115, true);
    const close = isSmallLead(gap, se);
    if (wantCall === "clear" && close) continue;
    if (wantCall === "tie" && !close) continue;
    if (needPlay) {
      const q = pick.best;
      const a = PAIR_A[q];
      const b = PAIR_B[q];
      const he0 = hand6[a];
      const he1 = hand6[b];
      if (wantHe && heShape(he0, he1) !== wantHe) continue;
      if (wantTop === "yes" && (he0 >> 2) !== hi && (he1 >> 2) !== hi) continue;
      if (wantTop === "no" && ((he0 >> 2) === hi || (he1 >> 2) === hi)) continue;
      if (wantPlace) {
        if (topPair < 0) continue;
        const inHe = ((he0 >> 2) === topPair ? 1 : 0) + ((he1 >> 2) === topPair ? 1 : 0);
        const place = inHe === 2 ? "hold'em" : inHe === 0 ? "omaha" : "split";
        if (place !== wantPlace) continue;
      }
      if (wantPlo) {
        ps.fill(0);
        for (let k = 0; k < 6; k++) if (k !== a && k !== b) ps[hand6[k] & 3]++;
        if (ploName() !== wantPlo) continue;
      }
    }
    if (n === cap) {
      cap *= 2;
      const ni = new Uint32Array(cap); ni.set(idx); idx = ni;
      const ne = new Int16Array(cap); ne.set(evA); evA = ne;
      const ng = new Int16Array(cap); ng.set(gapA); gapA = ng;
    }
    idx[n] = i;
    evA[n] = pick.bestEv;
    gapA[n] = gap;
    weight += u8[o + 6];
    n++;
  }
  const order = new Uint32Array(n);
  for (let i = 0; i < n; i++) order[i] = i;
  order.sort((a, b) => {
    let d = 0;
    if (sort === "weak") d = evA[a] - evA[b];
    else if (sort === "clear") d = gapA[b] - gapA[a];
    else if (sort === "close") d = gapA[a] - gapA[b];
    else d = evA[b] - evA[a];
    return d || idx[a] - idx[b];
  });
  browseIdx = idx;
  browseGap = gapA;
  browseOrder = order;
  browseN = n;
  browseShown = 40;
  count.textContent = n
    ? `${n.toLocaleString()} solved patterns · ${weight.toLocaleString()} real hands`
    : "No solved hand matches these filters.";
  renderBrowseList();
}

function renderBrowseList() {
  const host = $("browse-list");
  if (!browseN) {
    host.innerHTML = `<p class="empty">No solved hand matches these filters.</p>`;
    return;
  }
  const end = Math.min(browseShown, browseN);
  let html = "";
  for (let row = 0; row < end; row++) {
    const k = browseOrder[row];
    const rec = readRec(browseIdx[k]);
    let best = 0;
    for (let s = 1; s < 15; s++) if (rec.ev[s] > rec.ev[best]) best = s;
    const sp = splitOf(rec.cards, best);
    const gap = browseGap[k] / 1000;
    const close = isSmallLead(browseGap[k], Math.round(rec.se * 1000));
    const lead = (close ? "Small lead " : "Lead ") + bb(gap) + " BB";
    html += `<button type="button" class="brow" data-i="${browseIdx[k]}">
      <span class="cards">${rec.cards.map(cardHtml).join("")}</span>
      <span class="play">${sp.he.map(cardHtml).join("")} in hold’em · ${sp.plo.map(cardHtml).join("")}<br>${heShape(sp.he[0], sp.he[1])} / ${ploSuit(sp.plo)} · ${lead}</span>
      <span class="ev"><b class="${cls(rec.ev[best])}">${bb(rec.ev[best])}</b><span class="fine">± ${rec.se.toFixed(3)}</span></span>
    </button>`;
  }
  if (end < browseN) {
    html += `<button type="button" id="browse-more" class="ghost more">Show more · ${end.toLocaleString()} of ${browseN.toLocaleString()}</button>`;
  }
  host.innerHTML = html;
}

function scheduleBrowse() {
  browseDirty = true;
  clearTimeout(browseTimer);
  if ($("browse").hidden) return;
  $("f-count").textContent = "Filtering…";
  browseTimer = setTimeout(runBrowse, 60);
}

function showTab(which) {
  $("lookup").hidden = which !== "lookup";
  $("browse").hidden = which !== "browse";
  $("tend").hidden = which !== "tend";
  $("tab-lookup").classList.toggle("on", which === "lookup");
  $("tab-browse").classList.toggle("on", which === "browse");
  $("tab-tend").classList.toggle("on", which === "tend");
  if (which === "tend") renderTend();
  if (which === "browse" && browseDirty) runBrowse();
}

buildGrid();
syncGrid();
$("tab-lookup").addEventListener("click", () => showTab("lookup"));
$("tab-browse").addEventListener("click", () => showTab("browse"));
$("tab-tend").addEventListener("click", () => showTab("tend"));
$("filters").addEventListener("click", (event) => {
  const button = event.target.closest("button[data-value]");
  if (!button) return;
  const group = button.closest("fieldset");
  const value = button.dataset.value;
  if (group.hasAttribute("data-multiple") && value) {
    const ranks = new Set(filterValue(group.id).split(",").filter(Boolean));
    if (ranks.has(value)) ranks.delete(value);
    else ranks.add(value);
    selectFilters(group, [...ranks]);
  } else {
    selectFilters(group, [value]);
  }
  writeFilters();
  scheduleBrowse();
});
$("f-reset").addEventListener("click", () => {
  for (const id of Object.keys(FILTER_KEYS)) selectFilters($(id), []);
  writeFilters();
  scheduleBrowse();
});
$("browse-list").addEventListener("click", (e) => {
  if (e.target.closest("#browse-more")) {
    browseShown += 40;
    renderBrowseList();
    return;
  }
  const row = e.target.closest("[data-i]");
  if (row) showHand(fmtCards(readKey(Number(row.dataset.i))));
});
$("text").addEventListener("input", syncGrid);
$("entry").addEventListener("submit", (e) => {
  e.preventDefault();
  try {
    renderResult(parseText($("text").value));
  } catch (err) {
    showLookupMessage(err.message);
  }
});
$("clear").addEventListener("click", () => {
  $("text").value = "";
  syncGrid();
  rememberHand("");
  showLookupMessage(LOOKUP_PROMPT);
  $("text").focus();
});
$("result").addEventListener("click", async (e) => {
  const btn = e.target.closest("#copy-link");
  if (!btn) return;
  try {
    await navigator.clipboard.writeText(location.href);
    btn.textContent = "Copied";
  } catch {
    btn.textContent = "Copy the address bar";
  }
});
async function boot() {
  readFilters();
  const hand = params.get("hand");
  if (hand) {
    $("text").value = hand;
    syncGrid();
  } else if (hasFilterQuery()) {
    showTab("browse");
  }
  try {
    await loadBook();
    browseDirty = true;
    if (!$("browse").hidden) runBrowse();
    if ($("text").value.trim()) $("entry").requestSubmit();
  } catch (err) {
    hideProgress();
    setStatus("Split book unavailable");
    showLookupMessage(err.message === "missing" ? "The split book has not been published here yet." : err.message);
  }
  await loadSummary();
}
boot();
