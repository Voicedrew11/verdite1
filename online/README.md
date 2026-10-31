# online

Online play for the Verdite ports (`docs/NETWORKING_PLAN.md`): the service and the client library,
kept in this repo for now and moved out to their own repo once the protocol settles. Nothing here
knows a game; the game layer (vocabulary, ghosts, coop sync) stays in `port/` and `patches/`.

| folder | layer | what |
|---|---|---|
| `protocol/` | both | the wire protocol, the one definition the service and the client both follow |
| `service/` | service | the Cloudflare Worker: TURN credentials, the signaling rooms (a Durable Object per room) and the list of public rooms (one per game and build) |
| `client/` | client library | `verdite-online`: WebRTC data channels through Cloudflare TURN, behind a C interface (`include/verdite_online.h`) |

The client depends on neither `libverdite/` nor the game, so it can move out as a subtree, and the
C# recompilations can reach it through P/Invoke.

## The service

TypeScript on Cloudflare Workers, run with wrangler (`npm ci` in `service/` first):

```
cd online/service
npm ci
npm run check                   # type-check
npm run dev                     # wrangler dev on http://127.0.0.1:8787, no TURN key: host candidates only
npx wrangler d1 migrations apply verdite-online --local   # the records' tables, once per local state
npm test                        # the signaling test, against a running npm run dev
npm run test:records            # records: posts, refusals, ratings, samples, mine, retirement (about a minute)
npm run test:mesh               # the floor mesh: links, signals, a relink after a leave
npx wrangler deploy             # to verdite-online.<account>.workers.dev
SERVICE=https://... npm test    # the signaling test against the deployed Worker
SERVICE=https://... npm run test:turn   # TURN servers come with credentials (never printed)
```

The records (`/v1/floors/...`, a Durable Object per floor) keep their rows in D1 (`migrations/`).
Locally, `wrangler dev` needs the migration above first. The `database_id` in `wrangler.jsonc` is a
placeholder: a deploy needs the account's real database (and its migration) before it.

Without a TURN key, rooms hand out no ICE servers, which is enough for two instances on one
machine; local development never needs the key.

`TURN_CREDENTIAL_SECONDS` (60 to 3600, default 3600) shortens the credentials a room hands out, to
test their renewal; it is never set on the live service except for such a test. Locally:
`npx wrangler dev --ip 127.0.0.1 --port 8787 --var TURN_CREDENTIAL_SECONDS:60`, then
`TURN_CREDENTIAL_SECONDS=60 npm test` and `vo_pair --seconds 120` (a move every 30 seconds).

## The client

C++20 behind the C interface in `client/include/verdite_online.h`, on libdatachannel 0.24.1
(libjuice, usrsctp, OpenSSL), built from source with its submodules: the nix shell gives
`KF_LIBDATACHANNEL_SOURCE`, and the top-level CMake builds `online/client` when it is set
(desktop only for now). libdatachannel's threads only post what they see to a mailbox; `vo_poll`
acts on it on the game's thread, so the session has one thread. The client asks for a TURN
credential when it first has a peer and again halfway through its life (15 minutes before its end
at most), and moves each connection to a new one made with it, without losing or reordering a
CONTROL packet (`protocol/PROTOCOL.md`, "Moving to a new credential"; the ordering rules are
`src/handover.h`, unit-tested). A closed connection is kept until libdatachannel reports it
closed, so `vo_close` waits for that, at most 2 seconds.

```
cmake --build --preset linux --target verdite_online vo_pair vo_board_trio verdite_online_tests
build/linux/online/client/verdite_online_tests             # the frames, read and written
build/linux/online/client/vo_pair --service http://127.0.0.1:8787 --seconds 10
build/linux/online/client/vo_pair --service https://verdite-online.voicedrew11.workers.dev
build/linux/online/client/vo_pair --service http://127.0.0.1:8787 --seconds 3 --listed 1
build/linux/online/client/vo_board_trio --service http://127.0.0.1:8787
```

With `--listed 1` the room has a public code, the first session lists it and says it is on floor
7, and the run passes only if the list of public rooms (`vo_lobby_open`) shows it with both
players.

`vo_pair` opens two sessions in one room and sends CONTROL and STATE packets both ways at 20 a
second, plus one 64 KiB CONTROL packet; it passes when every CONTROL packet arrived in order and
neither side lost the other. The library logs each connection's route to stderr: `relayed`
through TURN, or `direct` when the service has no TURN key.

Records (`vo_board_*`, `protocol/PROTOCOL.md` "Records") use the floor's records socket and the
floor mesh: a board's calls run on the game's thread, and the sockets' threads only post to a
mailbox. `vo_board_trio` runs three boards on area 7 of game `test` and passes when a post reaches
the other two by gossip, is rated, and retires the first of ten more.

- libjuice speaks TURN over UDP only, so a network that blocks UDP cannot connect.
- ThreadSanitizer reports races inside usrsctp (libdatachannel's SCTP) and a lock-order
  inversion in libdatachannel's WebSocket, none in the client's own code; UBSan reports
  function-pointer casts in libjuice's `picohash.h` (2026-10-26).

## Secrets

The TURN key (`TURN_KEY_ID`, `TURN_KEY_API_TOKEN`) lives only in Cloudflare, as the deployed
Worker's secrets. It is never in a file in this repo, not even an ignored one, and never reaches a
client: a client gets a TURN credential only when it has a peer to connect to, for an hour,
revoked when it leaves or asks again.

## Abuse and cost

The URL is public and anything can speak the protocol, so the service cannot tell the game from
another program; it limits what anyone can take instead:

- credentials only to a peer with another peer in its room, for an hour, revoked on leaving;
- per address: 30 joins and 10 credentials a minute (approximate, per Cloudflare location);
- the month's budget: a scheduled check every 10 minutes reads the TURN key's egress (what
  Cloudflare bills, $0.05/GB past 1,000 GB a month) from the analytics API, and past
  `TURN_BUDGET_GB` (500) rooms get `unavailable` instead of credentials. Cloudflare itself has
  no cap and offers no usage alert on an account without usage-based billing.

Still to come: a key per install, so limits and bans apply per player (`docs/NETWORKING_PLAN.md`).

- Set each with `npx wrangler secret put <NAME>` in your own terminal, the name as the argument
  and the value typed at the prompt: not on the command line, not piped from `echo` (shell
  history), not in `.dev.vars`.
- The budget's check needs two more: `CF_ACCOUNT_ID` (from `npx wrangler whoami`) and
  `CF_ANALYTICS_TOKEN`, an API token with only Account → Account Analytics → Read. Without them
  there is no cap.
- wrangler's own login is in `~/.config/.wrangler`, outside the repo.
- The guards: `.gitignore` leaves out `.dev.vars*`, `.env*`, `*.pem` and `.wrangler/`; the
  pre-commit hook (`git config core.hooksPath scripts/hooks`) refuses those files by name and
  runs gitleaks on the staged changes; `scripts/check.sh` scans the history and the uncommitted
  changes. `.gitleaks.toml` allows only the settings-key names that look like keys.
