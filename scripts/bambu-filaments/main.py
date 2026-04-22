from parser import FilamentParser
from database import FilamentDatabase
import os

def main():
    db_path = "filaments.db"
    if os.path.exists(db_path):
        os.remove(db_path)
    
    parser = FilamentParser(".")
    print("Loading all JSON files...")
    parser.load_all()
    
    db = FilamentDatabase(db_path)
    
    # We want to store EVERYTHING in the registry as a delta, 
    # not just the final filaments, so we can reconstruct chains.
    all_names = list(parser.registry.keys())
    print(f"Found {len(all_names)} entities. Inserting as deltas...")
    
    for name in all_names:
        try:
            raw_data = parser.registry[name]
            delta_data = parser.get_delta_properties(name)
            path = parser.name_to_path[name]
            db.insert_filament(name, raw_data, delta_data, path)
        except Exception as e:
            print(f"Error processing {name}: {e}")

    # Collapse printer/nozzle variants (Bambu ships ~1600 rows that share
    # ~98 filament_ids) down to one canonical @base row per filament.
    # Inheritance resolution still works — the shared ancestor templates
    # (fdm_filament_*) are left untouched.
    db.dedupe_variants()

    db.finalize()
    print(f"Done! Database created at {db_path}")

if __name__ == "__main__":
    main()
