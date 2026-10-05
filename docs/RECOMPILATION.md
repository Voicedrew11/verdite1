# Recompilation: config, function maps and SDK addresses

How the recompiler turns the disc's MIPS into C#: the overlays in
`config/kf1.json`, the swept function maps under `config/funcmaps/`, and mapping
PSY-Q entry points by address to the runtime's HLE. Verdite3's
`docs/RECOMPILATION.md` is the model, and it documents the traps to expect here
too — data swept as code past the end of the text, false starts from `jal`-shaped
data words, and switch tables read past their end. Which of them apply to this
disc, and what is new, goes below.

## Status
