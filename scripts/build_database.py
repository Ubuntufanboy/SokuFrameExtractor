#!/usr/bin/env python3
"""
build_database.py — Soku Frame Extractor Database Builder
==========================================================

Ingests the output produced by the SokuFrameExtractor DLL
(BMP frame images + inputs.csv files) into a single SQLite
database suitable for AI training.

Output Structure from the DLL:
    <output_dir>/
        <replay_name>/
            frames/
                000000.bmp
                000001.bmp
                ...
            inputs.csv

This script reads each replay's inputs.csv and registers
the frame file paths in the database. The BMP files remain
on disk — the database stores paths to them, not the pixel
data itself (which would make the DB impossibly large).

Usage:
    python build_database.py <extract_dir> [--db soku_training.db]

Input Bitmask Format (per player, per frame, uint16):
    Bit 0 (0x001): Up          Bit 5 (0x020): B (Weak Bullet)
    Bit 1 (0x002): Down        Bit 6 (0x040): C (Strong Bullet)
    Bit 2 (0x004): Left        Bit 7 (0x080): D (Dash)
    Bit 3 (0x008): Right       Bit 8 (0x100): Change Card (A+B)
    Bit 4 (0x010): A (Melee)   Bit 9 (0x200): Spell / Use Card (B+C)
"""

import argparse
import csv
import logging
import os
import sqlite3
import sys
from pathlib import Path

logger = logging.getLogger("soku_db_builder")

# ---------------------------------------------------------------------------
# Database Schema
# ---------------------------------------------------------------------------

SCHEMA_SQL = """
-- Schema version tracking
CREATE TABLE IF NOT EXISTS schema_info (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
INSERT OR REPLACE INTO schema_info (key, value) VALUES ('version', '2');

-- Replay metadata
CREATE TABLE IF NOT EXISTS replays (
    replay_id    INTEGER PRIMARY KEY AUTOINCREMENT,
    replay_name  TEXT    NOT NULL UNIQUE,
    replay_dir   TEXT    NOT NULL,
    total_frames INTEGER NOT NULL,
    created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Per-frame data: input bitmasks + path to the visual frame image
CREATE TABLE IF NOT EXISTS frames (
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
    frame_file  TEXT    NOT NULL,
    FOREIGN KEY (replay_id) REFERENCES replays(replay_id) ON DELETE CASCADE,
    UNIQUE (replay_id, frame_index)
);

CREATE INDEX IF NOT EXISTS idx_frames_replay
    ON frames (replay_id, frame_index);

-- Self-documenting: input format reference stored in the DB
CREATE TABLE IF NOT EXISTS input_format (
    bit_index   INTEGER PRIMARY KEY,
    hex_mask    TEXT    NOT NULL,
    button_name TEXT    NOT NULL,
    description TEXT    NOT NULL
);

INSERT OR REPLACE INTO input_format VALUES (0, '0x001', 'Up',     'Directional Up');
INSERT OR REPLACE INTO input_format VALUES (1, '0x002', 'Down',   'Directional Down');
INSERT OR REPLACE INTO input_format VALUES (2, '0x004', 'Left',   'Directional Left');
INSERT OR REPLACE INTO input_format VALUES (3, '0x008', 'Right',  'Directional Right');
INSERT OR REPLACE INTO input_format VALUES (4, '0x010', 'A',      'Melee attack');
INSERT OR REPLACE INTO input_format VALUES (5, '0x020', 'B',      'Weak bullet / projectile');
INSERT OR REPLACE INTO input_format VALUES (6, '0x040', 'C',      'Strong bullet / projectile');
INSERT OR REPLACE INTO input_format VALUES (7, '0x080', 'D',      'Dash');
INSERT OR REPLACE INTO input_format VALUES (8, '0x100', 'Change', 'Switch card (A+B macro)');
INSERT OR REPLACE INTO input_format VALUES (9, '0x200', 'Spell',  'Use card (B+C macro)');
"""


def create_database(db_path: str) -> sqlite3.Connection:
    """Create or open the database and initialize the schema."""
    conn = sqlite3.connect(db_path)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA synchronous=NORMAL")
    conn.execute("PRAGMA foreign_keys=ON")
    conn.executescript(SCHEMA_SQL)
    conn.commit()
    return conn


def ingest_replay(
    conn: sqlite3.Connection,
    replay_name: str,
    replay_dir: str,
    csv_path: str,
) -> int:
    """
    Read a single replay's inputs.csv and insert all frames into the DB.
    Returns the number of frames inserted.
    """
    cursor = conn.cursor()

    # Read the CSV
    rows = []
    with open(csv_path, "r", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(row)

    if not rows:
        logger.warning("  %s: inputs.csv is empty, skipping", replay_name)
        return 0

    # Verify frame images exist for a sample of rows
    frames_dir = os.path.join(replay_dir, "frames")
    sample_file = os.path.join(frames_dir, rows[0].get("frame_file", ""))
    if not os.path.exists(sample_file):
        logger.warning("  %s: frame file not found: %s", replay_name, sample_file)

    # Insert replay metadata
    cursor.execute("""
        INSERT OR REPLACE INTO replays (replay_name, replay_dir, total_frames)
        VALUES (?, ?, ?)
    """, (replay_name, replay_dir, len(rows)))
    replay_id = cursor.lastrowid

    # Batch-insert frame data
    frame_rows = []
    for row in rows:
        # Build absolute path to frame image
        frame_file = os.path.join(frames_dir, row["frame_file"])

        frame_rows.append((
            replay_id,
            int(row["frame_index"]),
            int(row["p1_input"]),
            int(row["p2_input"]),
            int(row["p1_up"]),
            int(row["p1_down"]),
            int(row["p1_left"]),
            int(row["p1_right"]),
            int(row["p1_a"]),
            int(row["p1_b"]),
            int(row["p1_c"]),
            int(row["p1_d"]),
            int(row["p1_change"]),
            int(row["p1_spell"]),
            int(row["p2_up"]),
            int(row["p2_down"]),
            int(row["p2_left"]),
            int(row["p2_right"]),
            int(row["p2_a"]),
            int(row["p2_b"]),
            int(row["p2_c"]),
            int(row["p2_d"]),
            int(row["p2_change"]),
            int(row["p2_spell"]),
            frame_file,
        ))

    cursor.executemany("""
        INSERT OR REPLACE INTO frames (
            replay_id, frame_index, p1_input, p2_input,
            p1_up, p1_down, p1_left, p1_right,
            p1_a, p1_b, p1_c, p1_d, p1_change, p1_spell,
            p2_up, p2_down, p2_left, p2_right,
            p2_a, p2_b, p2_c, p2_d, p2_change, p2_spell,
            frame_file
        ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
    """, frame_rows)

    conn.commit()
    return len(frame_rows)


def process_extract_dir(extract_dir: str, db_path: str) -> dict:
    """
    Walk the extraction output directory and ingest all replays.
    """
    conn = create_database(db_path)
    stats = {"total": 0, "success": 0, "frames": 0, "skipped": 0}

    for entry in sorted(os.listdir(extract_dir)):
        replay_dir = os.path.join(extract_dir, entry)
        if not os.path.isdir(replay_dir):
            continue

        csv_path = os.path.join(replay_dir, "inputs.csv")
        if not os.path.exists(csv_path):
            continue

        stats["total"] += 1
        replay_name = entry

        # Check if already ingested
        cursor = conn.execute(
            "SELECT 1 FROM replays WHERE replay_name = ?", (replay_name,)
        )
        if cursor.fetchone():
            logger.info("  %s: already in database, skipping", replay_name)
            stats["skipped"] += 1
            continue

        try:
            n_frames = ingest_replay(conn, replay_name, replay_dir, csv_path)
            logger.info("  %s: ingested %d frames", replay_name, n_frames)
            stats["success"] += 1
            stats["frames"] += n_frames
        except Exception as e:
            logger.error("  %s: ERROR — %s", replay_name, e)

    conn.close()
    return stats


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Build an SQLite database from SokuFrameExtractor output. "
            "Pairs visual frame images with per-frame input bitmasks."
        ),
        epilog="""
Example queries:
  -- Load a training batch: frame image path + P1 inputs
  SELECT frame_file, p1_input FROM frames
  WHERE replay_id = 1 ORDER BY frame_index LIMIT 100;

  -- Get frames where P1 pressed A (melee)
  SELECT frame_file, frame_index FROM frames
  WHERE replay_id = 1 AND p1_a = 1;

  -- Count total frames across all replays
  SELECT SUM(total_frames) FROM replays;
        """,
    )
    parser.add_argument(
        "extract_dir",
        help="Root directory of SokuFrameExtractor output",
    )
    parser.add_argument(
        "--db", default="soku_training.db",
        help="Output SQLite database path (default: soku_training.db)",
    )
    parser.add_argument(
        "--verbose", "-v", action="store_true",
        help="Verbose logging",
    )

    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%H:%M:%S",
    )

    if not os.path.isdir(args.extract_dir):
        logger.error("Directory not found: %s", args.extract_dir)
        sys.exit(1)

    logger.info("Extract directory : %s", args.extract_dir)
    logger.info("Database output   : %s", args.db)
    logger.info("=" * 60)

    stats = process_extract_dir(args.extract_dir, args.db)

    logger.info("=" * 60)
    logger.info("Ingestion complete:")
    logger.info("  Replay dirs found : %d", stats["total"])
    logger.info("  Successfully added: %d", stats["success"])
    logger.info("  Skipped (exists)  : %d", stats["skipped"])
    logger.info("  Total frames      : %d", stats["frames"])
    logger.info("Database: %s", args.db)


if __name__ == "__main__":
    main()