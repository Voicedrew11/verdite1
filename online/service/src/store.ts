// The records in D1 (migrations/0001_messages.sql). Every write is one batch, so it is all or nothing.
import { toHex } from "./record";

// A stored record: its id as hex (the cache's key) and the fields a record is made of.
export interface Held {
  hex: string;
  id: Uint8Array;
  area: number;
  place: number;
  payload: Uint8Array;
  author: Uint8Array;
  serial: number;
  good: number;
  signature: Uint8Array;
}

interface Row {
  id: unknown;
  area: number;
  place: number;
  payload: unknown;
  serial: number;
  good: number;
  signature: unknown;
  author: unknown;
}

// D1 hands a BLOB back as an ArrayBuffer (or an array); a NULL is the empty blob.
function bytesOf(value: unknown): Uint8Array {
  if (value === null || value === undefined) return new Uint8Array(0);
  if (value instanceof ArrayBuffer) return new Uint8Array(value);
  if (ArrayBuffer.isView(value)) return new Uint8Array(value.buffer, value.byteOffset, value.byteLength).slice();
  if (Array.isArray(value)) return Uint8Array.from(value as number[]);
  throw new Error("a BLOB column came back as something else");
}

// A binding of exactly these bytes (a view's buffer may be larger).
function blob(bytes: Uint8Array): ArrayBuffer {
  return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) as ArrayBuffer;
}

function held(row: Row): Held {
  const id = bytesOf(row.id);
  return {
    hex: toHex(id.slice().reverse()),
    id,
    area: row.area,
    place: row.place,
    payload: bytesOf(row.payload),
    author: bytesOf(row.author),
    serial: row.serial,
    good: row.good,
    signature: bytesOf(row.signature),
  };
}

const SELECT = `SELECT m.id, m.area, m.place, m.payload, m.serial, m.good, m.signature, a.key AS author
  FROM messages m JOIN games g ON g.id = m.game JOIN authors a ON a.id = m.author`;

export async function floorRecords(db: D1Database, game: string, area: number): Promise<Held[]> {
  const { results } = await db
    .prepare(`${SELECT} WHERE g.name = ? AND m.area = ?`)
    .bind(game, area)
    .all<Row>();
  return results.map(held);
}

// An author's records out in the game, in any area, newest first.
export async function mineRecords(db: D1Database, game: string, author: Uint8Array): Promise<Held[]> {
  const { results } = await db
    .prepare(`${SELECT} WHERE g.name = ? AND a.key = ? ORDER BY m.serial DESC`)
    .bind(game, blob(author))
    .all<Row>();
  return results.map(held);
}

// The author's newest serial in the game, or null when it has none out.
export async function newestSerial(db: D1Database, game: string, author: Uint8Array): Promise<number | null> {
  const row = await db
    .prepare(`SELECT MAX(m.serial) AS serial FROM messages m JOIN games g ON g.id = m.game JOIN authors a ON a.id = m.author WHERE g.name = ? AND a.key = ?`)
    .bind(game, blob(author))
    .first<{ serial: number | null }>();
  return row?.serial ?? null;
}

// The records a new serial retires: the author's at serial or below (in the game).
export async function retiredBy(db: D1Database, game: string, author: Uint8Array, serial: number): Promise<{ id: Uint8Array; area: number }[]> {
  const { results } = await db
    .prepare(`SELECT m.id, m.area FROM messages m JOIN games g ON g.id = m.game JOIN authors a ON a.id = m.author WHERE g.name = ? AND a.key = ? AND m.serial <= ?`)
    .bind(game, blob(author), serial)
    .all<{ id: unknown; area: number }>();
  return results.map((row) => ({ id: bytesOf(row.id), area: row.area }));
}

const RETIRING = `SELECT m.id FROM messages m JOIN games g ON g.id = m.game JOIN authors a ON a.id = m.author
  WHERE g.name = ? AND a.key = ? AND m.serial <= ?`;

// One record posted, and the author's records it retires, in one batch. Fails (and changes nothing)
// when the author already has a record of this serial in the game.
export async function insertPost(db: D1Database, game: string, record: { id: Uint8Array; area: number; place: number; payload: Uint8Array; author: Uint8Array; serial: number; signature: Uint8Array }): Promise<void> {
  const author = blob(record.author);
  const retire = record.serial - 10;
  await db.batch([
    db.prepare("INSERT OR IGNORE INTO games (name) VALUES (?)").bind(game),
    db.prepare("INSERT OR IGNORE INTO authors (key) VALUES (?)").bind(author),
    db
      .prepare(
        `INSERT INTO messages (id, game, area, place, payload, author, serial, signature)
         VALUES (?, (SELECT id FROM games WHERE name = ?), ?, ?, ?, (SELECT id FROM authors WHERE key = ?), ?, ?)`,
      )
      .bind(blob(record.id), game, record.area, record.place, blob(record.payload), author, record.serial, blob(record.signature)),
    db.prepare(`DELETE FROM ratings WHERE message IN (${RETIRING})`).bind(game, author, retire),
    db.prepare(`DELETE FROM messages WHERE id IN (${RETIRING})`).bind(game, author, retire),
  ]);
}

// A rating in one batch: whether it was new, and the record's good count after it.
export async function insertRating(db: D1Database, id: Uint8Array, rater: Uint8Array): Promise<{ inserted: boolean; good: number }> {
  const [, insert, , read] = await db.batch<{ good: number }>([
    db.prepare("INSERT OR IGNORE INTO authors (key) VALUES (?)").bind(blob(rater)),
    db
      .prepare("INSERT OR IGNORE INTO ratings (message, rater) VALUES (?, (SELECT id FROM authors WHERE key = ?))")
      .bind(blob(id), blob(rater)),
    db
      .prepare("UPDATE messages SET good = (SELECT COUNT(*) FROM ratings WHERE message = ?) WHERE id = ?")
      .bind(blob(id), blob(id)),
    db.prepare("SELECT good FROM messages WHERE id = ?").bind(blob(id)),
  ]);
  return { inserted: (insert?.meta.changes ?? 0) > 0, good: read?.results[0]?.good ?? 0 };
}
