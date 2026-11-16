const RANKS = "23456789TJQKA";
const SUITS = "cdhs";
const GLYPH = { c: "♣", d: "♦", h: "♥", s: "♠" };
const SUIT_NAMES = { c: "Clubs", d: "Diamonds", h: "Hearts", s: "Spades" };
const HAND_SIZE = 6;
const PAIR_A = [0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3, 4];
const PAIR_B = [1, 2, 3, 4, 5, 2, 3, 4, 5, 3, 4, 5, 4, 5, 5];
const HDR = 64;
const REC = 119;
const BROWSE_REC = 14;

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
const dataRoot = params.get("data") || (live && params.get("pack") !== "1" ? "../data/sixmax" : "../data/pages");
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
let indexedBook = null;
let bookReady;
let lookupRevision = 0;
let bookWeight = 0;
const LOOKUP_PROMPT = "Choose or type six cards, then select Show splits.";

const $ = (id) => document.getElementById(id);

function cardStr(id) {
  return RANKS[id >> 2] + SUITS[id & 3];
}
function cardFace(id) {
  const suit = SUITS[id & 3];
  return `<span class="r">${RANKS[id >> 2]}</span><span class="s s-${suit}">${GLYPH[suit]}</span>`;
}
function cardHtml(id) {
  return `<span class="card">${cardFace(id)}</span>`;
}
function cardLabel(id) {
  const rank = RANKS[id >> 2];
  const name = { A: "Ace", K: "King", Q: "Queen", J: "Jack", T: "Ten" }[rank] || rank;
  return name + " of " + SUIT_NAMES[SUITS[id & 3]].toLowerCase();
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
    if (/[\s,\[\]-]/.test(ch)) continue;
    if (ch === "1" && text[i + 1] === "0") {
      t += "T";
      i++;
      continue;
    }
    t += ch;
  }
  const out = [];
  for (let i = 0; i < t.length; i += 2) {
    const r = RANKS.indexOf(t[i].toUpperCase());
    if (i + 1 === t.length && r >= 0) throw new Error("Add a suit to the last rank, such as 4h.");
    const suit = t.slice(i + 1, i + 2).toLowerCase();
    const s = SUITS.indexOf(Object.keys(GLYPH).find((key) => GLYPH[key] === suit) || suit);
    if (r < 0 || s < 0) throw new Error(`“${t.slice(i, i + 2)}” is invalid. Use a rank and suit, such as 4h.`);
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

function readKey(i, bytes = u8, start = HDR, stride = REC) {
  const o = start + i * stride;
  return Array.from(bytes.subarray(o, o + 6));
}
function findKey(key, bytes = u8, start = HDR, size = ncanon) {
  let lo = 0, hi = size;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    const c = cmpArr(readKey(mid, bytes, start), key);
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  if (lo < size && cmpArr(readKey(lo, bytes, start), key) === 0) return lo;
  return -1;
}
function readRec(bytes, view, o) {
  const orbit = bytes[o + 6];
  const dcount = bytes[o + 7];
  const ev = [], scoop = [], lose = [], eq = [];
  for (let s = 0; s < 15; s++) {
    ev.push(view.getInt16(o + 8 + 2 * s, true) / 1000);
    scoop.push(view.getUint16(o + 38 + 2 * s, true) / 10000);
    lose.push(view.getUint16(o + 68 + 2 * s, true) / 10000);
    eq.push(bytes[o + 98 + s] / 255);
  }
  const es = eq.reduce((a, b) => a + b, 0) || 1;
  for (let s = 0; s < 15; s++) eq[s] /= es;
  return {
    orbit, deals: dcount, ev, scoop, lose, eq,
    nash: view.getInt16(o + 113, true) / 1000,
    se: view.getInt16(o + 115, true) / 1000,
    value: view.getInt16(o + 117, true) / 1000,
  };
}

function setStatus(text) {
  $("status").textContent = text;
}
function statusLine() {
  if (!buf && !indexedBook) return "Dataset not loaded.";
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
  bookWeight = 0;
  for (let i = 0; i < ncanon; i++) {
    const offset = HDR + i * REC;
    if (u8[offset + 7]) bookWeight += u8[offset + 6];
  }
  $("random-hand").disabled = bookWeight === 0;
  hideProgress();
  setStatus(statusLine());
}

const packedCacheName = "scoop-catalogue-assets-v1";
const shardCache = new Map();
const shardCacheLimit = 16;

async function fetchAsset(file) {
  const url = new URL(dataRoot + "/" + file, location.href).href;
  const cache = await globalThis.caches?.open(packedCacheName).catch(() => null);
  if (cache) {
    const hit = await cache.match(url).catch(() => null);
    if (hit) return hit;
  }
  const response = await fetch(url);
  if (!response.ok) throw new Error("Could not download " + file + " (" + response.status + ").");
  if (cache) cache.put(url, response.clone()).catch(() => {});
  return response;
}

async function packedBytes(file) {
  return maybeGunzip(new Uint8Array(await (await fetchAsset(file)).arrayBuffer()));
}

function readShard(shard) {
  let request = shardCache.get(shard.file);
  shardCache.delete(shard.file);
  if (!request) {
    request = packedBytes(shard.file).then(bytes => {
      if (bytes.length !== shard.count * REC) throw new Error("The hand chunk has an unexpected size.");
      return bytes;
    }).catch(error => {
      if (shardCache.get(shard.file) === request) shardCache.delete(shard.file);
      throw error;
    });
  }
  shardCache.set(shard.file, request);
  if (shardCache.size > shardCacheLimit) shardCache.delete(shardCache.keys().next().value);
  return request;
}

function shardFor(key) {
  const target = key.reduce((value, card) => value * 64 + card, 0);
  const shards = indexedBook.shards;
  let lo = 0, hi = shards.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (shards[mid].last < target) lo = mid + 1;
    else hi = mid;
  }
  return shards[lo];
}

async function lookupRecord(key) {
  if (!indexedBook) {
    const i = findKey(key);
    return i >= 0 && u8[HDR + i * REC + 7] ? readRec(u8, dv, HDR + i * REC) : null;
  }
  const shard = shardFor(key);
  if (!shard) return null;
  const bytes = await readShard(shard);
  const i = findKey(key, bytes, 0, shard.count);
  return i >= 0 ? readRec(bytes, new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength), i * REC) : null;
}

async function loadManifest(q) {
  const res = await fetch(dataRoot + "/manifest.json" + q, { cache: "no-cache" });
  if (res.status === 404) return false;
  if (!res.ok) throw new Error("Could not read the split book index (" + res.status + ").");
  const spec = await res.json();
  if (spec.format === "SCOOP-indexed-v1") {
    indexedBook = spec;
    ({ count: ncanon, completed, deals, boards, weight: bookWeight } = spec);
    $("random-hand").disabled = bookWeight === 0;
    setStatus(statusLine());
    return true;
  }
  if (spec.format !== "SCOOP6H1-gzip" || !spec.chunks?.length) {
    throw new Error("Unrecognised split book index.");
  }
  const raws = await Promise.all(spec.chunks.map(async file => {
    const response = await fetch(dataRoot + "/" + file + q);
    if (!response.ok) throw new Error("Could not download " + file + ".");
    return maybeGunzip(new Uint8Array(await response.arrayBuffer()));
  }));
  installBytes(concatBytes(raws));
  return true;
}

async function loadBook() {
  const packed = params.get("pack") === "1" || (!live && params.get("pack") !== "0");
  if (packed && await loadManifest("")) return;
  const q = live && !packed ? "?t=" + Date.now() : "";
  if (packed) {
    const gz = await fetchBytes(dataRoot + "/hands.bin.gz", "Loading the split book…");
    if (gz) {
      installBytes(await maybeGunzip(gz));
      return;
    }
  }
  const bin = await fetchBytes(dataRoot + "/hands.bin" + q, "Loading the split book…");
  if (!bin) throw new Error("The split book has not been published here yet.");
  installBytes(bin);
}

let activeSuit = "s";

function buildPicker() {
  for (const suit of ["s", "h", "d", "c"]) {
    const button = document.createElement("button");
    button.type = "button";
    button.dataset.suit = suit;
    button.innerHTML = `<span class="s-${suit}" aria-hidden="true">${GLYPH[suit]}</span><span>${SUIT_NAMES[suit]}</span>`;
    button.addEventListener("click", () => {
      activeSuit = suit;
      updateEntry();
    });
    $("suit-picker").appendChild(button);
  }
  for (let rank = RANKS.length - 1; rank >= 0; rank--) {
    const button = document.createElement("button");
    button.type = "button";
    button.dataset.rank = rank;
    button.addEventListener("click", () => toggleCard(Number(button.dataset.card)));
    $("grid").appendChild(button);
  }
  for (let i = 0; i < HAND_SIZE; i++) {
    const slot = document.createElement("div");
    slot.className = "selection-slot";
    const button = document.createElement("button");
    button.type = "button";
    button.addEventListener("click", () => {
      toggleCard(Number(button.dataset.card));
      if (button.hidden) {
        const filled = $("selection").querySelectorAll("button:not([hidden])");
        (filled[filled.length - 1] || $("text")).focus();
      }
    });
    slot.appendChild(button);
    $("selection").appendChild(slot);
  }
}

function readEntry() {
  let cards;
  try {
    cards = parseText($("text").value);
  } catch (err) {
    return { cards: [], unique: new Set(), error: err.message, malformed: true };
  }
  const unique = new Set();
  let duplicate;
  for (const card of cards) {
    if (unique.has(card) && duplicate === undefined) duplicate = card;
    unique.add(card);
  }
  let error = "";
  if (duplicate !== undefined) {
    error = `${cardLabel(duplicate)} appears more than once. Remove the extra copy.`;
  } else if (cards.length > HAND_SIZE) {
    error = `You entered ${cards.length} cards. Remove ${cards.length - HAND_SIZE} to make six.`;
  }
  return { cards, unique, error, malformed: false };
}

function updateEntry() {
  const { cards, unique, error, malformed } = readEntry();
  const full = unique.size >= HAND_SIZE;
  const note = $("entry-note");
  const message = error || `${unique.size} of ${HAND_SIZE} cards selected`;
  if (note.textContent !== message) note.textContent = message;
  $("text").setAttribute("aria-invalid", Boolean(error));
  $("show-splits").disabled = Boolean(error) || cards.length !== HAND_SIZE;

  for (const button of $("suit-picker").children) {
    button.setAttribute("aria-pressed", button.dataset.suit === activeSuit);
  }
  const suit = SUITS.indexOf(activeSuit);
  for (const button of $("grid").children) {
    const id = Number(button.dataset.rank) * SUITS.length + suit;
    if (button.dataset.card !== String(id)) {
      button.dataset.card = id;
      button.innerHTML = cardFace(id);
      button.setAttribute("aria-label", cardLabel(id));
    }
    const selected = unique.has(id);
    button.setAttribute("aria-pressed", selected);
    button.disabled = malformed || (full && !selected);
  }
  const selected = [...unique];
  for (const [i, button] of [...$("selection").querySelectorAll("button")].entries()) {
    const id = selected[i];
    button.hidden = id === undefined;
    if (id === undefined || button.dataset.card === String(id)) continue;
    button.dataset.card = id;
    button.innerHTML = cardFace(id);
    button.setAttribute("aria-label", "Remove " + cardLabel(id));
    button.title = "Remove " + cardLabel(id);
  }
}

function editHand(text) {
  lookupRevision++;
  $("text").value = text;
  updateEntry();
  showLookupMessage(LOOKUP_PROMPT);
}

function toggleCard(id) {
  const { cards, unique, malformed } = readEntry();
  if (malformed) return;
  const i = cards.indexOf(id);
  if (i >= 0) cards.splice(i, 1);
  else if (unique.size < HAND_SIZE) cards.push(id);
  else return;
  editHand(fmtCards(cards));
}

function showHand(text) {
  editHand(text);
  $("tab-lookup").click();
  $("entry").requestSubmit();
}

function weightedKey(ticket, count, bytes, start, stride, indices) {
  for (let k = 0; k < count; k++) {
    const i = indices ? indices[k] : k;
    const offset = start + i * stride;
    if (stride === REC && !bytes[offset + 7]) continue;
    ticket -= bytes[offset + 6];
    if (ticket < 0) return readKey(i, bytes, start, stride);
  }
  throw new Error("Could not find a solved hand for this selection.");
}

async function showRandomHand(matches = false) {
  const revision = ++lookupRevision;
  let ticket = Math.floor(Math.random() * (matches ? browseWeight : bookWeight));
  try {
    let key;
    if (matches) {
      key = weightedKey(ticket, browseN, browseData?.bytes ?? u8, browseData ? 0 : HDR,
        browseData ? BROWSE_REC : REC, browseIdx);
    } else if (indexedBook) {
      showLookupMessage("Loading a random hand…");
      for (const shard of indexedBook.shards) {
        if (ticket < shard.weight) {
          key = weightedKey(ticket, shard.count, await readShard(shard), 0, REC);
          break;
        }
        ticket -= shard.weight;
      }
    } else {
      key = weightedKey(ticket, ncanon, u8, HDR, REC);
    }
    if (revision !== lookupRevision) return;
    const suits = PERMS[Math.floor(Math.random() * PERMS.length)];
    const cards = key.map(card => (card & ~3) | suits[card & 3]);
    cards.sort((a, b) => b - a);
    showHand(fmtCards(cards));
  } catch (error) {
    if (revision === lookupRevision) showLookupMessage(error.message);
  }
}

function showLookupMessage(message) {
  const text = document.createElement("p");
  text.className = "empty";
  text.textContent = message;
  $("result").replaceChildren(text);
  $("lookup").style.removeProperty("--lookup-width");
}
function fitSplitTable() {
  const lookup = $("lookup");
  if (lookup.hidden) return;
  lookup.style.removeProperty("--lookup-width");
  const wrap = $("result").querySelector(".table-wrap");
  if (!wrap) return;
  const overflow = wrap.scrollWidth - wrap.clientWidth;
  if (overflow > 0) {
    const width = Math.ceil(lookup.getBoundingClientRect().width + overflow);
    lookup.style.setProperty("--lookup-width", width + "px");
  }
}
let fitFrame = 0;
function scheduleFitSplitTable() {
  cancelAnimationFrame(fitFrame);
  fitFrame = requestAnimationFrame(fitSplitTable);
}
function isSmallLead(gap, se) {
  return gap < Math.max(20, 2 * se);
}
async function renderResult(input) {
  const revision = ++lookupRevision;
  rememberHand(fmtCards(input));
  showLookupMessage("Loading splits…");
  if (!await bookReady) {
    if (revision === lookupRevision) showLookupMessage("The split book is unavailable right now.");
    return;
  }
  if (revision !== lookupRevision) return;
  const { asc, perm } = lexMin(input);
  const desc = asc.reverse();
  const inv = [0, 0, 0, 0];
  for (let suit = 0; suit < 4; suit++) inv[perm[suit]] = suit;
  const shown = desc.map(c => (c & ~3) | inv[c & 3]);
  let rec;
  try {
    rec = await lookupRecord(desc);
  } catch (error) {
    if (revision === lookupRevision) showLookupMessage(error.message);
    return;
  }
  if (revision !== lookupRevision) return;
  if (!rec) {
    showLookupMessage("Splits for " + fmtCards(input) + " are not available yet.");
    return;
  }

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
  fitSplitTable();
}
function bestSplitLabel(hand, s) {
  const sp = splitOf(hand, s);
  return sp.he.map(cardStr).join("");
}



function insightTable(heading, columns, rows) {
  return `<div class="table-wrap"><table>
    <thead><tr><th scope="col">${heading}</th>${columns.map(label => `<th scope="col" class="num">${label}</th>`).join("")}</tr></thead>
    <tbody>${rows.map(([label, ...cells]) => `<tr><th scope="row">${label}</th>${cells.map(cell => `<td class="num">${cell}</td>`).join("")}</tr>`).join("")}</tbody>
  </table></div>`;
}

function tendencyTable(title, heading, columns, rows) {
  return `<h3>${title}</h3>${insightTable(heading, columns.map(([label]) => label),
    rows.map(row => [row.label, ...columns.map(([, key]) => pct(row[key] ?? 0))]))}`;
}

let splitInsights = null;

let insightsRequest = null;

async function openInsights() {
  const host = $("tend-body");
  if (splitInsights) {
    renderTend();
    return;
  }
  host.innerHTML = '<p class="panel empty">Loading Insights…</p>';
  if (!await bookReady || !completed) {
    host.textContent = "Insights are unavailable.";
    return;
  }
  insightsRequest ??= (async () => {
    if (indexedBook) {
      summary = await (await fetchAsset(indexedBook.insights)).json();
      splitInsights = summary.splitInsights;
      return;
    }
    const [core, metadata] = await Promise.all([
      import("./catalogue.mjs"),
      fetch(dataRoot + "/summary.json", { cache: "no-cache" })
        .then(res => res.ok ? res.json() : null).catch(() => null),
    ]);
    summary = metadata;
    splitInsights = core.summarizeSplits(u8, dv, ncanon);
  })().catch(error => {
    insightsRequest = null;
    throw error;
  });
  try {
    await insightsRequest;
    if (!$("tend").hidden) renderTend();
  } catch (error) {
    host.textContent = error.message;
  }
}

function renderTend() {
  const host = $("tend-body");
  const { pairRanks, suitGroups: groups, totalWeight } = splitInsights;
  const suitGroups = new Map(groups);
  const s = summary?.completed === completed ? summary : null;
  const cost = (loss, weight) => (loss / weight / 1000).toFixed(3);
  const roundedPct = (part, whole) => Math.round(100 * part / whole) + "%";
  const pairRows = pairRanks.flatMap((group, rank) => group.weight ? [[
    RANKS[rank].repeat(2), pct(group.weight / totalWeight), pct(group.holdemBest / group.weight),
    cost(group.loss[2], group.weight), cost(group.loss[0], group.weight), cost(group.loss[1], group.weight),
  ]] : []).reverse();
  const suitRows = [...suitGroups].sort((a, b) => b[1].weight - a[1].weight).map(([pattern, group]) => [
    pattern, pct(group.weight / totalWeight), pct(group.doubleAhead / group.weight),
    cost(group.doubleLoss, group.weight), cost(group.otherLoss, group.weight),
  ]);

  const aces = pairRanks[12];
  const fives = pairRanks[3];
  const fourTwo = suitGroups.get("4-2");
  const twoTwo = suitGroups.get("2-2-1-1");
  const examples = [
    { hand: "AcJd6d6c5c4c", note: "4-2 · Keep 66 in double-suited PLO", holdem: "5c4c", plo: "AcJd6d6c" },
    { hand: "AcJd6d6h5c4c", note: "3-2-1 · Move 66 to Hold’em", holdem: "6h6d", plo: "AcJd5c4c" },
  ];
  const showExamples = Boolean(indexedBook) || examples.every(({ hand }) => {
    const key = lexMin(parseText(hand)).asc.reverse();
    const i = findKey(key);
    return i >= 0 && u8[HDR + i * REC + 7] > 0;
  });
  host.innerHTML = `
    <section class="panel">
      <h2>Six-max split strategy</h2>
      <p class="fine">Compare splits you can commit to using your six cards. Costs are average EV lost versus the best estimated split in the catalogue.</p>
      ${completed === ncanon ? "" : `<p class="fine">Partial catalogue · ${completed.toLocaleString()} of ${ncanon.toLocaleString()} suit patterns solved.</p>`}
    </section>
    ${pairRows.length ? `<section class="panel">
      <h2>Where should the pair go?</h2>
      ${aces.weight && fives.weight ? `<p>Start by checking premium pairs in Hold’em. With exactly one pair, AA in Hold’em is best or tied ${roundedPct(aces.holdemBest, aces.weight)} of the time, versus ${roundedPct(fives.holdemBest, fives.weight)} for 55.</p>` : ""}
      <details>
        <summary>See each pair rank</summary>
        <p class="fine">Hands with exactly one pair. Each cost is in BB per hand within that row, using the best split that puts both cards in Hold’em, both in PLO, or one in each.</p>
        ${insightTable("Pair", ["Share of all hands", "Hold’em best or tied", "Hold’em cost (BB)", "PLO cost (BB)", "Split cost (BB)"], pairRows)}
      </details>
    </section>` : ""}
    ${suitRows.length ? `<section class="panel">
      <h2>When is double-suited PLO worth it?</h2>
      ${fourTwo && twoTwo ? `<p>With 4-2 suits, double-suited PLO beats the best alternative in ${roundedPct(fourTwo.doubleAhead, fourTwo.weight)} of hands. With 2-2-1-1, it wins in ${roundedPct(twoTwo.doubleAhead, twoTwo.weight)}. In 4-2 you can choose two cards from the four-card suit for Hold’em. In 2-2-1-1 you have to take the two singletons.</p>` : ""}
      <details>
        <summary>See each suit pattern</summary>
        <p class="fine">Includes hands where both choices are possible. “Double-suited ahead” counts strict EV wins. Each cost is in BB per hand within that row.</p>
        ${insightTable("Six-card suits", ["Share of all hands", "Double-suited ahead", "Make it (BB cost)", "Skip it (BB cost)"], suitRows)}
      </details>
    </section>` : ""}
    ${showExamples ? `<section class="panel">
      <h2>One suit change flips the split</h2>
      <p>Change 6♣ to 6♥. In the first hand, 5♣4♣ in Hold’em preserves a pair and two suits in PLO. In the second, 66 in Hold’em has the higher EV.</p>
      <div class="two insight-examples">${examples.map(({ hand, note, holdem, plo }) => `
        <button type="button" class="handbtn" data-cards="${hand}">
          <span><span class="insight-cards">${parseText(hand).map(cardHtml).join("")}</span><span class="fine">${note}</span></span>
          <span class="insight-split">Hold’em ${parseText(holdem).map(id => RANKS[id >> 2] + GLYPH[SUITS[id & 3]]).join(" ")}<br>PLO ${parseText(plo).map(id => RANKS[id >> 2] + GLYPH[SUITS[id & 3]]).join(" ")}</span>
        </button>`).join("")}</div>
      <p class="fine">Confirmed in a separate solve with 2,400 opponent deals and 1,000 boards per deal. Select either hand to open its catalogue splits.</p>
    </section>` : ""}
    ${s?.completed ? `<section class="panel">
      <h2>Pair-first benchmark</h2>
      <p>Taking the highest pocket pair, or the two highest cards when unpaired, costs <strong>${Number(s.heurLoss).toFixed(3)} BB per hand</strong> on average versus the catalogue’s best fixed split.</p>
    </section>
    <details class="panel insight-reference">
      <summary>Split frequencies</summary>
      <p class="fine">These frequencies are for equilibrium players who can see everyone’s cards. The highest rank appears in Hold’em ${pct(s.topInHoldem)} of the time.</p>
      ${tendencyTable("Hold’em shapes", "Hold’em shape", [["Equilibrium", "share"], ["Random split", "available"]], s.heShape || [])}
      ${tendencyTable("Highest repeated rank", "Six-card hand", [["Hand frequency", "share"], ["Pocket pair in Hold’em", "pairHe"], ["All in PLO", "pairPlo"], ["Across both", "pairSplit"]], (s.pairing || []).filter(row => row.pairHe != null))}
      ${tendencyTable("PLO shapes", "PLO suits", [["Unpaired", "unpaired"], ["One pair", "one pair"], ["Two pair", "two pair"], ["Trips+", "trips+"], ["Total", "total"]], (s.plo || []).map(row => ({ ...row, label: row.suits })))}
    </details>` : ""}
    <details class="panel insight-reference">
      <summary>Methodology</summary>
      <p class="fine">The catalogue samples ${deals} opponent deals and ${boards} boards per deal for each starting hand. It estimates each fixed split’s EV against opponents who play an approximate full-information equilibrium. Each suit pattern is weighted by the number of actual hands it represents.</p>
      <p class="fine">Close EVs can switch order on a fresh sample. Selecting the highest estimated EV from the same sample can overstate the size of the gap.</p>
      ${s?.completed ? `<p class="fine">A uniform random split costs ${Number(s.randLoss).toFixed(3)} BB per hand versus the best estimated fixed split. Mean NashConv is ${Number(s.meanNashConv).toFixed(3)} BB across all six seats on the sampled boards.</p>` : ""}
    </details>`;
  host.querySelectorAll("button[data-cards]").forEach(button => button.addEventListener("click", () => showHand(button.dataset.cards)));
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
let browseGap = new Uint16Array(0);
let browseOrder = new Uint32Array(0);
let browseN = 0;
let browseWeight = 0;
let browseShown = 40;
let browseDirty = true;
let browseTimer = 0;

let browseData = null;
let browseRequest = null;

async function openBrowse() {
  if (!browseDirty) return;
  const count = $("f-count");
  if (!await bookReady) {
    count.textContent = "The split book is unavailable.";
    return;
  }
  if (indexedBook && !browseData) {
    count.textContent = "Loading Browse index…";
    browseRequest ??= packedBytes(indexedBook.browse).then(bytes => {
      if (bytes.length !== completed * BROWSE_REC) throw new Error("The Browse index has an unexpected size.");
      browseData = { bytes, view: new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength) };
    }).catch(error => {
      browseRequest = null;
      throw error;
    });
    try {
      await browseRequest;
    } catch (error) {
      count.textContent = error.message;
      return;
    }
  }
  if (!$("browse").hidden && browseDirty) runBrowse();
}

function browseKey(i) {
  return browseData ? readKey(i, browseData.bytes, 0, BROWSE_REC) : readKey(i);
}

function browseRow(o) {
  if (browseData) {
    const { bytes, view } = browseData;
    return {
      best: bytes[o + 7], bestEv: view.getInt16(o + 8, true),
      gap: view.getUint16(o + 10, true), se: view.getInt16(o + 12, true),
    };
  }
  const { best, bestEv, second } = bestOf(o);
  return { best, bestEv, gap: bestEv - second, se: dv.getInt16(o + 115, true) };
}

function runBrowse() {
  browseDirty = false;
  const host = $("browse-list");
  const count = $("f-count");
  $("random-match").disabled = true;
  const bytes = browseData?.bytes ?? u8;
  const start = browseData ? 0 : HDR;
  const stride = browseData ? BROWSE_REC : REC;
  const size = browseData ? completed : ncanon;
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
  let gapA = new Uint16Array(cap);
  let n = 0;
  let weight = 0;
  for (let i = 0; i < size; i++) {
    const o = start + i * stride;
    if (!browseData && !bytes[o + 7]) continue;
    cnt.fill(0);
    sc.fill(0);
    let hi = 0;
    let topPair = -1;
    for (let k = 0; k < 6; k++) {
      const c = bytes[o + k];
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

    const pick = browseRow(o);
    const { gap, se } = pick;
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
      const ng = new Uint16Array(cap); ng.set(gapA); gapA = ng;
    }
    idx[n] = i;
    evA[n] = pick.bestEv;
    gapA[n] = gap;
    weight += bytes[o + 6];
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
  browseWeight = weight;
  $("random-match").disabled = browseWeight === 0;
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
    const i = browseIdx[k];
    const cards = browseKey(i);
    const o = browseData ? i * BROWSE_REC : HDR + i * REC;
    const { best, bestEv, se } = browseRow(o);
    const sp = splitOf(cards, best);
    const ev = bestEv / 1000;
    const gap = browseGap[k] / 1000;
    const close = isSmallLead(browseGap[k], se);
    const lead = (close ? "Small lead " : "Lead ") + bb(gap) + " BB";
    html += `<button type="button" class="brow" data-i="${browseIdx[k]}">
      <span class="cards">${cards.map(cardHtml).join("")}</span>
      <span class="play">${sp.he.map(cardHtml).join("")} in hold’em · ${sp.plo.map(cardHtml).join("")}<br>${heShape(sp.he[0], sp.he[1])} / ${ploSuit(sp.plo)} · ${lead}</span>
      <span class="ev"><b class="${cls(ev)}">${bb(ev)}</b><span class="fine">± ${(se / 1000).toFixed(3)}</span></span>
    </button>`;
  }
  if (end < browseN) {
    html += `<button type="button" id="browse-more" class="ghost more">Show more · ${end.toLocaleString()} of ${browseN.toLocaleString()}</button>`;
  }
  host.innerHTML = html;
}

function scheduleBrowse() {
  browseDirty = true;
  $("random-match").disabled = true;
  clearTimeout(browseTimer);
  if ($("browse").hidden) return;
  $("f-count").textContent = "Filtering…";
  browseTimer = setTimeout(openBrowse, 60);
}

function showTab(which) {
  $("lookup").hidden = which !== "lookup";
  $("browse").hidden = which !== "browse";
  $("tend").hidden = which !== "tend";
  $("tab-lookup").classList.toggle("on", which === "lookup");
  $("tab-browse").classList.toggle("on", which === "browse");
  $("tab-tend").classList.toggle("on", which === "tend");
  if (which === "tend") openInsights();
  if (which === "browse") openBrowse();
  if (which === "lookup") scheduleFitSplitTable();
}

buildPicker();
updateEntry();
window.addEventListener("resize", scheduleFitSplitTable);
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
$("random-hand").addEventListener("click", () => showRandomHand());
$("random-match").addEventListener("click", () => showRandomHand(true));
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
  if (row) showHand(fmtCards(browseKey(Number(row.dataset.i))));
});
$("text").addEventListener("input", () => {
  lookupRevision++;
  updateEntry();
  showLookupMessage(LOOKUP_PROMPT);
});
$("entry").addEventListener("submit", (e) => {
  e.preventDefault();
  const { cards, error } = readEntry();
  if (error || cards.length !== HAND_SIZE) return;
  renderResult(cards);
});
$("clear").addEventListener("click", () => {
  editHand("");
  rememberHand("");
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
function boot() {
  readFilters();
  bookReady = loadBook().then(() => true, err => {
    hideProgress();
    setStatus("Split book unavailable");
    showLookupMessage(err.message);
    return false;
  });

  const hand = params.get("hand");
  if (hand) {
    $("text").value = hand;
    updateEntry();
    $("entry").requestSubmit();
  } else if (hasFilterQuery()) {
    showTab("browse");
  }
  return bookReady;
}
boot();
