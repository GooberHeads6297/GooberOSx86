#include "driver_log.h"
#include "../../fs/filesystem.h"

#define DRIVER_LOG_CAPACITY 8192U

static char g_driver_log[DRIVER_LOG_CAPACITY];
static uint32_t g_driver_log_len = 0;
static uint32_t g_driver_log_version = 0;
static int g_driver_log_truncated = 0;

void driver_log_clear(void) {
    g_driver_log_len = 0;
    g_driver_log[0] = '\0';
    g_driver_log_version++;
    g_driver_log_truncated = 0;
}

void driver_log(const char* msg) {
    if (!msg) return;
    for (uint32_t i = 0; msg[i]; i++) {
        if (g_driver_log_len + 1 >= DRIVER_LOG_CAPACITY) {
            g_driver_log_truncated = 1;
            g_driver_log[DRIVER_LOG_CAPACITY - 1] = '\0';
            return;
        }
        g_driver_log[g_driver_log_len++] = msg[i];
        g_driver_log[g_driver_log_len] = '\0';
        g_driver_log_version++;
    }
}

void driver_log_u32(uint32_t value) {
    char tmp[11];
    int n = 0;
    if (value == 0) {
        driver_log("0");
        return;
    }
    while (value && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (n > 0) {
        char c[2];
        c[0] = tmp[--n];
        c[1] = '\0';
        driver_log(c);
    }
}

void driver_log_hex32(uint32_t value) {
    static const char* hex = "0123456789ABCDEF";
    char out[11];
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 8; i++)
        out[2 + i] = hex[(value >> (28 - i * 4)) & 0xFU];
    out[10] = '\0';
    driver_log(out);
}

void driver_log_line(const char* msg) {
    driver_log(msg);
    driver_log("\n");
}

void driver_log_dump(void (*write)(const char*)) {
    if (!write) return;
    if (g_driver_log_len == 0) {
        write("[driver-log] no entries yet.\n");
        return;
    }
    write(g_driver_log);
    if (g_driver_log_truncated)
        write("\n[driver-log] log truncated; newest entries may be missing.\n");
}

const char* driver_log_buffer(void) {
    return g_driver_log;
}

uint32_t driver_log_size(void) {
    return g_driver_log_len;
}

uint32_t driver_log_version(void) {
    return g_driver_log_version;
}

int driver_log_sync_desktop_file(void) {
    Directory* desktop = fs_get_desktop_dir();
    if (!desktop) return -1;
    if (g_driver_log_len == 0) {
        static const char empty[] = "[driver-log] no entries yet.\n";
        return fs_dir_write(desktop, "log.txt", (const uint8_t*)empty, sizeof(empty) - 1);
    }
    return fs_dir_write(desktop, "log.txt", (const uint8_t*)g_driver_log, g_driver_log_len);
}
