export const PAIR_A = [0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3, 4];
export const PAIR_B = [1, 2, 3, 4, 5, 2, 3, 4, 5, 3, 4, 5, 4, 5, 5];
export const HDR = 64;
export const REC = 119;
export const BROWSE_REC = 14;

export function cardKey(cards) {
  return cards.reduce((key, card) => key * 64 + card, 0);
}

function suitPattern(counts) {
  return [...counts].filter(Boolean).sort((a, b) => b - a).join("-");
}

export function summarizeSplits(u8, dv, ncanon) {
  const pairRanks = Array.from({ length: 13 }, () => ({ weight: 0, holdemBest: 0, loss: [0, 0, 0] }));
  const suitGroups = new Map();
  const rankCounts = new Uint8Array(13);
  const suitCounts = new Uint8Array(4);
  // Pack suit counts in three bits each so removing the two Hold’em cards is a subtraction.
  const suitUnit = [1, 8, 64, 512];
  const doubleSuited = new Set(suitUnit.flatMap((first, i) => suitUnit.slice(i + 1).map(second => 2 * (first + second))));
  let totalWeight = 0;

  for (let i = 0; i < ncanon; i++) {
    const o = HDR + i * REC;
    if (!u8[o + 7]) continue;
    const weight = u8[o + 6];
    totalWeight += weight;
    rankCounts.fill(0);
    suitCounts.fill(0);
    let suitKey = 0;
    let pairRank = -1;
    let repeatedRanks = 0;
    for (let k = 0; k < 6; k++) {
      const card = u8[o + k];
      const rank = card >> 2;
      const suit = card & 3;
      if (++rankCounts[rank] === 2) {
        pairRank = rank;
        repeatedRanks++;
      }
      suitCounts[suit]++;
      suitKey += suitUnit[suit];
    }
    const onePair = repeatedRanks === 1 && rankCounts[pairRank] === 2;
    let pairedSuits = 0;
    for (const count of suitCounts) if (count >= 2) pairedSuits++;
    const canDoubleSuit = pairedSuits >= 2;
    if (!onePair && !canDoubleSuit) continue;

    const pairBest = [-Infinity, -Infinity, -Infinity]; // PLO, split, Hold’em
    let bestDouble = -Infinity;
    let bestOther = -Infinity;
    for (let q = 0; q < 15; q++) {
      const a = u8[o + PAIR_A[q]];
      const b = u8[o + PAIR_B[q]];
      const ev = dv.getInt16(o + 8 + 2 * q, true);
      if (onePair) {
        const placement = Number((a >> 2) === pairRank) + Number((b >> 2) === pairRank);
        pairBest[placement] = Math.max(pairBest[placement], ev);
      }
      if (canDoubleSuit) {
        const ploKey = suitKey - suitUnit[a & 3] - suitUnit[b & 3];
        if (doubleSuited.has(ploKey)) bestDouble = Math.max(bestDouble, ev);
        else bestOther = Math.max(bestOther, ev);
      }
    }

    if (onePair) {
      const group = pairRanks[pairRank];
      const best = Math.max(...pairBest);
      group.weight += weight;
      if (pairBest[2] === best) group.holdemBest += weight;
      for (let placement = 0; placement < 3; placement++) {
        group.loss[placement] += (best - pairBest[placement]) * weight;
      }
    }
    if (canDoubleSuit) {
      const pattern = suitPattern(suitCounts);
      let group = suitGroups.get(pattern);
      if (!group) {
        group = { weight: 0, doubleAhead: 0, doubleLoss: 0, otherLoss: 0 };
        suitGroups.set(pattern, group);
      }
      group.weight += weight;
      if (bestDouble > bestOther) group.doubleAhead += weight;
      group.doubleLoss += Math.max(0, bestOther - bestDouble) * weight;
      group.otherLoss += Math.max(0, bestDouble - bestOther) * weight;
    }
  }
  return { pairRanks, suitGroups: [...suitGroups], totalWeight };
}
