#ifndef MIKO_RUNTIME_H
#define MIKO_RUNTIME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MikoFile MikoFile;

/* A file remains a valid handle after close so close is idempotent. Call
 * miko_file_destroy when the handle itself is no longer needed. */
MikoFile* miko_file_open(const char* path, const char* mode);
int miko_file_is_open(const MikoFile* file);
size_t miko_file_read(MikoFile* file, void* buffer, size_t bytes);
size_t miko_file_write(MikoFile* file, const void* buffer, size_t bytes);
int miko_file_close(MikoFile* file);
void miko_file_destroy(MikoFile* file);

#ifdef __cplusplus
}
#endif

#endif
