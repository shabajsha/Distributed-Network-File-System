#ifndef SS_FILE_OPS_H
#define SS_FILE_OPS_H

#include "ss_common.h"

// File operations. Filenames must already pass dnfs_valid_filename().
void ensure_directories(void);
void build_filepath(char *dest, size_t size, const char *filename);
void build_snapshot_path(char *dest, size_t size, const char *filename);
int file_exists(const char *path);
char *load_file(const char *filename);
char *load_snapshot(const char *filename);
int save_file_atomic(const char *filename, const char *content);
int save_snapshot(const char *filename, const char *content);
void remove_snapshot(const char *filename);
char *build_files_manifest(void);

#endif // SS_FILE_OPS_H
