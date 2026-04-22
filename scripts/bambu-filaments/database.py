import sqlite3
import json
from typing import Any, Dict, List

class FilamentDatabase:
    def __init__(self, db_path: str):
        self.conn = sqlite3.connect(db_path)
        self.create_tables()
        self._key_cache = {}
        self._val_cache = {}

    def create_tables(self):
        cursor = self.conn.cursor()
        
        # Main filaments table
        cursor.execute("""
            CREATE TABLE IF NOT EXISTS filaments (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT UNIQUE NOT NULL,
                type TEXT,
                inherits_name TEXT,
                instantiation TEXT,
                setting_id TEXT,
                filament_id TEXT,
                from_path TEXT
            )
        """)

        # String normalization tables
        cursor.execute("CREATE TABLE IF NOT EXISTS property_keys (id INTEGER PRIMARY KEY, key TEXT UNIQUE)")
        cursor.execute("CREATE TABLE IF NOT EXISTS property_values (id INTEGER PRIMARY KEY, value TEXT UNIQUE)")

        # Delta properties (only overrides)
        cursor.execute("""
            CREATE TABLE IF NOT EXISTS filament_properties (
                filament_id INTEGER,
                key_id INTEGER,
                value_id INTEGER,
                value_index INTEGER,
                FOREIGN KEY (filament_id) REFERENCES filaments(id),
                FOREIGN KEY (key_id) REFERENCES property_keys(id),
                FOREIGN KEY (value_id) REFERENCES property_values(id)
            )
        """)

        # Inheritance relation (for reconstruction)
        cursor.execute("""
            CREATE TABLE IF NOT EXISTS filament_inheritance (
                child_id INTEGER,
                parent_name TEXT,
                FOREIGN KEY (child_id) REFERENCES filaments(id)
            )
        """)

        # Includes relation
        cursor.execute("""
            CREATE TABLE IF NOT EXISTS filament_includes (
                filament_id INTEGER,
                include_name TEXT,
                sequence INTEGER,
                FOREIGN KEY (filament_id) REFERENCES filaments(id)
            )
        """)

        cursor.execute("CREATE INDEX IF NOT EXISTS idx_prop_filament ON filament_properties(filament_id)")
        self.conn.commit()

    def _get_id(self, table: str, column: str, value: str, cache: dict) -> int:
        if value in cache:
            return cache[value]
        cursor = self.conn.cursor()
        cursor.execute(f"SELECT id FROM {table} WHERE {column} = ?", (value,))
        result = cursor.fetchone()
        if result:
            cache[value] = result[0]
            return result[0]
        cursor.execute(f"INSERT INTO {table} ({column}) VALUES (?)", (value,))
        new_id = cursor.lastrowid
        cache[value] = new_id
        return new_id

    def insert_filament(self, name: str, raw_data: Dict[str, Any], delta_data: Dict[str, Any], path: str):
        cursor = self.conn.cursor()
        
        cursor.execute("""
            INSERT OR REPLACE INTO filaments (name, type, inherits_name, instantiation, setting_id, filament_id, from_path)
            VALUES (?, ?, ?, ?, ?, ?, ?)
        """, (
            name,
            raw_data.get("type"),
            raw_data.get("inherits"),
            raw_data.get("instantiation"),
            raw_data.get("setting_id"),
            raw_data.get("filament_id"),
            path
        ))
        filament_id = cursor.lastrowid

        # Insert inheritance
        if raw_data.get("inherits"):
            cursor.execute("INSERT INTO filament_inheritance (child_id, parent_name) VALUES (?, ?)",
                           (filament_id, raw_data["inherits"]))

        # Insert includes
        for idx, inc in enumerate(raw_data.get("include", [])):
            cursor.execute("INSERT INTO filament_includes (filament_id, include_name, sequence) VALUES (?, ?, ?)",
                           (filament_id, inc, idx))

        # Insert delta properties
        prop_entries = []
        for key, value in delta_data.items():
            key_id = self._get_id("property_keys", "key", key, self._key_cache)
            if isinstance(value, list):
                for idx, item in enumerate(value):
                    val_id = self._get_id("property_values", "value", str(item), self._val_cache)
                    prop_entries.append((filament_id, key_id, val_id, idx))
            else:
                val_id = self._get_id("property_values", "value", str(value), self._val_cache)
                prop_entries.append((filament_id, key_id, val_id, 0))

        cursor.executemany("INSERT INTO filament_properties (filament_id, key_id, value_id, value_index) VALUES (?, ?, ?, ?)", 
                           prop_entries)
        self.conn.commit()

    def dedupe_variants(self):
        """
        Collapse the ~1,600 printer/nozzle variants of the same filament down
        to the ~98 `@base` canonical presets. Bambu ships each filament in 30+
        combos ("Bambu PLA Basic @BBL X1C 0.6 nozzle", etc.) — all with
        identical nozzle_temp / material metadata for the picker's purposes.
        The `@base` preset is the common parent and carries the `filament_id`
        column; promoting it to instantiation='true' and dropping the
        variants gets rid of 94% of the rows while preserving every field
        the SpoolEase picker cares about.

        Ancestor templates (e.g. `fdm_filament_pla`, `fdm_filament_common`)
        are kept intact — the surviving `@base` rows still inherit temps and
        other resolved properties through them.

        Call this before finalize().
        """
        cur = self.conn.cursor()

        # 1. Promote @base entries that carry a filament_id to canonical.
        cur.execute("""
            UPDATE filaments
               SET instantiation = 'true'
             WHERE name LIKE '%@base'
               AND filament_id IS NOT NULL
        """)
        promoted = cur.rowcount

        # 2. Collect the variant rows we're about to drop. These are
        #    instantiable presets that aren't an @base — pure child overrides
        #    on top of their @base parent, which we no longer need.
        cur.execute("""
            SELECT id FROM filaments
             WHERE instantiation = 'true'
               AND name NOT LIKE '%@base'
        """)
        victim_ids = [r[0] for r in cur.fetchall()]

        if victim_ids:
            placeholders = ",".join("?" * len(victim_ids))
            cur.execute(f"DELETE FROM filament_properties  WHERE filament_id IN ({placeholders})", victim_ids)
            cur.execute(f"DELETE FROM filament_includes    WHERE filament_id IN ({placeholders})", victim_ids)
            cur.execute(f"DELETE FROM filament_inheritance WHERE child_id    IN ({placeholders})", victim_ids)
            cur.execute(f"DELETE FROM filaments            WHERE id          IN ({placeholders})", victim_ids)

        # 3. Garbage-collect normalization rows that are now unreferenced.
        cur.execute("""
            DELETE FROM property_keys
             WHERE id NOT IN (SELECT DISTINCT key_id FROM filament_properties)
        """)
        cur.execute("""
            DELETE FROM property_values
             WHERE id NOT IN (SELECT DISTINCT value_id FROM filament_properties)
        """)

        self.conn.commit()
        print(f"Dedup: promoted {promoted} @base entries, dropped {len(victim_ids)} variants.")

    def finalize(self):
        print("Vacuuming database...")
        self.conn.execute("VACUUM")
        self.conn.close()
