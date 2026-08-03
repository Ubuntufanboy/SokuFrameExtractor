"""Fetch a replay corpus from sokureplays.delthas.fr.

The site is a Vue SPA over a JSON backend at ``sokureplaysback.delthas.fr``:

    GET /replays?order=last&offset=N&limit=50   ->  {"replays": [...], "more": bool}
    GET /download_replay/<id>                   ->  the .rep, as an attachment

``limit`` is capped at 50 server-side regardless of what you ask for, so
paging is ``offset += 50`` until ``more`` goes false.

WHY THE METADATA MATTERS AS MUCH AS THE FILES
---------------------------------------------
Each listing entry carries what the .rep header parser could never be trusted
to give us (see REPPARSE_STATUS.md -- it reports "Forest of Magic" as the
stage for all 397 local replays, and fails a full parse on 67% of them):

    serverCharacter / clientCharacter    character ids
    serverCards / clientCards            the full 20-card decks
    serverUserElo / clientUserElo        skill, both sides
    winner, serverRounds, clientRounds   the outcome
    unranked, mountainVaporBug           quality flags

That is ground-truth conditioning data, and it arrives free with the download.
It is written to ``index.jsonl`` next to the files, one object per replay.

BEING A GOOD CITIZEN
--------------------
This hits someone else's server a few thousand times. So: one request at a
time, a deliberate delay between them, exponential backoff that honours
Retry-After, a User-Agent that says who we are and how to complain, and full
resumability so an interrupted run never re-downloads. The defaults are
deliberately unhurried -- a corpus fetch that takes half an hour and nobody
notices beats one that takes two minutes and gets us blocked.
"""

from __future__ import annotations

import argparse
import json
import random
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

API = "https://sokureplaysback.delthas.fr"
PAGE = 50                      # server-side cap; asking for more returns 50

USER_AGENT = (
    "SokuFrameExtractor/1.0 (world-model dataset collection; "
    "+https://github.com/Ubuntufanboy/SokuFrameExtractor)"
)

DEFAULT_DELAY_S = 0.35
MAX_RETRIES = 5


class Fetcher:
    """One connection's worth of politeness: paced, retrying, identifiable."""

    def __init__(self, delay_s: float = DEFAULT_DELAY_S, timeout_s: float = 60.0):
        self.delay_s = delay_s
        self.timeout_s = timeout_s
        self._last = 0.0

    def _pace(self) -> None:
        wait = self.delay_s - (time.monotonic() - self._last)
        if wait > 0:
            time.sleep(wait)
        self._last = time.monotonic()

    def get(self, url: str) -> bytes:
        for attempt in range(MAX_RETRIES):
            self._pace()
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            try:
                with urllib.request.urlopen(req, timeout=self.timeout_s) as r:
                    return r.read()
            except urllib.error.HTTPError as e:
                if e.code in (404, 410):
                    raise                       # genuinely gone; not worth retrying
                # Respect an explicit Retry-After; otherwise back off with
                # jitter so a retry storm cannot synchronise across workers.
                retry_after = e.headers.get("Retry-After") if e.headers else None
                delay = (float(retry_after) if retry_after and retry_after.isdigit()
                         else (2.0 ** attempt) + random.random())
                if attempt == MAX_RETRIES - 1:
                    raise
                print(f"    HTTP {e.code}; retrying in {delay:.1f}s", file=sys.stderr)
                time.sleep(delay)
            except (urllib.error.URLError, TimeoutError) as e:
                if attempt == MAX_RETRIES - 1:
                    raise
                delay = (2.0 ** attempt) + random.random()
                print(f"    {e}; retrying in {delay:.1f}s", file=sys.stderr)
                time.sleep(delay)
        raise RuntimeError("unreachable")


def list_page(f: Fetcher, offset: int, order: str = "last") -> tuple[list[dict], bool]:
    q = urllib.parse.urlencode({"order": order, "offset": offset, "limit": PAGE})
    d = json.loads(f.get(f"{API}/replays?{q}"))
    return d.get("replays", []), bool(d.get("more"))


def download_one(f: Fetcher, replay_id: int, dest: Path) -> int:
    """Fetch one .rep to `dest`. Returns bytes written."""
    data = f.get(f"{API}/download_replay/{replay_id}")
    # Write via a temporary and rename, so an interrupted run never leaves a
    # truncated .rep that a later --resume would mistake for a finished one.
    tmp = dest.with_suffix(".part")
    tmp.write_bytes(data)
    tmp.rename(dest)
    return len(data)


# Identity fields. `serverHidden`/`clientHidden` mean the player asked the
# site not to display their rank badge -- a presentation preference, not a
# restriction on the replay, which the backend serves to anyone. Filtering on
# it discarded 80% of the corpus for nothing.
#
# The preference is still worth honouring in the only way that costs us
# nothing: the corpus does not need nicknames. Characters, decks, Elo and the
# outcome are what a world model conditions on; who played is irrelevant. So
# nicknames are dropped by default and the numeric user ids are kept, which
# is all that de-duplication or per-player analysis actually needs.
NICKNAME_FIELDS = ("serverUserNick", "clientUserNick")


def strip_identity(r: dict, *, keep_nicknames: bool) -> dict:
    if keep_nicknames:
        return r
    return {k: v for k, v in r.items() if k not in NICKNAME_FIELDS}


def usable(r: dict, *, ranked_only: bool) -> tuple[bool, str]:
    """Whether a listing entry is worth capturing.

    Cheap filters on metadata we already have, applied before spending a
    download -- and long before spending three minutes of capture.
    """
    if r.get("mountainVaporBug"):
        # A known desync bug: the replay does not play back faithfully, so the
        # frames would not match the inputs. Exactly the corruption the whole
        # validate.py gate exists to keep out, caught for free here.
        return False, "mountainVaporBug"
    if ranked_only and r.get("unranked"):
        return False, "unranked"
    return True, ""


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("-o", "--out", type=Path, required=True,
                    help="corpus directory for .rep files and index.jsonl")
    ap.add_argument("-n", "--count", type=int, default=500,
                    help="how many replays to fetch (default 500)")
    ap.add_argument("--order", default="last",
                    help="listing order (the backend appears to ignore this: "
                         "order=first/last/elo all return identical rows)")
    ap.add_argument("--start-offset", type=int, default=0,
                    help="begin paging here instead of at the newest replay. "
                         "This is how several machines build disjoint corpora "
                         "without talking to each other: give each a range of "
                         "the listing wide enough that they cannot meet. "
                         "Seeding index.jsonl handles exact duplicates within "
                         "one corpus; this avoids them between corpora.")
    ap.add_argument("--delay", type=float, default=DEFAULT_DELAY_S,
                    help=f"seconds between requests (default {DEFAULT_DELAY_S})")
    ap.add_argument("--ranked-only", action="store_true",
                    help="skip unranked matches (they are still real gameplay, "
                         "so they are included by default)")
    ap.add_argument("--keep-nicknames", action="store_true",
                    help="keep player nicknames in index.jsonl (dropped by "
                         "default; the corpus does not need identities)")
    ap.add_argument("--min-elo", type=float, default=0.0,
                    help="skip matches where either player is below this Elo")
    args = ap.parse_args(argv)

    args.out.mkdir(parents=True, exist_ok=True)
    index_path = args.out / "index.jsonl"

    have = {int(p.stem) for p in args.out.glob("*.rep") if p.stem.isdigit()}
    if have:
        print(f"resuming: {len(have)} .rep files already present")

    # Offset paging over a *live* feed revisits rows: new matches are inserted
    # at the top of `order=last` while we page, shifting everything down, so
    # the same replay comes back at a later offset. A first run produced 12
    # index entries for 11 files that way. Track ids explicitly rather than
    # trusting the offset to mean anything stable.
    seen: set[int] = set(have)
    if index_path.exists():
        with index_path.open() as fh:
            for line in fh:
                try:
                    seen.add(int(json.loads(line)["id"]))
                except (ValueError, KeyError, json.JSONDecodeError):
                    continue

    f = Fetcher(delay_s=args.delay)
    index = index_path.open("a")

    fetched = skipped = 0
    total_bytes = 0
    offset = args.start_offset
    t0 = time.monotonic()

    try:
        while fetched + len(have) < args.count:
            page, more = list_page(f, offset, args.order)
            if not page:
                print("listing exhausted")
                break
            offset += len(page)

            for r in page:
                if fetched + len(have) >= args.count:
                    break
                rid = r["id"]
                if rid in seen:
                    continue
                seen.add(rid)

                ok, why = usable(r, ranked_only=args.ranked_only)
                if ok and args.min_elo:
                    lo = min(r.get("serverUserElo") or 0, r.get("clientUserElo") or 0)
                    if lo < args.min_elo:
                        ok, why = False, f"elo {lo:.0f} < {args.min_elo:.0f}"
                if not ok:
                    skipped += 1
                    continue

                dest = args.out / f"{rid}.rep"
                try:
                    n = download_one(f, rid, dest)
                except urllib.error.HTTPError as e:
                    print(f"  {rid}: HTTP {e.code}, skipping", file=sys.stderr)
                    skipped += 1
                    continue

                total_bytes += n
                fetched += 1
                index.write(json.dumps(
                    strip_identity(r, keep_nicknames=args.keep_nicknames),
                    ensure_ascii=False) + "\n")
                index.flush()

                if fetched % 50 == 0:
                    rate = fetched / max(time.monotonic() - t0, 1e-9)
                    print(f"  {fetched + len(have)}/{args.count} "
                          f"({total_bytes / 1e6:.0f} MB, {rate:.1f}/s, "
                          f"{skipped} skipped)", flush=True)

            if not more:
                print("reached the end of the listing")
                break
    except KeyboardInterrupt:
        print("\ninterrupted; the corpus is resumable — rerun the same command")
    finally:
        index.close()

    on_disk = sorted(args.out.glob("*.rep"))
    print(f"\ncorpus: {len(on_disk)} replays, "
          f"{sum(p.stat().st_size for p in on_disk) / 1e6:.0f} MB")
    print(f"metadata: {index_path}")
    print(f"this run: +{fetched} fetched, {skipped} skipped by filters")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
