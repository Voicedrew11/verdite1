# online

Online play for the Verdite ports (`docs/NETWORKING_PLAN.md`): the service and the client library,
kept in this repo for now and moved out to their own repo once the protocol settles. Nothing here
knows a game; the game layer (vocabulary, ghosts, coop sync) stays in `port/` and `patches/`.

| folder | layer | what |
|---|---|---|
| `protocol/` | both | the wire protocol, the one definition the service and the client both follow |
| `service/` | service | the Cloudflare Worker: TURN credentials and the signaling rooms (a Durable Object per room) |
| `client/` | client library | `verdite-online`: WebRTC data channels through Cloudflare TURN, behind a C interface |

The client depends on neither `libverdite/` nor the game, so it can move out as a subtree, and the
C# recompilations can reach it through P/Invoke.

## The service

TypeScript on Cloudflare Workers, run with wrangler (`npm ci` in `service/` first):

```
cd online/service
npm ci
npm run check                   # type-check
npm run dev                     # wrangler dev on http://127.0.0.1:8787, no TURN key: host candidates only
npm test                        # the signaling test, against a running npm run dev
npx wrangler deploy             # to verdite-online.<account>.workers.dev
SERVICE=https://... npm test    # the signaling test against the deployed Worker
SERVICE=https://... npm run test:turn   # TURN servers come with credentials (never printed)
```

Without a TURN key, rooms hand out no ICE servers, which is enough for two instances on one
machine; local development never needs the key.

## Secrets

The TURN key (`TURN_KEY_ID`, `TURN_KEY_API_TOKEN`) lives only in Cloudflare, as the deployed
Worker's secrets. It is never in a file in this repo, not even an ignored one, and never reaches a
client: clients get a TURN credential per room visit, for 6 hours, revoked when they leave.

- Set it with `npx wrangler secret put TURN_KEY_ID` (and `TURN_KEY_API_TOKEN`) in your own
  terminal, typing it at the prompt: not piped from `echo` (shell history), not in `.dev.vars`.
- wrangler's own login is in `~/.config/.wrangler`, outside the repo.
- The guards: `.gitignore` leaves out `.dev.vars*`, `.env*`, `*.pem` and `.wrangler/`; the
  pre-commit hook (`git config core.hooksPath scripts/hooks`) refuses those files by name and
  runs gitleaks on the staged changes; `scripts/check.sh` scans the history and the uncommitted
  changes. `.gitleaks.toml` allows only the settings-key names that look like keys.
