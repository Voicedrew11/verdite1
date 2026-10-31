// A floor's sample (online/protocol/PROTOCOL.md, "A floor's records"; docs/MESSAGES_PLAN.md, "The
// fraction a player sees"): at most 2 records in each 8 by 8 block of cells and 24 in all, drawn
// without replacement, weighted by 1 + good, from a seed. Deterministic: the same records and seed
// give the same sample.

export const SAMPLE_PER_BLOCK = 2;
export const SAMPLE_MOST = 24;

export interface Sampled {
  hex: string; // the record's id, the order that makes the draw deterministic
  place: number; // the cell x is its low byte, z its second
  good: number;
}

// mulberry32: a 32-bit generator from a 32-bit seed, values in [0, 1).
function generator(seed: number): () => number {
  let state = seed >>> 0;
  return () => {
    state = (state + 0x6d2b79f5) >>> 0;
    let t = state;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function blockOf(place: number): string {
  const x = place & 0xff;
  const z = (place >>> 8) & 0xff;
  return `${x >> 3},${z >> 3}`;
}

export function sample<T extends Sampled>(records: T[], seed: number): T[] {
  const next = generator(seed);
  const ordered = [...records].sort((a, b) => (a.hex < b.hex ? -1 : a.hex > b.hex ? 1 : 0));
  // Efraimidis-Spirakis: a key log(u) / weight; the largest keys win.
  const keyed = ordered.map((record) => {
    const u = next() + 2 ** -33; // in (0, 1)
    return { record, key: Math.log(u) / (1 + record.good) };
  });
  keyed.sort((a, b) => b.key - a.key);
  const perBlock = new Map<string, number>();
  const chosen: T[] = [];
  for (const { record } of keyed) {
    if (chosen.length >= SAMPLE_MOST) break;
    const block = blockOf(record.place);
    const taken = perBlock.get(block) ?? 0;
    if (taken >= SAMPLE_PER_BLOCK) continue;
    perBlock.set(block, taken + 1);
    chosen.push(record);
  }
  return chosen;
}
