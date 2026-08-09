# Status of the .rep header parser

> **UPDATE 2026-08-07 — the character offsets were wrong, and are now fixed.**
>
> The suspicion below was correct. Over the 3010 `.rep` files in
> `Smashlytics/soku-frames-a` (`metadata/source_reps_A.tar`), offset `0x08` gives
> **Remilia for 100% of files** and the derived P2 offset gives Reimu for 100%.
> Not a skew — a constant.
>
> The real layout, found by scanning which header bytes vary across files and
> keeping those confined to 0-19:
>
> | field | offset | evidence |
> |---|---|---|
> | P1 character | **0x0E** | exactly 20 distinct values over 400 files |
> | P1 palette | 0x0F | 8 distinct values, adjacent to the character |
> | P2 character | **0x3F** | exactly 20 distinct values |
> | P2 palette | 0x40 | 8 distinct values, adjacent |
>
> The (character, palette) adjacency the old parser expected at 0x08/0x09 is
> real; it just sits six bytes later. With the corrected offsets the corpus shows
> all 20 characters on both sides with a plausible popularity curve (Sakuya 10.7%
> … Suwako 1.5%), and Cirno in 4.3% of replays.
>
> **Verified against ground truth, not just plausibility:** 157 replays supplied
> as known Cirno matches parse as containing Cirno in 122/122 of the ones that
> should (the other 35 were unrelated matches in the same archives). The old
> offset calls every one of them Remilia.
>
> The `stage` field is still wrong and the deflate input section is still
> unreliable; nothing below that concerns characters has been re-checked.

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
