import { createHash } from "node:crypto";
import { mkdir, readFile, readdir, rename, unlink, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { gzipSync } from "node:zlib";
import { HDR, REC, BROWSE_REC, cardKey, summarizeSplits } from "../web/catalogue.mjs";

const root = fileURLToPath(new URL("../", import.meta.url));
const recordsPerShard = Math.floor(256 * 1024 / REC);

export async function packPages({ source = join(root, "data/sixmax"), out = join(root, "data/pages") } = {}) {
  const [bytes, rawSummary] = await Promise.all([
    readFile(join(source, "hands.bin")), readFile(join(source, "summary.json"), "utf8"),
  ]);
  const summary = JSON.parse(rawSummary);
  const count = bytes.readUInt32LE(8);
  const completed = bytes.readUInt32LE(12);
  const deals = bytes.readUInt16LE(16);
  const boards = bytes.readUInt16LE(18);
  if (bytes.toString("ascii", 0, 8) !== "SCOOP6H1" || bytes.readUInt16LE(24) !== REC ||
      bytes.length !== HDR + count * REC || completed !== count ||
      !summary.finished || summary.completed !== count || summary.canonical !== count ||
      summary.deals !== deals || summary.boards !== boards ||
      summary.seed !== bytes.readUInt32LE(20)) {
    throw new Error("Publish a finished catalogue and its matching summary.");
  }

  await mkdir(out, { recursive: true });
  const emitted = new Set();
  let totalBytes = 0;
  async function emit(prefix, ext, data) {
    const hash = createHash("sha256").update(data).digest("hex").slice(0, 20);
    const file = `${prefix}-${hash}.${ext}`;
    await writeFile(join(out, file), data);
    emitted.add(file);
    totalBytes += data.length;
    return file;
  }

  const browse = Buffer.alloc(count * BROWSE_REC);
  const shards = [];
  let totalWeight = 0;
  for (let start = 0; start < count; start += recordsPerShard) {
    const end = Math.min(start + recordsPerShard, count);
    let weight = 0;
    for (let i = start; i < end; i++) {
      const o = HDR + i * REC;
      const b = i * BROWSE_REC;
      if (!bytes[o + 7] || !bytes[o + 6]) throw new Error(`Unsolved record ${i} in finished catalogue.`);
      bytes.copy(browse, b, o, o + 7);
      weight += bytes[o + 6];
      let best = -Infinity;
      let second = -Infinity;
      let split = 0;
      for (let q = 0; q < 15; q++) {
        const ev = bytes.readInt16LE(o + 8 + 2 * q);
        if (ev > best) {
          second = best;
          best = ev;
          split = q;
        } else {
          second = Math.max(second, ev);
        }
      }
      browse[b + 7] = split;
      browse.writeInt16LE(best, b + 8);
      browse.writeUInt16LE(best - second, b + 10);
      browse.writeInt16LE(bytes.readInt16LE(o + 115), b + 12);
    }
    const raw = bytes.subarray(HDR + start * REC, HDR + end * REC);
    const file = await emit("hand", "bin.gz", gzipSync(raw));
    shards.push({ file, last: cardKey(raw.subarray(raw.length - REC, raw.length - REC + 6)), count: end - start, weight });
    totalWeight += weight;
  }
  if (totalWeight !== summary.realHands) throw new Error("Catalogue weight does not match the summary.");

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  summary.splitInsights = summarizeSplits(bytes, view, count);
  const manifest = {
    format: "SCOOP-indexed-v1", count, completed, deals, boards, weight: totalWeight, shards,
    browse: await emit("browse", "bin.gz", gzipSync(browse)),
    insights: await emit("insights", "json", Buffer.from(JSON.stringify(summary))),
  };
  const manifestPath = join(out, "manifest.json");
  await writeFile(manifestPath + ".tmp", JSON.stringify(manifest) + "\n");
  await rename(manifestPath + ".tmp", manifestPath);
  for (const file of await readdir(out)) {
    if (/^(?:c\d+\.bin\.gz|summary\.json|(?:hand|browse)-[a-f0-9]+\.bin\.gz|insights-[a-f0-9]+\.json)$/.test(file) &&
        !emitted.has(file)) await unlink(join(out, file));
  }

  return { shards: shards.length, totalBytes, manifestBytes: Buffer.byteLength(JSON.stringify(manifest)) + 1 };
}

if (typeof process !== "undefined" && process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const { shards, totalBytes, manifestBytes } = await packPages();
  console.log(`${shards} hand chunks, a Browse index, and Insights: ${totalBytes.toLocaleString()} bytes; manifest: ${manifestBytes.toLocaleString()} bytes`);
}
