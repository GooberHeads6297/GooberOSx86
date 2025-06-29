#include "filesystem.h"
#include <stddef.h>

extern void print(const char*);



static const uint8_t file1_data[] = "Hello from GooberOS file system!\n";

static const FileEntry files[] = {
    { "file1.txt", file1_data, sizeof(file1_data) - 1 },
};

#define MAX_OPEN_FILES 4

static FileHandle handles[MAX_OPEN_FILES];
static int handle_count = 0;

void fs_init() {
    handle_count = 0;
}

FileHandle* fs_open(const char* filename) {
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        const char* n = files[i].name;
        size_t j = 0;
        while (filename[j] && filename[j] == n[j]) j++;
        if (filename[j] == '\0' && n[j] == '\0') {
            if (handle_count >= MAX_OPEN_FILES) return 0;
            handles[handle_count].entry = &files[i];
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
    (void)fh; // no action needed for now
}

// --- New implementations ---

#include "../kernel.h" // for print()

int fs_list() {
    print("Files in root directory:\n");
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        print(files[i].name);
        print("\n");
    }
    return 0;
}

int fs_change_dir(const char* path) {
    // Since no directories yet, only accept "/" or "" as root
    if (path == 0 || path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        return 0; // success, root directory
    }
    print("Error: Only root directory '/' supported.\n");
    return -1; // failure
}
