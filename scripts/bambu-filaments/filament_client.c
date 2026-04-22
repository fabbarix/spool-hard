#include "filament_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

sqlite3* filament_db_open(const char *db_path) {
    sqlite3 *db;
    int rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

void filament_db_close(sqlite3 *db) {
    if (db) sqlite3_close(db);
}

// Internal structure to handle a dynamic list of strings
typedef struct {
    char **items;
    int count;
    int capacity;
} string_list_t;

static void list_init(string_list_t *l) {
    l->items = NULL;
    l->count = 0;
    l->capacity = 0;
}

static void list_add(string_list_t *l, const char *val) {
    if (l->count >= l->capacity) {
        l->capacity = l->capacity == 0 ? 4 : l->capacity * 2;
        l->items = realloc(l->items, sizeof(char*) * l->capacity);
    }
    l->items[l->count++] = strdup(val);
}

static void list_free(string_list_t *l) {
    for (int i = 0; i < l->count; i++) free(l->items[i]);
    free(l->items);
}

// Simple hash map or linear search for properties to handle overrides in C
// For ESP32, a sorted array + binary search is usually a good RAM/CPU trade-off,
// but for simplicity here we'll use a dynamic array.
typedef struct {
    char *key;
    string_list_t values;
} prop_entry_t;

typedef struct {
    prop_entry_t *entries;
    int count;
    int capacity;
} prop_map_t;

static void prop_map_add(prop_map_t *map, const char *key, string_list_t *new_vals) {
    // Check for override
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->entries[i].key, key) == 0) {
            list_free(&map->entries[i].values);
            map->entries[i].values = *new_vals;
            return;
        }
    }
    // New entry
    if (map->count >= map->capacity) {
        map->capacity = map->capacity == 0 ? 8 : map->capacity * 2;
        map->entries = realloc(map->entries, sizeof(prop_entry_t) * map->capacity);
    }
    map->entries[map->count].key = strdup(key);
    map->entries[map->count].values = *new_vals;
    map->count++;
}

static void resolve_recursive(sqlite3 *db, const char *name, prop_map_t *map) {
    sqlite3_stmt *stmt;
    int f_id = -1;
    char *inherits = NULL;

    // 1. Get ID and Parent
    sqlite3_prepare_v2(db, "SELECT id, inherits_name FROM filaments WHERE name = ?", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        f_id = sqlite3_column_int(stmt, 0);
        const char *inh = (const char*)sqlite3_column_text(stmt, 1);
        if (inh) inherits = strdup(inh);
    }
    sqlite3_finalize(stmt);

    if (f_id == -1) return;

    // 2. Resolve Parent
    if (inherits) {
        resolve_recursive(db, inherits, map);
        free(inherits);
    }

    // 3. Resolve Includes
    sqlite3_prepare_v2(db, "SELECT include_name FROM filament_includes WHERE filament_id = ? ORDER BY sequence", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, f_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *inc = (const char*)sqlite3_column_text(stmt, 0);
        resolve_recursive(db, inc, map);
    }
    sqlite3_finalize(stmt);

    // 4. Load Local Properties
    const char *q = "SELECT k.key, v.value FROM filament_properties p "
                    "JOIN property_keys k ON p.key_id = k.id "
                    "JOIN property_values v ON p.value_id = v.id "
                    "WHERE p.filament_id = ? ORDER BY k.key, p.value_index";
    sqlite3_prepare_v2(db, q, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, f_id);

    char *current_key = NULL;
    string_list_t current_vals;
    list_init(&current_vals);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *key = (const char*)sqlite3_column_text(stmt, 0);
        const char *val = (const char*)sqlite3_column_text(stmt, 1);

        if (current_key && strcmp(key, current_key) != 0) {
            prop_map_add(map, current_key, &current_vals);
            free(current_key);
            list_init(&current_vals);
            current_key = strdup(key);
        } else if (!current_key) {
            current_key = strdup(key);
        }
        list_add(&current_vals, val);
    }
    if (current_key) {
        prop_map_add(map, current_key, &current_vals);
        free(current_key);
    }
    sqlite3_finalize(stmt);
}

filament_property_set_t* filament_get_all_properties(sqlite3 *db, const char *filament_name) {
    prop_map_t map = {0};
    resolve_recursive(db, filament_name, &map);

    if (map.count == 0) return NULL;

    filament_property_set_t *set = malloc(sizeof(filament_property_set_t));
    set->count = map.count;
    set->properties = malloc(sizeof(filament_property_t) * map.count);

    for (int i = 0; i < map.count; i++) {
        set->properties[i].key = map.entries[i].key;
        set->properties[i].values = map.entries[i].values.items;
        set->properties[i].value_count = map.entries[i].values.count;
    }
    free(map.entries);
    return set;
}

void filament_properties_free(filament_property_set_t *set) {
    if (!set) return;
    for (int i = 0; i < set->count; i++) {
        free(set->properties[i].key);
        for (int j = 0; j < set->properties[i].value_count; j++) {
            free(set->properties[i].values[j]);
        }
        free(set->properties[i].values);
    }
    free(set->properties);
    free(set);
}
