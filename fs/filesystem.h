#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char* name;
    const uint8_t* data;
    size_t size;
} FileEntry;

typedef struct {
    const FileEntry* entry;
    size_t offset;
} FileHandle;

void fs_init();
FileHandle* fs_open(const char* filename);
size_t fs_read(FileHandle* fh, uint8_t* buffer, size_t bytes);
void fs_close(FileHandle* fh);
int fs_list();
int fs_change_dir(const char* path);
int fs_cd_up();

#endif
