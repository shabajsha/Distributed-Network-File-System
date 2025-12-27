#ifndef SS_FILE_OPS_H
#define SS_FILE_OPS_H

#include "ss_common.h"

// File operations
void ensure_directories(void);
void build_filepath(char *dest, const char *filename);
void build_snapshot_path(char *dest, const char *filename);
int file_exists(const char *path);
char *load_file(const char *filename);
void save_file(const char *filename, const char *content);
void save_snapshot(const char *filename, const char *content);
int save_file_atomic(const char *filename, const char *content);
char *build_files_manifest(void);

#endif // SS_FILE_OPS_H
