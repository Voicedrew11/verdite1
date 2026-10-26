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
