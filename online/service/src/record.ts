// Records and ratings as bytes (online/protocol/PROTOCOL.md, "Records"): the layout, the id, the
// Ed25519 checks over the game's domain, and base64 for the JSON frames.
export const RECORD_BYTES = 118;
export const BODY_BYTES = 54;
export const RATING_BYTES = 104;
export const PAYLOAD_MOST = 8;
export const KEY_BYTES = 32;

// What a record holds, the payload as its own bytes (no zero padding).
export interface RecordFields {
  area: number;
  place: number;
  payload: Uint8Array;
  author: Uint8Array;
  serial: number;
  signature: Uint8Array;
}

// The 118 bytes of a record whose structure is right (not its signature): null otherwise.
export function decodeRecord(bytes: Uint8Array): RecordFields | null {
  if (bytes.length !== RECORD_BYTES || bytes[0] !== 1) return null;
  const size = bytes[9] ?? 0;
  if (size > PAYLOAD_MOST) return null;
  for (let i = 10 + size; i < 18; i++) if (bytes[i] !== 0) return null;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const serial = view.getUint32(50, true);
  if (serial === 0) return null;
  return {
    area: view.getUint32(1, true),
    place: view.getUint32(5, true),
    payload: bytes.slice(10, 10 + size),
    author: bytes.slice(18, 50),
    serial,
    signature: bytes.slice(54, 118),
  };
}

export function encodeRecord(fields: RecordFields): Uint8Array {
  const out = new Uint8Array(RECORD_BYTES);
  const view = new DataView(out.buffer);
  out[0] = 1;
  view.setUint32(1, fields.area, true);
  view.setUint32(5, fields.place, true);
  out[9] = fields.payload.length;
  out.set(fields.payload, 10);
  out.set(fields.author, 18);
  view.setUint32(50, fields.serial, true);
  out.set(fields.signature, 54);
  return out;
}

// The 8 bytes of a record's id: the first 8 of the SHA-256 of its body, little-endian, 0 made 1.
export async function recordId(bytes: Uint8Array): Promise<Uint8Array> {
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes.slice(0, BODY_BYTES)));
  const id = digest.slice(0, 8);
  if (id.every((b) => b === 0)) id[0] = 1;
  return id;
}

// The id as the JSON frames write it: 16 lowercase hex digits of the number.
export function idHex(id: Uint8Array): string {
  return toHex(id.slice().reverse());
}

export function toHex(bytes: Uint8Array): string {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

// A rating's 104 bytes: the record's id, the rater's key, the signature.
export interface RatingFields {
  id: Uint8Array;
  rater: Uint8Array;
  signature: Uint8Array;
}

export function decodeRating(bytes: Uint8Array): RatingFields | null {
  if (bytes.length !== RATING_BYTES) return null;
  return { id: bytes.slice(0, 8), rater: bytes.slice(8, 40), signature: bytes.slice(40, 104) };
}

// Ed25519 over the game's domain, a zero byte, then the signed bytes. A key that is not a key
// is a failed check, not an error.
async function verifySigned(publicKey: Uint8Array, signature: Uint8Array, domain: string, signed: Uint8Array): Promise<boolean> {
  try {
    const key = await crypto.subtle.importKey("raw", publicKey, { name: "Ed25519" }, false, ["verify"]);
    const message = new Uint8Array([...new TextEncoder().encode(domain), 0, ...signed]);
    return await crypto.subtle.verify("Ed25519", key, signature, message);
  } catch {
    return false;
  }
}

// Whether a record's signature is its author's, over its body (the first 54 bytes).
export function verifyRecord(game: string, fields: RecordFields, bytes: Uint8Array): Promise<boolean> {
  return verifySigned(fields.author, fields.signature, `verdite-record/${game}`, bytes.slice(0, BODY_BYTES));
}

export function verifyRating(game: string, rating: RatingFields): Promise<boolean> {
  const signed = new Uint8Array(40);
  signed.set(rating.id, 0);
  signed.set(rating.rater, 8);
  return verifySigned(rating.rater, rating.signature, `verdite-rating/${game}`, signed);
}

// The bytes of a base64 string when it decodes to exactly length bytes, else null.
export function fromBase64(text: unknown, length: number): Uint8Array | null {
  if (typeof text !== "string") return null;
  let binary: string;
  try {
    binary = atob(text);
  } catch {
    return null;
  }
  if (binary.length !== length) return null;
  return Uint8Array.from(binary, (c) => c.charCodeAt(0));
}

export function toBase64(bytes: Uint8Array): string {
  let binary = "";
  for (const b of bytes) binary += String.fromCharCode(b);
  return btoa(binary);
}
