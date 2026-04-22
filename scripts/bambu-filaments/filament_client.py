import sqlite3

class FilamentClient:
    def __init__(self, db_path="filaments.db"):
        self.db_path = db_path
        # We don't keep the connection open to save RAM on embedded
    
    def _get_conn(self):
        return sqlite3.connect(self.db_path)

    def get_filament_properties(self, filament_name):
        """
        Recursively resolves all properties for a filament.
        Returns a flat dictionary of {key: [values]}.
        """
        resolved = {}
        visited = set()
        
        conn = self._get_conn()
        try:
            self._resolve_recursive(conn, filament_name, resolved, visited)
        finally:
            conn.close()
            
        return resolved

    def _resolve_recursive(self, conn, name, resolved, visited):
        if name in visited: return
        visited.add(name)

        cursor = conn.cursor()
        
        # 1. Get Base Info & Inheritance
        cursor.execute("SELECT id, inherits_name FROM filaments WHERE name = ?", (name,))
        row = cursor.fetchone()
        if not row: return
        
        f_id, inherits = row

        # 2. Recurse into Parent first (if exists)
        if inherits:
            self._resolve_recursive(conn, inherits, resolved, visited)

        # 3. Recurse into Includes (in sequence)
        cursor.execute("SELECT include_name FROM filament_includes WHERE filament_id = ? ORDER BY sequence", (f_id,))
        includes = [r[0] for r in cursor.fetchall()]
        for inc_name in includes:
            self._resolve_recursive(conn, inc_name, resolved, visited)

        # 4. Apply Local Overrides
        # We join with normalization tables to get actual strings
        query = """
            SELECT k.key, v.value, p.value_index
            FROM filament_properties p
            JOIN property_keys k ON p.key_id = k.id
            JOIN property_values v ON p.value_id = v.id
            WHERE p.filament_id = ?
            ORDER BY k.key, p.value_index
        """
        cursor.execute(query, (f_id,))
        
        current_key = None
        temp_list = []
        
        for key, value, idx in cursor.fetchall():
            if key != current_key:
                if current_key:
                    resolved[current_key] = temp_list
                current_key = key
                temp_list = []
            temp_list.append(value)
            
        if current_key:
            resolved[current_key] = temp_list

    def list_filaments(self):
        """Returns a list of all instantiable filament names."""
        conn = self._get_conn()
        cursor = conn.cursor()
        cursor.execute("SELECT name FROM filaments WHERE instantiation = 'true' ORDER BY name")
        names = [r[0] for r in cursor.fetchall()]
        conn.close()
        return names
