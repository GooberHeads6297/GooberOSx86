#include "filesystem.h"
#include "fs_backend.h"
#include "config_boot.h"
#include "../kernel.h"
#include "../lib/string.h"

extern void print(const char*);

typedef enum {
    VFS_BACKEND_MEMFS = 0,
    VFS_BACKEND_FAT32 = 1
} vfs_backend_t;

static vfs_backend_t g_backend = VFS_BACKEND_MEMFS;
static char g_mount_line[96];

static int use_fat32(void) {
    return g_backend == VFS_BACKEND_FAT32 && fat32_is_mounted();
}

void fs_init(void) {
    const boot_config_t* cfg = boot_get_config();
    const char* root_cfg = "live";

    g_mount_line[0] = '\0';
    if (cfg && cfg->root[0]) root_cfg = cfg->root;

    if (strcmp(root_cfg, "live") == 0) {
        memfs_init();
        g_backend = VFS_BACKEND_MEMFS;
        print("[boot] fs_init complete (memfs / live).\n");
        return;
    }

    if (fat32_try_auto_mount(root_cfg) == 0) {
        g_backend = VFS_BACKEND_FAT32;
        fat32_init_cwd();
        {
            const char* desc = fat32_mount_description();
            if (desc && desc[0]) {
                for (size_t i = 0; i < sizeof(g_mount_line) - 1 && desc[i]; i++)
                    g_mount_line[i] = desc[i];
                g_mount_line[sizeof(g_mount_line) - 1] = '\0';
            }
        }
        print("[boot] fs_init complete (");
        print(g_mount_line[0] ? g_mount_line : "fat32");
        print(").\n");
        config_boot_apply();
        return;
    }

    memfs_init();
    g_backend = VFS_BACKEND_MEMFS;
    print("[boot] fs_init complete (memfs / live).\n");
}

int fs_is_persistent(void) {
    return use_fat32() ? 1 : 0;
}

int fs_sync(void) {
    return use_fat32() ? fat32_sync() : memfs_sync();
}

const char* fs_backend_name(void) {
    return use_fat32() ? "fat32" : "memfs";
}

const char* fs_mount_description(void) {
    if (use_fat32() && g_mount_line[0]) return g_mount_line;
    return use_fat32() ? "fat32" : "memfs (live)";
}

FileHandle* fs_open(const char* filename) {
    return use_fat32() ? fat32_dir_open(fat32_get_cwd_dir(), filename)
                       : memfs_dir_open(memfs_get_cwd_dir(), filename);
}

FileHandle* fs_dir_open(Directory* dir, const char* filename) {
    return use_fat32() ? fat32_dir_open(dir, filename)
                       : memfs_dir_open(dir, filename);
}

size_t fs_read(FileHandle* fh, uint8_t* buffer, size_t bytes) {
    return use_fat32() ? fat32_read(fh, buffer, bytes) : memfs_read(fh, buffer, bytes);
}

void fs_close(FileHandle* fh) {
    if (use_fat32()) fat32_close(fh);
    else memfs_close(fh);
}

int fs_list(void) {
    return use_fat32() ? fat32_list() : memfs_list();
}

int fs_change_dir(const char* path) {
    return use_fat32() ? fat32_change_dir(path) : memfs_change_dir(path);
}

int fs_cd_up(void) {
    return use_fat32() ? fat32_cd_up() : memfs_cd_up();
}

int fs_create(const char* filename) {
    return use_fat32() ? fat32_create(filename) : memfs_create(filename);
}

int fs_dir_create(Directory* dir, const char* filename) {
    return use_fat32() ? fat32_dir_create(dir, filename) : memfs_dir_create(dir, filename);
}

int fs_write(const char* filename, const uint8_t* data, size_t size) {
    return use_fat32() ? fat32_write(filename, data, size) : memfs_write(filename, data, size);
}

int fs_dir_write(Directory* dir, const char* filename,
                 const uint8_t* data, size_t size) {
    return use_fat32() ? fat32_dir_write(dir, filename, data, size)
                       : memfs_dir_write(dir, filename, data, size);
}

int fs_delete(const char* filename) {
    return use_fat32() ? fat32_delete(filename) : memfs_delete(filename);
}

int fs_create_dir(const char* dirname) {
    return use_fat32() ? fat32_create_dir(dirname) : memfs_create_dir(dirname);
}

int fs_dir_create_dir(Directory* dir, const char* dirname) {
    return use_fat32() ? fat32_dir_create_dir(dir, dirname)
                       : memfs_dir_create_dir(dir, dirname);
}

int fs_rename(const char* old_name, const char* new_name) {
    return use_fat32() ? fat32_rename(old_name, new_name) : memfs_rename(old_name, new_name);
}

int fs_dir_rename(Directory* dir, const char* old_name, const char* new_name) {
    return use_fat32() ? fat32_dir_rename(dir, old_name, new_name)
                       : memfs_dir_rename(dir, old_name, new_name);
}

int fs_delete_dir(const char* dirname) {
    return use_fat32() ? fat32_delete_dir(dirname) : memfs_delete_dir(dirname);
}

const char* fs_get_cwd(void) {
    return use_fat32() ? fat32_get_cwd() : memfs_get_cwd();
}

const Directory* fs_get_current_dir(void) {
    return use_fat32() ? fat32_get_current_dir() : memfs_get_current_dir();
}

Directory* fs_get_cwd_dir(void) {
    return use_fat32() ? fat32_get_cwd_dir() : memfs_get_cwd_dir();
}

Directory* fs_dir_find_child(Directory* dir, const char* name) {
    return use_fat32() ? fat32_dir_find_child(dir, name) : memfs_dir_find_child(dir, name);
}

void fs_set_current_dir(Directory* dir) {
    if (use_fat32()) fat32_set_current_dir(dir);
    else memfs_set_current_dir(dir);
}

void fs_dir_refresh(Directory* dir) {
    if (use_fat32()) fat32_dir_refresh(dir);
}

Directory* fs_get_desktop_dir(void) {
    return use_fat32() ? fat32_get_desktop_dir() : memfs_get_desktop_dir();
}
