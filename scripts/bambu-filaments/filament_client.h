#ifndef FILAMENT_CLIENT_H
#define FILAMENT_CLIENT_H

#include <sqlite3.h>

typedef struct {
    char *key;
    char **values;
    int value_count;
} filament_property_t;

typedef struct {
    filament_property_t *properties;
    int count;
} filament_property_set_t;

// Initialize the client with the path to the database file
sqlite3* filament_db_open(const char *db_path);

// Retrieve all resolved properties for a given filament name
// Returns a property set that must be freed with filament_properties_free
filament_property_set_t* filament_get_all_properties(sqlite3 *db, const char *filament_name);

// Get a specific property directly (more memory efficient)
int filament_get_property(sqlite3 *db, const char *filament_name, const char *key, char ***out_values, int *out_count);

// Helper to free memory
void filament_properties_free(filament_property_set_t *set);
void filament_values_free(char **values, int count);

// Close the database
void filament_db_close(sqlite3 *db);

#endif // FILAMENT_CLIENT_H
