#include "filesystem.h"
#include <stddef.h>
#include <stdint.h>
#include "../lib/memory.h"
#include "../lib/string.h"
#include "../kernel.h"

extern void print(const char*);

const uint8_t file1_data[] = "Hello from GooberOS root!\n";
const uint8_t readme_data[] = "This is a readme file in /docs\n";
const uint8_t log_data[] = "System logs...\n";
const uint8_t config_data[] = "Configuration settings.\n";

static FileEntry root_files_static[] = {
    { "file1.txt", (uint8_t*)file1_data, sizeof(file1_data) - 1, 0 },
    { "log.txt", (uint8_t*)log_data, sizeof(log_data) - 1, 0 }
};

static FileEntry docs_files_static[] = {
    { "readme.txt", (uint8_t*)readme_data, sizeof(readme_data) - 1, 0 }
};

static FileEntry etc_files_static[] = {
    { "config.ini", (uint8_t*)config_data, sizeof(config_data) - 1, 0 }
};

static Directory base_root_dir[] = {
    { "/", root_files_static, sizeof(root_files_static) / sizeof(FileEntry), NULL, NULL, 0 },
    { "docs", docs_files_static, sizeof(docs_files_static) / sizeof(FileEntry), &base_root_dir[0], NULL, 0 },
    { "etc", etc_files_static, sizeof(etc_files_static) / sizeof(FileEntry), &base_root_dir[0], NULL, 0 }
};

static Directory* root_dir = base_root_dir;
static size_t root_dir_count = sizeof(base_root_dir) / sizeof(Directory);

#define MAX_OPEN_FILES 8
static FileHandle handles[MAX_OPEN_FILES];

Directory* current_dir = NULL;

static int dir_name_equal(const char* a, const char* b) {
    return strcmp(a, b) == 0;
}

void fs_init() {
    for (size_t i = 0; i < MAX_OPEN_FILES; i++) {
        handles[i].used = 0;
        handles[i].entry = NULL;
        handles[i].offset = 0;
    }
    root_dir = base_root_dir;
    root_dir_count = sizeof(base_root_dir) / sizeof(Directory);
    root_dir[0].parent = NULL;
    root_dir[1].parent = &root_dir[0];
    root_dir[2].parent = &root_dir[0];
    if (root_dir[0].children == NULL) {
        root_dir[0].children = (Directory*)kmalloc(2 * sizeof(Directory));
        root_dir[0].children[0] = root_dir[1];
        root_dir[0].children[1] = root_dir[2];
        root_dir[0].child_count = 2;
    }
    current_dir = &root_dir[0];
}

FileHandle* fs_open(const char* filename) {
    if (!current_dir || !filename) return 0;
    for (size_t i = 0; i < current_dir->file_count; i++) {
        if (dir_name_equal(filename, current_dir->files[i].name)) {
            for (size_t h = 0; h < MAX_OPEN_FILES; h++) {
                if (!handles[h].used) {
                    handles[h].used = 1;
                    handles[h].entry = &current_dir->files[i];
                    handles[h].offset = 0;
                    return &handles[h];
                }
            }
            return 0;
        }
    }
    return 0;
}

size_t fs_read(FileHandle* fh, uint8_t* buffer, size_t bytes) {
    if (!fh || !fh->entry || !buffer) return 0;
    if (fh->offset >= fh->entry->size) return 0;
    size_t remain = fh->entry->size - fh->offset;
    size_t to_read = (bytes < remain) ? bytes : remain;
    for (size_t i = 0; i < to_read; i++) {
        buffer[i] = fh->entry->data[fh->offset + i];
    }
    fh->offset += to_read;
    return to_read;
}

void fs_close(FileHandle* fh) {
    if (!fh) return;
    fh->used = 0;
    fh->entry = NULL;
    fh->offset = 0;
}

int fs_list() {
    print("Contents of directory: ");
    print(current_dir->name);
    print("\n");
    for (size_t i = 0; i < current_dir->file_count; i++) {
        print(current_dir->files[i].name);
        print("\n");
    }
    for (size_t i = 0; i < current_dir->child_count; i++) {
        print("<DIR> ");
        print(current_dir->children[i].name);
        print("\n");
    }
    return 0;
}

static Directory* find_child_dir(Directory* dir, const char* name) {
    if (!dir) return NULL;
    for (size_t i = 0; i < dir->child_count; i++) {
        if (dir_name_equal(dir->children[i].name, name)) return &dir->children[i];
    }
    return NULL;
}

int fs_change_dir(const char* path) {
    if (!path || path[0] == '\0') {
        return -1;
    }
    if (path[0] == '/' && path[1] == '\0') {
        current_dir = &root_dir[0];
        return 0;
    }
    if (dir_name_equal(path, "..")) {
        if (current_dir->parent) {
            current_dir = current_dir->parent;
            return 0;
        }
        return -1;
    }
    Directory* found = find_child_dir(current_dir, path);
    if (found) {
        current_dir = found;
        return 0;
    }
    print("cd: Directory not found: ");
    print(path);
    print("\n");
    return -1;
}

int fs_cd_up() {
    if (!current_dir) return -1;
    if (current_dir->parent) {
        current_dir = current_dir->parent;
        return 0;
    }
    return -1;
}

int fs_create(const char* filename) {
    if (!filename || filename[0] == '\0') return -1;
    for (size_t i = 0; i < current_dir->file_count; i++) {
        if (dir_name_equal(filename, current_dir->files[i].name)) {
            print("fs_create: File already exists\n");
            return -1;
        }
    }
    size_t new_count = current_dir->file_count + 1;
    FileEntry* new_files = (FileEntry*)kmalloc(new_count * sizeof(FileEntry));
    if (!new_files) {
        print("fs_create: Memory allocation failed\n");
        return -1;
    }
    for (size_t i = 0; i < current_dir->file_count; i++) {
        new_files[i] = current_dir->files[i];
    }
    for (size_t j = 0; j < MAX_NAME_LEN; j++) new_files[new_count - 1].name[j] = 0;
    strncpy(new_files[new_count - 1].name, filename, MAX_NAME_LEN);
    new_files[new_count - 1].name[MAX_NAME_LEN - 1] = '\0';
    new_files[new_count - 1].data = 0;
    new_files[new_count - 1].size = 0;
    new_files[new_count - 1].owned = 0;
    if (current_dir->files != root_files_static &&
        current_dir->files != docs_files_static &&
        current_dir->files != etc_files_static) {
        kfree(current_dir->files);
    }
    current_dir->files = new_files;
    current_dir->file_count = new_count;
    return 0;
}

int fs_write(const char* filename, const uint8_t* data, size_t size) {
    if (!filename) return -1;
    FileEntry* target = NULL;
    for (size_t i = 0; i < current_dir->file_count; i++) {
        if (dir_name_equal(filename, current_dir->files[i].name)) {
            target = &current_dir->files[i];
            break;
        }
    }
    if (!target) {
        if (fs_create(filename) != 0) return -1;
        target = &current_dir->files[current_dir->file_count - 1];
    }
    if (target->owned && target->data) {
        kfree(target->data);
        target->data = NULL;
        target->size = 0;
        target->owned = 0;
    }
    if (size > 0) {
        uint8_t* buf = (uint8_t*)kmalloc(size);
        if (!buf) return -1;
        for (size_t i = 0; i < size; i++) buf[i] = data[i];
        target->data = buf;
        target->size = size;
        target->owned = 1;
    } else {
        target->data = 0;
        target->size = 0;
        target->owned = 1;
    }
    return 0;
}

int fs_delete(const char* filename) {
    if (!filename || filename[0] == '\0') return -1;
    size_t idx = current_dir->file_count;
    for (size_t i = 0; i < current_dir->file_count; i++) {
        if (dir_name_equal(filename, current_dir->files[i].name)) {
            idx = i;
            break;
        }
    }
    if (idx == current_dir->file_count) {
        print("fs_delete: File not found\n");
        return -1;
    }
    if (current_dir->files[idx].owned && current_dir->files[idx].data) {
        kfree(current_dir->files[idx].data);
    }
    size_t new_count = current_dir->file_count - 1;
    if (new_count == 0) {
        if (current_dir->files != root_files_static &&
            current_dir->files != docs_files_static &&
            current_dir->files != etc_files_static) {
            kfree(current_dir->files);
        }
        current_dir->files = 0;
        current_dir->file_count = 0;
        return 0;
    }
    FileEntry* new_files = (FileEntry*)kmalloc(new_count * sizeof(FileEntry));
    if (!new_files) {
        print("fs_delete: Memory allocation failed\n");
        return -1;
    }
    for (size_t i = 0, j = 0; i < current_dir->file_count; i++) {
        if (i != idx) {
            new_files[j++] = current_dir->files[i];
        }
    }
    if (current_dir->files != root_files_static &&
        current_dir->files != docs_files_static &&
        current_dir->files != etc_files_static) {
        kfree(current_dir->files);
    }
    current_dir->files = new_files;
    current_dir->file_count = new_count;
    return 0;
}

int fs_create_dir(const char* dirname) {
    if (!dirname || dirname[0] == '\0') return -1;
    for (size_t i = 0; i < current_dir->child_count; i++) {
        if (dir_name_equal(dirname, current_dir->children[i].name)) {
            print("fs_create_dir: Directory already exists\n");
            return -1;
        }
    }
    Directory* new_children = (Directory*)kmalloc((current_dir->child_count + 1) * sizeof(Directory));
    if (!new_children) {
        print("fs_create_dir: Memory allocation failed\n");
        return -1;
    }
    for (size_t i = 0; i < current_dir->child_count; i++) {
        new_children[i] = current_dir->children[i];
    }
    for (size_t j = 0; j < MAX_NAME_LEN; j++) new_children[current_dir->child_count].name[j] = 0;
    strncpy(new_children[current_dir->child_count].name, dirname, MAX_NAME_LEN);
    new_children[current_dir->child_count].name[MAX_NAME_LEN - 1] = '\0';
    new_children[current_dir->child_count].files = 0;
    new_children[current_dir->child_count].file_count = 0;
    new_children[current_dir->child_count].parent = current_dir;
    new_children[current_dir->child_count].children = NULL;
    new_children[current_dir->child_count].child_count = 0;
    if (current_dir->children) kfree(current_dir->children);
    current_dir->children = new_children;
    current_dir->child_count++;
    return 0;
}

int fs_delete_dir(const char* dirname) {
    if (!dirname || dirname[0] == '\0') return -1;
    for (size_t i = 0; i < current_dir->child_count; i++) {
        if (dir_name_equal(dirname, current_dir->children[i].name)) {
            for (size_t f = 0; f < current_dir->children[i].file_count; f++) {
                if (current_dir->children[i].files[f].owned && current_dir->children[i].files[f].data) {
                    kfree(current_dir->children[i].files[f].data);
                }
            }
            if (current_dir->children[i].files &&
                current_dir->children[i].files != root_files_static &&
                current_dir->children[i].files != docs_files_static &&
                current_dir->children[i].files != etc_files_static) {
                kfree(current_dir->children[i].files);
            }
            size_t new_count = current_dir->child_count - 1;
            if (new_count == 0) {
                kfree(current_dir->children);
                current_dir->children = NULL;
                current_dir->child_count = 0;
                return 0;
            }
            Directory* new_children = (Directory*)kmalloc(new_count * sizeof(Directory));
            if (!new_children) {
                print("fs_delete_dir: Memory allocation failed\n");
                return -1;
            }
            for (size_t a = 0, b = 0; a < current_dir->child_count; a++) {
                if (a != i) new_children[b++] = current_dir->children[a];
            }
            kfree(current_dir->children);
            current_dir->children = new_children;
            current_dir->child_count = new_count;
            return 0;
        }
    }
    print("fs_delete_dir: Directory not found\n");
    return -1;
}

const char* fs_get_cwd(void) {
    static char path[256];
    for (size_t i = 0; i < sizeof(path); i++) path[i] = 0;
    if (!current_dir) { path[0] = '/'; path[1] = '\0'; return path; }
    if (current_dir->parent == NULL) { path[0] = '/'; path[1] = '\0'; return path; }
    const char* parts[32];
    int count = 0;
    Directory* it = current_dir;
    while (it && it->parent && count < (int)(sizeof(parts)/sizeof(parts[0]))) {
        parts[count++] = it->name;
        it = it->parent;
    }
    path[0] = '/';
    path[1] = '\0';
    for (int i = count - 1; i >= 0; i--) {
        strcat(path, parts[i]);
        if (i > 0) strcat(path, "/");
    }
    return path;
}
