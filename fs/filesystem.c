#include "filesystem.h"
#include <stddef.h>
#include "../kernel.h"

extern void print(const char*);

typedef struct Directory {
    const char* name;
    const FileEntry* files;
    size_t file_count;
} Directory;

static const uint8_t file1_data[] = "Hello from GooberOS root!\n";
static const uint8_t readme_data[] = "This is a readme file in /docs\n";
static const uint8_t log_data[] = "System logs...\n";
static const uint8_t config_data[] = "Configuration settings.\n";

static FileEntry root_files[] = {
    { "file1.txt", file1_data, sizeof(file1_data) - 1 },
    { "log.txt", log_data, sizeof(log_data) - 1 }
};

static FileEntry docs_files[] = {
    { "readme.txt", readme_data, sizeof(readme_data) - 1 }
};

static FileEntry etc_files[] = {
    { "config.ini", config_data, sizeof(config_data) - 1 }
};

static Directory root_dir[] = {
    { "/", root_files, sizeof(root_files) / sizeof(FileEntry) },
    { "docs", docs_files, sizeof(docs_files) / sizeof(FileEntry) },
    { "etc", etc_files, sizeof(etc_files) / sizeof(FileEntry) }
};

#define MAX_OPEN_FILES 4
static FileHandle handles[MAX_OPEN_FILES];
static int handle_count = 0;

static const Directory* current_dir = &root_dir[0];

void fs_init() {
    handle_count = 0;
    current_dir = &root_dir[0];
}

FileHandle* fs_open(const char* filename) {
    for (size_t i = 0; i < current_dir->file_count; i++) {
        const char* n = current_dir->files[i].name;
        size_t j = 0;
        while (filename[j] && filename[j] == n[j]) j++;
        if (filename[j] == '\0' && n[j] == '\0') {
            if (handle_count >= MAX_OPEN_FILES) return 0;
            handles[handle_count].entry = &current_dir->files[i];
            handles[handle_count].offset = 0;
            return &handles[handle_count++];
        }
    }
    return 0;
}

size_t fs_read(FileHandle* fh, uint8_t* buffer, size_t bytes) {
    if (!fh) return 0;
    size_t remain = fh->entry->size - fh->offset;
    size_t to_read = (bytes < remain) ? bytes : remain;
    for (size_t i = 0; i < to_read; i++) {
        buffer[i] = fh->entry->data[fh->offset + i];
    }
    fh->offset += to_read;
    return to_read;
}

void fs_close(FileHandle* fh) {
    (void)fh;
}

int fs_list() {
    print("Contents of directory: ");
    print(current_dir->name);
    print("\n");

    for (size_t i = 0; i < current_dir->file_count; i++) {
        print(current_dir->files[i].name);
        print("\n");
    }

    if (current_dir == &root_dir[0]) {
        for (size_t i = 1; i < sizeof(root_dir) / sizeof(Directory); i++) {
            print("<DIR> ");
            print(root_dir[i].name);
            print("\n");
        }
    }

    return 0;
}

int fs_change_dir(const char* path) {
    if (!path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        current_dir = &root_dir[0];
        return 0;
    }

    for (size_t i = 1; i < sizeof(root_dir) / sizeof(Directory); i++) {
        const char* name = root_dir[i].name;
        size_t j = 0;
        while (path[j] && path[j] == name[j]) j++;
        if (path[j] == '\0' && name[j] == '\0') {
            current_dir = &root_dir[i];
            return 0;
        }
    }

    print("cd: Directory not found: ");
    print(path);
    print("\n");
    return -1;
}

int fs_cd_up() {
    if (current_dir == &root_dir[0]) {
        return -1;
    }
    current_dir = &root_dir[0];
    return 0;
}
