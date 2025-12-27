#ifndef NM_CACHE_H
#define NM_CACHE_H

#include "nm_common.h"

void init_cache(void);
FileMetadata *cache_get(const char *filename);
void cache_put(const char *filename, FileMetadata *file);
void cache_remove(const char *filename);

#endif /* NM_CACHE_H */
