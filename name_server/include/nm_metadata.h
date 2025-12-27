#ifndef NM_METADATA_H
#define NM_METADATA_H

#include "nm_common.h"

FileMetadata *lookup_file(const char *filename);
void insert_file(FileMetadata *file);
int check_access(FileMetadata *file, const char *username, const char *required_mode);
void save_metadata(void);
void load_metadata(void);
void parse_json_string(const char *json, const char *key, char *value, int max_len);
int parse_json_int(const char *json, const char *key);
int request_file_stats(const char *ip, int port, const char *filename,
                       int *words, int *chars, int *bytes);

#endif /* NM_METADATA_H */
