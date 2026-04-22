# Bambu Filament to SQLite

This tool transforms Bambu filament JSON files into a SQLite database, resolving `inherits` and `include` directives recursively.

## Requirements

- [uv](https://github.com/astral-sh/uv)

## Usage

Run the transformation script:

```bash
uv run python main.py
```

This will create `filaments.db` in the current directory.

## Schema

The database contains the following tables:

- `filaments`: Basic info for each filament (name, type, etc.)
- `filament_properties`: Resolved properties (including inherited and included values). Multiple values for a key are stored with a `value_index`.
- `filament_inheritance`: Record of direct inheritance relationships.
- `filament_includes`: Record of included templates.
- `filament_raw_data`: The original JSON content for each file.
