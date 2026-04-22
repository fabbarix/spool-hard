import json
import os
from typing import Any, Dict, List, Optional, Set

class FilamentParser:
    def __init__(self, root_dir: str):
        self.root_dir = root_dir
        self.registry: Dict[str, Dict[str, Any]] = {}
        self.name_to_path: Dict[str, str] = {}

    def load_all(self):
        for root, dirs, files in os.walk(self.root_dir):
            for file in files:
                if file.endswith(".json"):
                    path = os.path.join(root, file)
                    try:
                        with open(path, 'r') as f:
                            data = json.load(f)
                            if isinstance(data, dict) and "name" in data:
                                name = data["name"]
                                self.registry[name] = data
                                self.name_to_path[name] = path
                    except Exception as e:
                        print(f"Error loading {path}: {e}")

    def resolve_filament(self, name: str, visited: Optional[Set[str]] = None) -> Dict[str, Any]:
        """Returns the fully resolved properties for a filament."""
        if visited is None:
            visited = set()
        
        if name in visited:
            raise ValueError(f"Circular dependency detected for {name}")
        visited.add(name)

        if name not in self.registry:
            return {}

        raw_data = self.registry[name]
        resolved = {}

        # 1. Start with Parent (inherits)
        parent_name = raw_data.get("inherits")
        if parent_name:
            resolved.update(self.resolve_filament(parent_name, visited.copy()))

        # 2. Apply Includes
        includes = raw_data.get("include", [])
        for inc_name in includes:
            resolved.update(self.resolve_filament(inc_name, visited.copy()))

        # 3. Apply Own Fields
        resolved.update(raw_data)
        
        return resolved

    def get_delta_properties(self, name: str) -> Dict[str, Any]:
        """Returns only the properties that are explicitly defined in this file (deltas)."""
        if name not in self.registry:
            return {}
        
        raw_data = self.registry[name].copy()
        # Remove structural keys
        for key in ["name", "inherits", "include", "type", "from", "instantiation", "setting_id", "filament_id"]:
            raw_data.pop(key, None)
        return raw_data

    def get_all_filament_names(self) -> List[str]:
        return [name for name, data in self.registry.items() if data.get("type") == "filament"]
