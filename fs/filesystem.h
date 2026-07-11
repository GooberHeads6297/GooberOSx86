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
    /* FAT32 backing (zero when in-memory backend) */
    uint8_t  fat32;
    uint32_t fat_cluster;
    uint8_t  is_dir;
} FileEntry;

struct Directory {
    char name[MAX_NAME_LEN];
    FileEntry* files;
    size_t file_count;
    size_t files_cap;
    Directory* parent;
    Directory* children;
    size_t child_count;
    size_t children_cap;
    /* FAT32 backing (zero when in-memory backend) */
    uint8_t  fat32;
    uint32_t fat_cluster;
};

typedef struct {
    FileEntry* entry;
    size_t offset;
    int used;
    /* FAT32 sequential read cursor */
    uint32_t fat_cluster;
    uint32_t fat_cluster_base;
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
int fs_rename(const char* old_name, const char* new_name);
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
/* Re-read a FAT32-backed directory from disk (no-op for memfs). */
void fs_dir_refresh(Directory* dir);
FileHandle* fs_dir_open(Directory* dir, const char* filename);
int fs_dir_create(Directory* dir, const char* filename);
int fs_dir_create_dir(Directory* dir, const char* dirname);
int fs_dir_rename(Directory* dir, const char* old_name, const char* new_name);
int fs_dir_write(Directory* dir, const char* filename, const uint8_t* data, size_t size);

/* Persistent filesystem helpers */
int fs_is_persistent(void);
int fs_sync(void);
const char* fs_backend_name(void);
const char* fs_mount_description(void);

#endif
