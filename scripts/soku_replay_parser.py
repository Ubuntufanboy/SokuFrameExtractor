#!/usr/bin/env python3
"""
Soku Replay Parser - Touhou 12.3 Hisoutensoku (.rep) Replay File Parser
========================================================================

Parses .rep replay files from Touhou 12.3: Hisoutensoku (Soku) and stores
the extracted metadata and per-frame input data into an SQLite database.
Designed for AI training dataset preparation.

Replay File Format (reverse-engineered from SokuDev/SokuMods):
    - Uncompressed header: game metadata, character IDs, deck cards, stage, etc.
    - Deflate-compressed body: per-frame input bitmasks for both players.

Input Bitmask Format (per player, per frame, uint16):
    Bit 0  (0x001):  Up
    Bit 1  (0x002):  Down
    Bit 2  (0x004):  Left
    Bit 3  (0x008):  Right
    Bit 4  (0x010):  A  (Melee)
    Bit 5  (0x020):  B  (Weak Bullet)
    Bit 6  (0x040):  C  (Strong Bullet)
    Bit 7  (0x080):  D  (Dash)
    Bit 8  (0x100):  Change Card (A+B macro)
    Bit 9  (0x200):  Spell / Use Card (B+C macro)

    Directional bits (0-3) encode numpad notation:
        5=neutral, 1=DL, 2=D, 3=DR, 4=L, 6=R, 7=UL, 8=U, 9=UR

Usage:
    python soku_replay_parser.py <replay_directory> [--db <output.db>] [--verbose]

Author: AI Training Pipeline
License: MIT
"""

import argparse
import hashlib
import logging
import os
import sqlite3
import struct
import sys
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

CHARACTER_NAMES = {
    0: "Reimu",
    1: "Marisa",
    2: "Sakuya",
    3: "Alice",
    4: "Patchouli",
    5: "Youmu",
    6: "Remilia",
    7: "Yuyuko",
    8: "Yukari",
    9: "Suika",
    10: "Reisen",
    11: "Aya",
    12: "Komachi",
    13: "Iku",
    14: "Tenshi",
    15: "Sanae",
    16: "Cirno",
    17: "Meiling",
    18: "Utsuho",
    19: "Suwako",
}

STAGE_NAMES = {
    0: "Hakurei Shrine",
    1: "Forest of Magic",
    2: "Scarlet Devil Mansion Clock Tower",
    3: "SDM Library",
    4: "Netherworld",
    5: "Hakugyokurou Snowy Garden",
    6: "Bamboo Forest of the Lost",
    7: "Bhava-Agra",
    8: "Youkai Mountain",
    9: "Heaven",
    10: "SDM Foyer",
    11: "Spring Path (Hakurei)",
    12: "Scarlet Devil Mansion Rooftop",
    13: "Moriya Shrine",
    14: "Mouth of Geyser",
    15: "Cauldera of Spirits",
    16: "Nuclear Reactor Core",
    17: "SDM Basement",
    18: "Fusion Reactor",
    19: "Generic Stage",
}

# Input bitmask constants (matches ReplayInputView+ CBattleManager_RefleshCommandInfo)
INPUT_UP         = 0x001
INPUT_DOWN       = 0x002
INPUT_LEFT       = 0x004
INPUT_RIGHT      = 0x008
INPUT_A          = 0x010  # Melee
INPUT_B          = 0x020  # Weak Bullet
INPUT_C          = 0x040  # Strong Bullet
INPUT_D          = 0x080  # Dash
INPUT_CHANGE     = 0x100  # Change/Switch Card (A+B)
INPUT_SPELL      = 0x200  # Use Card (B+C)

INPUT_DIRECTION_MASK = 0x00F
INPUT_BUTTON_MASK    = 0x3F0

# Minimum valid replay file size (header alone is ~100+ bytes)
MIN_REPLAY_SIZE = 100

# Maximum sane frame count to prevent runaway parsing on corrupt files
MAX_FRAME_COUNT = 600_000  # ~2.7 hours at 60fps, way beyond any real match

logger = logging.getLogger("soku_parser")


# ---------------------------------------------------------------------------
# Data Structures
# ---------------------------------------------------------------------------

@dataclass
class PlayerInfo:
    """Metadata for a single player extracted from the replay header."""
    character_id: int = -1
    character_name: str = "Unknown"
    palette: int = 0
    deck_id: int = 0
    card_count: int = 0
    deck_cards: list = field(default_factory=list)


@dataclass
class ReplayMetadata:
    """All metadata extracted from a replay file header."""
    file_path: str = ""
    file_hash: str = ""
    file_size: int = 0
    game_version: int = 0
    p1: PlayerInfo = field(default_factory=PlayerInfo)
    p2: PlayerInfo = field(default_factory=PlayerInfo)
    stage_id: int = -1
    stage_name: str = "Unknown"
    music_id: int = -1
    seed: int = 0
    total_frames: int = 0
    header_size: int = 0
    parse_errors: list = field(default_factory=list)


@dataclass
class FrameInput:
    """Input state for a single frame."""
    frame_index: int
    p1_input: int  # Raw bitmask
    p2_input: int  # Raw bitmask


# ---------------------------------------------------------------------------
# Replay Parser
# ---------------------------------------------------------------------------

class SokuReplayParser:
    """
    Parses Touhou 12.3 Hisoutensoku .rep replay files.

    The replay file format (Hisoutensoku v1.10a):
        [Uncompressed Header]  Variable length (~0x70-0x80 bytes)
        [Deflate Compressed]   Frame-by-frame input data

    Header layout (approximate, based on community reverse-engineering):
        0x00  (4 bytes)  : Version / signature dword
        0x04  (2 bytes)  : Unknown
        0x06  (1 byte)   : Unknown flags
        0x07  (1 byte)   : Game mode / version sub-byte
        0x08  (1 byte)   : P1 character ID
        0x09  (1 byte)   : P1 palette
        0x0A  (2 bytes)  : P1 deck profile / unknown
        0x0C  (4 bytes)  : Unknown
        0x10  (4 bytes)  : P1 card count (uint32 LE, typically 20)
        0x14  (N*2 bytes): P1 deck cards (N uint16 LE values)
        ...then P2 data in same structure...
        ...then stage, music, seed, padding...
        ...then deflate compressed input data begins
    """

    def __init__(self, file_path: str):
        self.file_path = file_path
        self.raw_data: bytes = b""
        self.metadata = ReplayMetadata(file_path=file_path)
        self.frames: list[FrameInput] = []

    def parse(self) -> bool:
        """
        Parse the replay file. Returns True on success, False on failure.
        Partial results may still be available in self.metadata / self.frames
        even if this returns False (best-effort parsing).
        """
        try:
            with open(self.file_path, "rb") as f:
                self.raw_data = f.read()
        except (IOError, OSError) as e:
            self.metadata.parse_errors.append(f"File read error: {e}")
            return False

        if len(self.raw_data) < MIN_REPLAY_SIZE:
            self.metadata.parse_errors.append(
                f"File too small ({len(self.raw_data)} bytes)"
            )
            return False

        self.metadata.file_size = len(self.raw_data)
        self.metadata.file_hash = hashlib.sha256(self.raw_data).hexdigest()

        header_ok = self._parse_header()
        input_ok = self._parse_inputs()

        return header_ok and input_ok

    # -----------------------------------------------------------------------
    # Header Parsing
    # -----------------------------------------------------------------------

    def _parse_header(self) -> bool:
        """Parse the uncompressed header section."""
        data = self.raw_data
        try:
            # Game version / signature (first 4 bytes, uint32 LE)
            self.metadata.game_version = struct.unpack_from("<I", data, 0)[0]

            # Player 1 character ID at offset 0x08
            self.metadata.p1.character_id = data[0x08]
            self.metadata.p1.character_name = CHARACTER_NAMES.get(
                self.metadata.p1.character_id, f"Unknown({self.metadata.p1.character_id})"
            )
            self.metadata.p1.palette = data[0x09]

            # P1 card count at offset 0x10 (uint32 LE)
            p1_card_count = struct.unpack_from("<I", data, 0x10)[0]
            if p1_card_count > 20:
                # Sanity check: decks can't exceed 20 cards
                logger.warning(
                    "%s: P1 card count %d exceeds 20, clamping",
                    self.file_path, p1_card_count,
                )
                p1_card_count = min(p1_card_count, 20)
            self.metadata.p1.card_count = p1_card_count

            # P1 deck cards starting at offset 0x14
            p1_deck_start = 0x14
            p1_deck_end = p1_deck_start + p1_card_count * 2
            if p1_deck_end > len(data):
                self.metadata.parse_errors.append("P1 deck extends beyond file")
                return False

            self.metadata.p1.deck_cards = [
                struct.unpack_from("<H", data, p1_deck_start + i * 2)[0]
                for i in range(p1_card_count)
            ]

            # Player 2 data follows P1 deck
            p2_header_start = p1_deck_end
            if p2_header_start + 10 > len(data):
                self.metadata.parse_errors.append("P2 header beyond file bounds")
                return False

            self.metadata.p2.character_id = data[p2_header_start]
            self.metadata.p2.character_name = CHARACTER_NAMES.get(
                self.metadata.p2.character_id,
                f"Unknown({self.metadata.p2.character_id})",
            )
            self.metadata.p2.palette = data[p2_header_start + 1]

            # P2 card count: scan for the uint32 that equals 20 (0x14)
            # The P2 block mirrors P1 structure but the exact offsets
            # depend on intervening unknown fields. We search forward
            # for a plausible card count (0-20) encoded as uint32.
            p2_card_count_offset = self._find_card_count(p2_header_start + 2, max_scan=12)
            if p2_card_count_offset is None:
                # Fallback: assume 5-byte gap like P1 (offset+5)
                p2_card_count_offset = p2_header_start + 5
                logger.debug(
                    "%s: Could not find P2 card count, using fallback offset 0x%X",
                    self.file_path, p2_card_count_offset,
                )

            p2_card_count = struct.unpack_from("<I", data, p2_card_count_offset)[0]
            if p2_card_count > 20:
                p2_card_count = min(p2_card_count, 20)
            self.metadata.p2.card_count = p2_card_count

            p2_deck_start = p2_card_count_offset + 4
            p2_deck_end = p2_deck_start + p2_card_count * 2
            if p2_deck_end > len(data):
                self.metadata.parse_errors.append("P2 deck extends beyond file")
                return False

            self.metadata.p2.deck_cards = [
                struct.unpack_from("<H", data, p2_deck_start + i * 2)[0]
                for i in range(p2_card_count)
            ]

            # Post-deck metadata (stage, music, seed)
            post_deck = p2_deck_end
            if post_deck + 12 <= len(data):
                self.metadata.stage_id = data[post_deck]
                self.metadata.stage_name = STAGE_NAMES.get(
                    self.metadata.stage_id,
                    f"Unknown({self.metadata.stage_id})",
                )
                self.metadata.music_id = data[post_deck + 1]

            self.metadata.header_size = post_deck + 12  # Approximate
            return True

        except (struct.error, IndexError) as e:
            self.metadata.parse_errors.append(f"Header parse error: {e}")
            return False

    def _find_card_count(
        self, start: int, max_scan: int = 16
    ) -> Optional[int]:
        """
        Scan forward from `start` looking for a uint32 LE value
        that looks like a valid card count (1-20). Returns the offset
        of the first match, or None.
        """
        data = self.raw_data
        for offset in range(start, min(start + max_scan, len(data) - 4)):
            val = struct.unpack_from("<I", data, offset)[0]
            if 1 <= val <= 20:
                return offset
        return None

    # -----------------------------------------------------------------------
    # Input Data Parsing
    # -----------------------------------------------------------------------

    def _parse_inputs(self) -> bool:
        """
        Locate and decompress the deflate-compressed input data, then
        parse per-frame input bitmasks for both players.
        """
        decompressed = self._decompress_input_data()
        if decompressed is None:
            return False

        return self._parse_frame_inputs(decompressed)

    def _decompress_input_data(self) -> Optional[bytes]:
        """
        Find and decompress the deflate stream in the replay file.

        The compressed data starts after the header. We try multiple
        strategies:
          1. Look for a zlib header (0x78 xx) and decompress from there
          2. Try raw deflate at progressively later offsets
          3. Try wbits=-15 (raw deflate without zlib header)
        """
        data = self.raw_data

        # Strategy 1: Scan for zlib header bytes (0x78 0x01/0x9C/0xDA/0x5E)
        zlib_second_bytes = {0x01, 0x5E, 0x9C, 0xDA}
        for offset in range(0x40, min(len(data) - 2, 0x200)):
            if data[offset] == 0x78 and data[offset + 1] in zlib_second_bytes:
                try:
                    result = zlib.decompress(data[offset:])
                    logger.debug(
                        "%s: Decompressed %d bytes from zlib at offset 0x%X",
                        self.file_path, len(result), offset,
                    )
                    self.metadata.header_size = offset
                    return result
                except zlib.error:
                    continue

        # Strategy 2: Try raw deflate (wbits=-15) at various offsets
        for offset in range(0x40, min(len(data) - 10, 0x200)):
            try:
                deobj = zlib.decompressobj(wbits=-15)
                result = deobj.decompress(data[offset:])
                result += deobj.flush()
                if len(result) > 4:  # Must yield meaningful data
                    logger.debug(
                        "%s: Decompressed %d bytes (raw deflate) at offset 0x%X",
                        self.file_path, len(result), offset,
                    )
                    self.metadata.header_size = offset
                    return result
            except zlib.error:
                continue

        self.metadata.parse_errors.append("Could not find compressed input data")
        return None

    def _parse_frame_inputs(self, decompressed: bytes) -> bool:
        """
        Parse decompressed data into per-frame input pairs.

        The decompressed stream contains sequential frame data.
        Each frame stores two uint16 LE values: P1 input, P2 input.
        Total: 4 bytes per frame.
        """
        frame_size = 4  # 2 bytes P1 + 2 bytes P2
        total_frames = len(decompressed) // frame_size

        if total_frames == 0:
            self.metadata.parse_errors.append("No frame data in decompressed stream")
            return False

        if total_frames > MAX_FRAME_COUNT:
            self.metadata.parse_errors.append(
                f"Frame count {total_frames} exceeds maximum {MAX_FRAME_COUNT}"
            )
            return False

        self.frames = []
        valid_input_mask = 0x3FF  # Only bits 0-9 are valid inputs

        for i in range(total_frames):
            offset = i * frame_size
            p1_raw, p2_raw = struct.unpack_from("<HH", decompressed, offset)

            # Mask to valid input bits only (bits 0-9)
            p1_input = p1_raw & valid_input_mask
            p2_input = p2_raw & valid_input_mask

            self.frames.append(FrameInput(
                frame_index=i,
                p1_input=p1_input,
                p2_input=p2_input,
            ))

        self.metadata.total_frames = len(self.frames)

        # Validate: check that inputs look reasonable
        # A fully zero replay or all-max replay is suspicious
        nonzero_p1 = sum(1 for f in self.frames if f.p1_input != 0)
        nonzero_p2 = sum(1 for f in self.frames if f.p2_input != 0)
        if nonzero_p1 == 0 and nonzero_p2 == 0 and total_frames > 60:
            self.metadata.parse_errors.append(
                "Warning: all inputs are zero (possible parse error)"
            )

        logger.info(
            "%s: Parsed %d frames (P1 active: %d, P2 active: %d)",
            self.file_path, total_frames, nonzero_p1, nonzero_p2,
        )
        return True


# ---------------------------------------------------------------------------
# Database Manager
# ---------------------------------------------------------------------------

class ReplayDatabase:
    """SQLite database for storing parsed Soku replay data."""

    SCHEMA_VERSION = 1

    def __init__(self, db_path: str):
        self.db_path = db_path
        self.conn: Optional[sqlite3.Connection] = None

    def connect(self):
        """Open (or create) the database and initialize the schema."""
        self.conn = sqlite3.connect(self.db_path)
        self.conn.execute("PRAGMA journal_mode=WAL")
        self.conn.execute("PRAGMA synchronous=NORMAL")
        self.conn.execute("PRAGMA foreign_keys=ON")
        self._create_schema()

    def close(self):
        """Commit and close the database connection."""
        if self.conn:
            self.conn.commit()
            self.conn.close()
            self.conn = None

    def _create_schema(self):
        """Create all tables if they don't already exist."""
        cursor = self.conn.cursor()

        cursor.execute("""
            CREATE TABLE IF NOT EXISTS schema_info (
                key   TEXT PRIMARY KEY,
                value TEXT NOT NULL
            )
        """)

        cursor.execute("""
            INSERT OR REPLACE INTO schema_info (key, value)
            VALUES ('version', ?)
        """, (str(self.SCHEMA_VERSION),))

        # Replay metadata table
        cursor.execute("""
            CREATE TABLE IF NOT EXISTS replays (
                replay_id       INTEGER PRIMARY KEY AUTOINCREMENT,
                file_name       TEXT    NOT NULL,
                file_path       TEXT    NOT NULL UNIQUE,
                file_hash       TEXT    NOT NULL,
                file_size       INTEGER NOT NULL,
                game_version    INTEGER,
                p1_character_id INTEGER,
                p1_character    TEXT,
                p1_palette      INTEGER,
                p1_deck_cards   TEXT,
                p2_character_id INTEGER,
                p2_character    TEXT,
                p2_palette      INTEGER,
                p2_deck_cards   TEXT,
                stage_id        INTEGER,
                stage_name      TEXT,
                music_id        INTEGER,
                total_frames    INTEGER NOT NULL,
                parse_errors    TEXT,
                created_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        """)

        # Per-frame input data table
        # This is the core table for AI training:
        #   - frame_index is the 0-based game tick
        #   - p1_input / p2_input are the raw bitmask integers
        #   - The individual boolean columns are pre-decomposed for convenience
        cursor.execute("""
            CREATE TABLE IF NOT EXISTS frame_inputs (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                replay_id   INTEGER NOT NULL,
                frame_index INTEGER NOT NULL,
                p1_input    INTEGER NOT NULL,
                p2_input    INTEGER NOT NULL,
                p1_up       INTEGER NOT NULL,
                p1_down     INTEGER NOT NULL,
                p1_left     INTEGER NOT NULL,
                p1_right    INTEGER NOT NULL,
                p1_a        INTEGER NOT NULL,
                p1_b        INTEGER NOT NULL,
                p1_c        INTEGER NOT NULL,
                p1_d        INTEGER NOT NULL,
                p1_change   INTEGER NOT NULL,
                p1_spell    INTEGER NOT NULL,
                p2_up       INTEGER NOT NULL,
                p2_down     INTEGER NOT NULL,
                p2_left     INTEGER NOT NULL,
                p2_right    INTEGER NOT NULL,
                p2_a        INTEGER NOT NULL,
                p2_b        INTEGER NOT NULL,
                p2_c        INTEGER NOT NULL,
                p2_d        INTEGER NOT NULL,
                p2_change   INTEGER NOT NULL,
                p2_spell    INTEGER NOT NULL,
                FOREIGN KEY (replay_id) REFERENCES replays(replay_id)
                    ON DELETE CASCADE,
                UNIQUE (replay_id, frame_index)
            )
        """)

        # Index for fast frame lookups within a replay
        cursor.execute("""
            CREATE INDEX IF NOT EXISTS idx_frame_inputs_replay_frame
            ON frame_inputs (replay_id, frame_index)
        """)

        # Index for finding replays by character matchup
        cursor.execute("""
            CREATE INDEX IF NOT EXISTS idx_replays_matchup
            ON replays (p1_character_id, p2_character_id)
        """)

        # Convenience view: input format documentation stored in the DB itself
        cursor.execute("""
            CREATE TABLE IF NOT EXISTS input_format (
                bit_index   INTEGER PRIMARY KEY,
                hex_mask    TEXT    NOT NULL,
                button_name TEXT    NOT NULL,
                description TEXT    NOT NULL
            )
        """)

        format_rows = [
            (0, "0x001", "Up",     "Directional Up"),
            (1, "0x002", "Down",   "Directional Down"),
            (2, "0x004", "Left",   "Directional Left"),
            (3, "0x008", "Right",  "Directional Right"),
            (4, "0x010", "A",      "Melee attack"),
            (5, "0x020", "B",      "Weak bullet / projectile"),
            (6, "0x040", "C",      "Strong bullet / projectile"),
            (7, "0x080", "D",      "Dash"),
            (8, "0x100", "Change", "Switch card (A+B macro)"),
            (9, "0x200", "Spell",  "Use card (B+C macro)"),
        ]
        cursor.executemany(
            "INSERT OR REPLACE INTO input_format VALUES (?, ?, ?, ?)",
            format_rows,
        )

        self.conn.commit()

    def replay_exists(self, file_hash: str) -> bool:
        """Check if a replay with this hash is already in the database."""
        cursor = self.conn.execute(
            "SELECT 1 FROM replays WHERE file_hash = ?", (file_hash,)
        )
        return cursor.fetchone() is not None

    def insert_replay(
        self, metadata: ReplayMetadata, frames: list[FrameInput]
    ) -> int:
        """
        Insert a parsed replay and all its frame data into the database.
        Returns the replay_id of the inserted record.
        """
        cursor = self.conn.cursor()

        # Encode deck cards as comma-separated hex strings
        p1_deck_str = ",".join(f"0x{c:04X}" for c in metadata.p1.deck_cards)
        p2_deck_str = ",".join(f"0x{c:04X}" for c in metadata.p2.deck_cards)
        errors_str = "; ".join(metadata.parse_errors) if metadata.parse_errors else None

        cursor.execute("""
            INSERT INTO replays (
                file_name, file_path, file_hash, file_size, game_version,
                p1_character_id, p1_character, p1_palette, p1_deck_cards,
                p2_character_id, p2_character, p2_palette, p2_deck_cards,
                stage_id, stage_name, music_id, total_frames, parse_errors
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, (
            os.path.basename(metadata.file_path),
            metadata.file_path,
            metadata.file_hash,
            metadata.file_size,
            metadata.game_version,
            metadata.p1.character_id,
            metadata.p1.character_name,
            metadata.p1.palette,
            p1_deck_str,
            metadata.p2.character_id,
            metadata.p2.character_name,
            metadata.p2.palette,
            p2_deck_str,
            metadata.stage_id,
            metadata.stage_name,
            metadata.music_id,
            metadata.total_frames,
            errors_str,
        ))

        replay_id = cursor.lastrowid

        # Batch-insert frame data for performance
        frame_rows = []
        for f in frames:
            frame_rows.append((
                replay_id,
                f.frame_index,
                f.p1_input,
                f.p2_input,
                # P1 decomposed buttons
                int(bool(f.p1_input & INPUT_UP)),
                int(bool(f.p1_input & INPUT_DOWN)),
                int(bool(f.p1_input & INPUT_LEFT)),
                int(bool(f.p1_input & INPUT_RIGHT)),
                int(bool(f.p1_input & INPUT_A)),
                int(bool(f.p1_input & INPUT_B)),
                int(bool(f.p1_input & INPUT_C)),
                int(bool(f.p1_input & INPUT_D)),
                int(bool(f.p1_input & INPUT_CHANGE)),
                int(bool(f.p1_input & INPUT_SPELL)),
                # P2 decomposed buttons
                int(bool(f.p2_input & INPUT_UP)),
                int(bool(f.p2_input & INPUT_DOWN)),
                int(bool(f.p2_input & INPUT_LEFT)),
                int(bool(f.p2_input & INPUT_RIGHT)),
                int(bool(f.p2_input & INPUT_A)),
                int(bool(f.p2_input & INPUT_B)),
                int(bool(f.p2_input & INPUT_C)),
                int(bool(f.p2_input & INPUT_D)),
                int(bool(f.p2_input & INPUT_CHANGE)),
                int(bool(f.p2_input & INPUT_SPELL)),
            ))

        cursor.executemany("""
            INSERT INTO frame_inputs (
                replay_id, frame_index, p1_input, p2_input,
                p1_up, p1_down, p1_left, p1_right,
                p1_a, p1_b, p1_c, p1_d, p1_change, p1_spell,
                p2_up, p2_down, p2_left, p2_right,
                p2_a, p2_b, p2_c, p2_d, p2_change, p2_spell
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, frame_rows)

        self.conn.commit()
        return replay_id


# ---------------------------------------------------------------------------
# Batch Processor
# ---------------------------------------------------------------------------

def find_replay_files(directory: str) -> list[str]:
    """Recursively find all .rep files in a directory."""
    replay_files = []
    for root, _, files in os.walk(directory):
        for fname in sorted(files):
            if fname.lower().endswith(".rep"):
                replay_files.append(os.path.join(root, fname))
    return replay_files


def process_directory(
    replay_dir: str,
    db_path: str,
    verbose: bool = False,
) -> dict:
    """
    Process all .rep files in a directory and store results in the database.

    Returns a summary dict with counts of successes, failures, and skips.
    """
    replay_files = find_replay_files(replay_dir)
    if not replay_files:
        logger.error("No .rep files found in %s", replay_dir)
        return {"total": 0, "success": 0, "failed": 0, "skipped": 0}

    logger.info("Found %d .rep files in %s", len(replay_files), replay_dir)

    db = ReplayDatabase(db_path)
    db.connect()

    stats = {"total": len(replay_files), "success": 0, "failed": 0, "skipped": 0}

    for idx, rep_path in enumerate(replay_files, 1):
        rel_path = os.path.relpath(rep_path, replay_dir)
        progress = f"[{idx}/{stats['total']}]"

        # Quick hash check for deduplication
        try:
            with open(rep_path, "rb") as f:
                file_hash = hashlib.sha256(f.read()).hexdigest()
        except IOError as e:
            logger.error("%s %s: Read error: %s", progress, rel_path, e)
            stats["failed"] += 1
            continue

        if db.replay_exists(file_hash):
            logger.info("%s %s: Already in database, skipping", progress, rel_path)
            stats["skipped"] += 1
            continue

        # Parse the replay
        parser = SokuReplayParser(rep_path)
        success = parser.parse()

        if success and parser.frames:
            replay_id = db.insert_replay(parser.metadata, parser.frames)
            logger.info(
                "%s %s: OK — %s vs %s on %s, %d frames (id=%d)",
                progress, rel_path,
                parser.metadata.p1.character_name,
                parser.metadata.p2.character_name,
                parser.metadata.stage_name,
                parser.metadata.total_frames,
                replay_id,
            )
            stats["success"] += 1
        else:
            errors = "; ".join(parser.metadata.parse_errors) or "Unknown error"
            logger.warning("%s %s: FAILED — %s", progress, rel_path, errors)

            # Still store the metadata (with errors) so we know we attempted it
            if parser.metadata.file_hash:
                try:
                    db.insert_replay(parser.metadata, parser.frames)
                except sqlite3.IntegrityError:
                    pass  # Duplicate path, ignore

            stats["failed"] += 1

    db.close()
    return stats


# ---------------------------------------------------------------------------
# CLI Entry Point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description=(
            "Soku Replay Parser — Extract per-frame input data from "
            "Touhou 12.3 Hisoutensoku .rep replay files into an SQLite database."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Input Bitmask Format (stored as integer per player per frame):
  Bit 0 (0x001): Up          Bit 5 (0x020): B (Weak Bullet)
  Bit 1 (0x002): Down        Bit 6 (0x040): C (Strong Bullet)
  Bit 2 (0x004): Left        Bit 7 (0x080): D (Dash)
  Bit 3 (0x008): Right       Bit 8 (0x100): Change Card (A+B)
  Bit 4 (0x010): A (Melee)   Bit 9 (0x200): Spell / Use Card (B+C)

Example queries after parsing:
  -- Get all frames where P1 pressed A (melee) in replay 1
  SELECT frame_index, p1_input FROM frame_inputs
  WHERE replay_id = 1 AND p1_a = 1;

  -- Count matches per character matchup
  SELECT p1_character, p2_character, COUNT(*) as matches
  FROM replays GROUP BY p1_character_id, p2_character_id;

  -- Get directional input sequence for P1
  SELECT frame_index, p1_input & 0x00F as direction
  FROM frame_inputs WHERE replay_id = 1;
        """,
    )
    parser.add_argument(
        "replay_dir",
        help="Directory containing .rep replay files (searched recursively)",
    )
    parser.add_argument(
        "--db",
        default="soku_replays.db",
        help="Output SQLite database path (default: soku_replays.db)",
    )
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Enable verbose/debug logging",
    )

    args = parser.parse_args()

    # Configure logging
    log_level = logging.DEBUG if args.verbose else logging.INFO
    logging.basicConfig(
        level=log_level,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%H:%M:%S",
    )

    replay_dir = os.path.abspath(args.replay_dir)
    if not os.path.isdir(replay_dir):
        logger.error("Directory not found: %s", replay_dir)
        sys.exit(1)

    db_path = os.path.abspath(args.db)
    logger.info("Replay directory : %s", replay_dir)
    logger.info("Database output  : %s", db_path)
    logger.info("=" * 60)

    stats = process_directory(replay_dir, db_path, args.verbose)

    logger.info("=" * 60)
    logger.info("Processing complete:")
    logger.info("  Total files : %d", stats["total"])
    logger.info("  Successful  : %d", stats["success"])
    logger.info("  Failed      : %d", stats["failed"])
    logger.info("  Skipped     : %d", stats["skipped"])
    logger.info("Database saved to: %s", db_path)

    if stats["failed"] > 0:
        logger.info(
            "Tip: Query failed replays with: "
            "SELECT file_name, parse_errors FROM replays WHERE parse_errors IS NOT NULL"
        )


if __name__ == "__main__":
    main()