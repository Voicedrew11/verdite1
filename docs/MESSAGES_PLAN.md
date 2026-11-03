# Messages plan

**Draft, 2026-10-30. Subject to change**; edit the section and add a line to "Changes" rather
than leaving old text standing. Nothing is built yet. Part of [NETWORKING_PLAN.md](NETWORKING_PLAN.md)
(the asynchronous tier's messages, and the realtime tier's gossip).

Players leave short messages on the floor of a cell: a fill-in-the-blank phrase, chosen from a
few templates and a fixed vocabulary, never free text. Other players find a mark there, face it,
press the action button and read it in the game's message box, and may rate it good. Each player
sees only a fraction of a floor's messages.

## Decisions (2026-10-30)

| question | decided |
|---|---|
| how messages spread | a **floor mesh**: each online player keeps 2 or 3 links to random players on the same floor, through TURN, and gossips records over them; a **Durable Object per floor** is the cache over a D1 table that keeps them while nobody plays |
| the fraction a player sees | a **rated random sample**: at most a few per area of the floor and a cap per floor, drawn at random weighted by rating, drawn again each session |
| in the world | a **mark on the cell's floor**; facing it and pressing action shows the text in the game's message box, with a choice to rate it good |
| lifetime | **permanent until replaced**: a player has at most **10** out; an 11th retires their oldest. Ratings change only how often one is shown |
| vocabulary | **the disc's item names** (both languages, no translation) **plus a hand-written list** (directions, features, actions, enemies, people), which we translate |
| writing | **the Message Stone**, an item in the Use Item list while messages are on and a floor is entered; using it opens a menu in the game's style, and the message is placed on the cell the player stands on, facing where they look |
| reward for a good rating | **none**; a player sees their messages' counts under the Message Stone's Your messages. The game played alone is unchanged |

## A message on the wire and in the database

A message is the game's few bytes (the payload) inside a record the client library signs. The
library knows no game: for it a record is an area (a floor), a place (a cell and facing) and a
payload of at most 8 bytes.

The signed body, little-endian, 54 bytes; the signature covers it with the game's name in front
(`verdite-record/kf1`), so a record cannot be replayed into another game:

| bytes | field |
|---|---|
| 1 | version, 1 |
| 4 | area: KF1's floor |
| 4 | place: KF1's cell x, cell z, facing (256ths of a turn), 0 |
| 1 | payload size, at most 8 |
| 8 | payload, zero past its size |
| 32 | author: the install's Ed25519 public key |
| 4 | serial: the author's count of records posted, from 1 |

then 64 bytes of Ed25519 signature: 118 bytes in all. A record's id is the first 8 bytes of the
SHA-256 of its signed body.

KF1's payload is 5 bytes: the template (1 byte) and two words (2 bytes each, `0xffff` for none).
A word id names its kind in its top nibble, so the tables only ever grow at the end:

| ids | words |
|---|---|
| `0x0000`–`0x0fff` | hand-written, append-only, translated in our table |
| `0x1000`–`0x10ff` | an item, by the game's item id; its name read from the disc in the reader's language |

The text is made where it is read, in the reader's language, so the English and Japanese builds
share every message (an open question in the networking plan, answered for messages).

**D1** stores a message as small integers, with the author's key once in its own table: the
signature is the only wide column, kept so a record the service serves can be checked by
whoever it is gossiped to.

```sql
CREATE TABLE authors  (id INTEGER PRIMARY KEY, key BLOB NOT NULL UNIQUE);
CREATE TABLE messages (id INTEGER PRIMARY KEY,           -- the record id
                       game INTEGER, area INTEGER, place INTEGER,
                       payload BLOB, author INTEGER, serial INTEGER,
                       good INTEGER NOT NULL DEFAULT 0, signature BLOB NOT NULL);
CREATE INDEX messages_area   ON messages (game, area);
CREATE INDEX messages_author ON messages (author, serial);
CREATE TABLE ratings  (message INTEGER, rater INTEGER, PRIMARY KEY (message, rater)) WITHOUT ROWID;
```

About 90 bytes a message with its indexes; with 10 a player, a million players is under 1 GB.

## The service

The client library has WebSockets (libdatachannel's) and no HTTP client, so a player reaches a
floor's records over a WebSocket to the floor object, `/v1/floors/<game>/<area>/records`
(`online/protocol/PROTOCOL.md`, "A floor's records"), not HTTPS; the same requests over HTTPS,
for the 3DS, come when that port does.

- **On opening**: the floor's sample (below) from a seed the client sends.
- **`post`**: a signed record. The floor object checks the signature, the payload size, the area
  and a rate limit per address, then in one D1 batch adds the author if new, inserts the record
  and deletes the author's records at or below the new serial less 10, and tells the floor
  objects that held them.
- **`rate`**: a signed rating (rater's key, record id). One per rater and record; a rater cannot
  rate their own.
- **`mine`**: an author's records out, in any area, with their counts.
- **The floor object** (Durable Object per game and floor) is the cache: it loads its floor's
  records from D1 when it wakes, keeps them and their counts in memory, answers samples, and
  is the floor mesh's membership (below). It writes counts back to D1 in batches.

## Gossip: the floor mesh

The realtime tier only; the 3DS will read samples and post over HTTPS.

- **Membership**: entering a floor joins its mesh at the floor object, which answers with up to
  3 random players already there; the newcomer connects to those (the signaling, TURN and
  credentials of rooms, unchanged). A player accepts links until it has 4, so the mesh stays
  connected without anyone holding many. A floor mesh is separate from any coop room, and keyed
  by game and floor, not build: the record format is the same in every build.
- **On a new link**, each side sends the records it shows (its sample), so a player's pool is
  their own sample and their neighbours'.
- **A new post** is flooded: sent to every link, forwarded once by each player who had not seen
  its id, at most 3 hops.
- **Every record received is checked** (signature, size, retirement) before it is kept or
  forwarded; a player who sends a bad one is dropped.
- **Retirement travels by serial**: holding a record of serial s from an author, a player drops
  that author's records at or below s less 10, so the cap holds in the mesh without the service.

Cost: a record is 118 bytes, and a link sends a sample on opening (24 records, under 3 KB) and
then only new posts. The pose traffic of coop dwarfs it.

## The fraction a player sees

Which of a floor's records the game draws, from everything it holds (the service's sample and
gossip): **at most 2 in each 8 by 8 block of cells, and 24 on the floor**, drawn without
replacement with weight 1 + good, from a seed new each session. A record shown stays shown for
the session while new ones arrive, unless its author retires it. The service draws its sample
the same way, so what a player is sent is already about what they see. Their own records are
always shown to them and do not count.

## The game layer (KF1)

- **Templates**: about 12, in the game's voice, each with one or two blanks. The English and
  Japanese text live in a table in `port/ext/messages.cpp`.
- **The hand-written words**: about 60 to start: directions, features (hidden wall, pit, door,
  lever), actions, enemies and people (the game names neither on screen, so they are ours),
  conditions (poison, darkness). Japanese is drawn with the game's Japanese font page, so the
  Japanese words must use glyphs that page has: kana, and only kanji it holds.
- **Writing**: the Message Stone, an item in Use Item while messages are on and a floor is entered,
  opens an in-game menu: it picks a template, then each blank's word (by kind, then the word),
  shows the result, and posts it on the player's cell.
- **The mark and reading**: a patch draws a mark on the floor of each shown record's cell, the
  signboard's carved face laid flat, tinted green and turned by the writer's facing, and
  the action button facing one opens it in the game's message box with "Good" and "Leave".
- **Online off, or offline**: nothing is drawn or posted, and the Message Stone is not offered.

## Interfaces (proposed)

- `online/client/include/verdite_online.h`, "Records": identity, the board (enter a floor,
  post, rate, the floor's records), all game-agnostic.
- `libverdite/include/verdite/message_sample.h`: which records are shown, the fraction above.
- `port/ext/messages.h`: KF1's templates, vocabulary, payload and text.

## Order

1. The record format and identity in the client, with tests (sign, check, retire by serial).
2. D1 tables, posting, ratings and the floor object's samples in the service, tested against
   `wrangler dev`.
3. `message_sample.h` in libverdite, with tests.
4. KF1's templates, vocabulary and text, with tests; the overlay writer; then the mark and
   reading patch. Messages work here through the floor's records socket alone.
5. The floor mesh: membership in the floor object, the client's gossip, tested with
   `scripts/coop_pair.py`-style pairs and a trio on one floor.

## Open questions

- The template wording and the word list, in both languages (to be judged by the user).
- What the mark looks like (judged by eye, the user's).
- Moderation beyond templates: whether a poorly placed message needs a "bad" rating at all.

## Changes

- 2026-10-30: first draft, from the questions answered that day.
- 2026-10-30: the floor's records are reached over a WebSocket, not HTTPS: the client library has
  no HTTP client. No bad rating. The game's text comes as pieces (`message_pieces`), since an
  item's name is drawn from the disc's glyph row, not spelled.
- 2026-10-31: writing moved from the overlay's "Leave a message" window to the Message Stone, an
  item in Use Item (`port/ext/extra_items.cpp`), whose menu is drawn in the game's style, not ImGui.
