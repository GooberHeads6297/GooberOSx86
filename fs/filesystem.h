#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <stddef.h>
#include <stdint.h>

#define MAX_NAME_LEN 32

typedef struct Directory Directory;

typedef struct {
    char name[MAX_NAME_LEN];
    uint8_t* data;
    size_t size;
    int owned;
} FileEntry;

struct Directory {
    char name[MAX_NAME_LEN];
    FileEntry* files;
    size_t file_count;
    Directory* parent;
    Directory* children;
    size_t child_count;
};

typedef struct {
    FileEntry* entry;
    size_t offset;
    int used;
} FileHandle;

void fs_init(void);
FileHandle* fs_open(const char* filename);
size_t fs_read(FileHandle* fh, uint8_t* buffer, size_t bytes);
void fs_close(FileHandle* fh);
int fs_list(void);
int fs_change_dir(const char* path);
int fs_cd_up(void);
int fs_create(const char* filename);
int fs_delete(const char* filename);
int fs_delete_dir(const char* dirname);
int fs_create_dir(const char* dirname);
int fs_write(const char* filename, const uint8_t* data, size_t size);
const char* fs_get_cwd(void);
const Directory* fs_get_current_dir(void);

#endif
