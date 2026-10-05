# Verdite1

A static recompilation of **King's Field** (`SLPS-00017`), the first game in the
series, released only in Japan in 1994, using
[RecompOne](https://github.com/BlackLabelHQ/RecompOne). The series was renumbered
for the West: the game sold in North America as "King's Field" is the Japanese
*King's Field II* (Verdite2), and the North American "King's Field II" is the
Japanese *King's Field III* (Verdite3). This project is the one before both.

You must supply your own dump of `SLPS-00017`. No disc data is included, and
none ever will be.

**Status: bootstrapping.** The state of each piece is in `NOTES.md`.
`tools/RecompOne` is a subtree of the shared fork `Voicedrew11/verdite-recompone`,
and `tools/verdite-core` of the code shared with Verdite2 and Verdite3.

## No prebuilt binary

A playable binary cannot be shipped. The generated code is a translation of
FromSoftware's own code, so the assembly that plays the game has to be built on
the machine of somebody who owns the disc. The project will ship its inputs and
build the game at first run instead, as Verdite2 and Verdite3 do.

## Upstream

This project is built on the RecompOne fork, not directly on upstream RecompOne.
No pull requests and no issues go to upstream RecompOne; see `AGENTS.md`.
