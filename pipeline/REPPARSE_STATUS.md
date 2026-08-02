# Status of the .rep header parser

`repparse.py` (formerly `scripts/soku_replay_parser.py`) describes its own
header offsets as "approximate, based on community reverse-engineering". They
were never checked against real files. Running it over all 397 replays in
`~/.wine-soku/drive_c/Games/Soku/replay` on 2026-08-01 gives:

| field | result | verdict |
|---|---|---|
| `card_count` | `20` for 397/397 | **trustworthy** — matches the game's fixed deck size |
| `p1.character_id` | Remilia 351, Sakuya 25, Alice 20, Reimu 1 | **unverified** — a 88% single-character skew is possible for one player's own replays, but has not been confirmed against known matchups |
| `stage` | "Forest of Magic" for 397/397 | **wrong** — a constant, not parsed data |
| full parse (header + inputs) | 131 ok / 266 fail | **unreliable** — the deflate input section fails on two thirds of files |

## What this means for the pipeline

Nothing downstream depends on these fields today. `meta.json` records the
source `.rep` path and its SHA-256, which is what makes a capture attributable;
matchup metadata is a milestone-3 concern (conditioning the model on
characters/stage).

**Do not** write these fields into `meta.json` as if they were facts. Either
confirm the offsets against replays with known matchups first, or read the
matchup off the captured frames instead — the character portraits are on screen
every frame, and by then we have the video anyway.
