#ifndef NM_METADATA_H
#define NM_METADATA_H

#include "nm_common.h"

/* All file-table functions require files_mutex to be held. */
FileMetadata *lookup_file(const char *filename);
FileMetadata *find_any_file(const char *filename);
void insert_file(FileMetadata *file);
void mark_file_ready(FileMetadata *file);
void remove_file(const char *filename);
int check_access(FileMetadata *file, const char *username, const char *required_mode);

/* Requires users_mutex. */
int user_index(const char *username);

/* Takes its own locks; never call it while holding files_mutex or users_mutex. */
void save_metadata(void);
void load_metadata(void);

void parse_json_string(const char *json, const char *key, char *value, int max_len);
int parse_json_int(const char *json, const char *key);

#endif /* NM_METADATA_H */
