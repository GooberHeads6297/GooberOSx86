#ifndef FS_BACKEND_H
#define FS_BACKEND_H

#include "filesystem.h"
#include <stddef.h>
#include <stdint.h>

void memfs_init(void);
FileHandle* memfs_dir_open(Directory* dir, const char* filename);
size_t memfs_read(FileHandle* fh, uint8_t* buffer, size_t bytes);
void memfs_close(FileHandle* fh);
int memfs_list(void);
int memfs_change_dir(const char* path);
int memfs_cd_up(void);
int memfs_dir_create(Directory* dir, const char* filename);
int memfs_create(const char* filename);
int memfs_dir_write(Directory* dir, const char* filename,
                    const uint8_t* data, size_t size);
int memfs_write(const char* filename, const uint8_t* data, size_t size);
int memfs_delete(const char* filename);
int memfs_dir_create_dir(Directory* dir, const char* dirname);
int memfs_create_dir(const char* dirname);
int memfs_dir_rename(Directory* dir, const char* old_name, const char* new_name);
int memfs_rename(const char* old_name, const char* new_name);
int memfs_delete_dir(const char* dirname);
const char* memfs_get_cwd(void);
const Directory* memfs_get_current_dir(void);
Directory* memfs_get_cwd_dir(void);
Directory* memfs_dir_find_child(Directory* dir, const char* name);
void memfs_set_current_dir(Directory* dir);
Directory* memfs_get_desktop_dir(void);
int memfs_sync(void);

int fat32_try_auto_mount(const char* root_cfg);
int fat32_is_mounted(void);
int fat32_mount_device(int device_index, int part_index);
/* Mount without requiring a Goober volume signature (USB sticks, etc.). */
int fat32_mount_device_loose(int device_index, int part_index);
void fat32_unmount(void);
void fat32_init_cwd(void);
FileHandle* fat32_dir_open(Directory* dir, const char* filename);
size_t fat32_read(FileHandle* fh, uint8_t* buffer, size_t bytes);
void fat32_close(FileHandle* fh);
int fat32_list(void);
int fat32_change_dir(const char* path);
int fat32_cd_up(void);
int fat32_dir_create(Directory* dir, const char* filename);
int fat32_create(const char* filename);
int fat32_dir_write(Directory* dir, const char* filename,
                    const uint8_t* data, size_t size);
int fat32_write(const char* filename, const uint8_t* data, size_t size);
int fat32_delete(const char* filename);
int fat32_dir_create_dir(Directory* dir, const char* dirname);
int fat32_create_dir(const char* dirname);
int fat32_dir_rename(Directory* dir, const char* old_name, const char* new_name);
int fat32_rename(const char* old_name, const char* new_name);
int fat32_delete_dir(const char* dirname);
const char* fat32_get_cwd(void);
const Directory* fat32_get_current_dir(void);
Directory* fat32_get_cwd_dir(void);
Directory* fat32_dir_find_child(Directory* dir, const char* name);
void fat32_set_current_dir(Directory* dir);
Directory* fat32_get_desktop_dir(void);
void fat32_dir_refresh(Directory* dir);
int fat32_sync(void);
const char* fat32_mount_label(void);
int fat32_mount_device_index(void);
int fat32_mount_part_index(void);
const char* fat32_mount_description(void);

#endif
