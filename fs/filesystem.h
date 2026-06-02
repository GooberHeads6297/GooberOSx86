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

/*
 * Directory-explicit operations. These behave exactly like their current-dir
 * counterparts above, but act on a caller-supplied directory handle instead of
 * the global working directory. This lets the VESA desktop own a fixed folder
 * (see fs_get_desktop_dir) without being disturbed by File Explorer navigation.
 */
Directory* fs_get_desktop_dir(void);
Directory* fs_get_cwd_dir(void);
Directory* fs_dir_find_child(Directory* dir, const char* name);
void fs_set_current_dir(Directory* dir);
FileHandle* fs_dir_open(Directory* dir, const char* filename);
int fs_dir_create(Directory* dir, const char* filename);
int fs_dir_create_dir(Directory* dir, const char* dirname);
int fs_dir_write(Directory* dir, const char* filename, const uint8_t* data, size_t size);

#endif
