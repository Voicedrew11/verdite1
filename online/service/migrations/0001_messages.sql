-- Messages on the ground (docs/MESSAGES_PLAN.md, "A message on the wire and in the database").
-- Deviations there: a game is named once in games (the records hold its integer); a record's id and
-- a rating's record are the 8 bytes of its id, not an integer (a u64 does not fit a JavaScript
-- number); the unique index on an author's serial per game is what makes two posts race safely.
CREATE TABLE games (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE);
CREATE TABLE authors (id INTEGER PRIMARY KEY, key BLOB NOT NULL UNIQUE);
CREATE TABLE messages (
  id BLOB PRIMARY KEY,                   -- the record id, 8 bytes little-endian
  game INTEGER NOT NULL REFERENCES games (id),
  area INTEGER NOT NULL,
  place INTEGER NOT NULL,
  payload BLOB NOT NULL,                 -- the payload's own bytes, 0 to 8
  author INTEGER NOT NULL REFERENCES authors (id),
  serial INTEGER NOT NULL,
  good INTEGER NOT NULL DEFAULT 0,
  signature BLOB NOT NULL                -- 64 bytes
);
CREATE INDEX messages_area ON messages (game, area);
CREATE UNIQUE INDEX messages_author ON messages (game, author, serial);
CREATE TABLE ratings (
  message BLOB NOT NULL,
  rater INTEGER NOT NULL REFERENCES authors (id),
  PRIMARY KEY (message, rater)
) WITHOUT ROWID;
