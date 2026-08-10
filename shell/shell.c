#include "shell.h"
#include <stddef.h>
#include "../drivers/io/io.h"
/*
 * Phase 3f closes the Phase 3 umbrella: storage / editor / taskmgr / games
 * are now linked under -m64 alongside the existing x86 build, so the
 * arch-conditional include block + shell_deferred_3f stubs that 3e used
 * are gone. Both arches see the same parser bodies. The bios_disk.c +
 * bios_int13.s real-mode INT 13h path stays x86-only (it physically can't
 * link into long mode); on x64 the storage stack reaches USB / SDHCI /
 * AHCI / NVMe / ATA-PIO devices through the controller drivers.
 */
#include "../games/snake.h"
#include "../games/cubeDip.h"
#include "../games/pong.h"
#include "../games/doom.h"
#include "../editor/editor.h"
#include "../taskmgr/taskmgr.h"
#include "../drivers/storage/storage.h"
#include "../drivers/storage/partition.h"
#include "../drivers/usb/usb.h"
#include "../drivers/usb/storage/msc.h"
#include "../drivers/usb/host/host.h"
#include "../drivers/input/touchpad.h"
#include "../fs/filesystem.h"
#include "../fs/fs_backend.h"
#include "../install/install.h"
#include "../userspace/userspace.h"
#include "../lib/string.h"
#include "../drivers/video/vga.h"
#include "../drivers/video/textcon.h"
#include "../drivers/video/display.h"
#include "../drivers/video/connector.h"
#include "../drivers/video/intel_gfx.h"
#include "../drivers/keyboard/keyboard.h"
#include "../drivers/timer/timer.h"
#include "../drivers/diagnostics/driver_log.h"
#include "../gui/window.h"

#define PROMPT_COLOR VGA_COLOR_BLUE
#define INPUT_COLOR VGA_COLOR_BLUE
static uint8_t current_color = VGA_COLOR_LIGHT_GREEN;
#define SCREEN_COLS 80
#define SCREEN_ROWS 25

extern char keyboard_read_char();
extern uint8_t cursor_row;
extern uint8_t cursor_col;

#ifndef EMBED_INSTALL_ISO
#define EMBED_INSTALL_ISO 0
#endif

#if EMBED_INSTALL_ISO
#ifdef __x86_64__
extern unsigned char _binary_GooberOSx86_x64_iso_start;
extern unsigned char _binary_GooberOSx86_x64_iso_end;
#define INSTALL_IMAGE_START _binary_GooberOSx86_x64_iso_start
#define INSTALL_IMAGE_END   _binary_GooberOSx86_x64_iso_end
#else
extern unsigned char _binary_GooberOSx86_iso_start;
extern unsigned char _binary_GooberOSx86_iso_end;
#define INSTALL_IMAGE_START _binary_GooberOSx86_iso_start
#define INSTALL_IMAGE_END   _binary_GooberOSx86_iso_end
#endif
#endif

extern FileHandle* fs_open(const char* filename);
extern size_t fs_read(FileHandle* fh, uint8_t* buffer, size_t bytes);
extern void fs_close(FileHandle* fh);
extern int fs_create(const char* filename);
extern int fs_delete(const char* filename);
extern int fs_delete_dir(const char* dirname);
extern int fs_create_dir(const char* dirname);
extern int fs_rename(const char* old_name, const char* new_name);
extern int fs_write(const char* filename, const uint8_t* data, size_t size);
extern int fs_list(void);
extern int fs_change_dir(const char* path);
extern int fs_cd_up(void);
extern const char* fs_get_cwd();

#define INPUT_BUFFER_SIZE 256
#define HISTORY_SIZE 32
static char input_buffer[INPUT_BUFFER_SIZE];
static size_t input_pos = 0;
static char history[HISTORY_SIZE][INPUT_BUFFER_SIZE];
static int history_next = 0;
static int history_count = 0;
static int history_nav_offset = -1; // -1 = not browsing history
static char saved_input[INPUT_BUFFER_SIZE];

static int strcmp_local(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

static int strncmp_local(const char* s1, const char* s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) return (unsigned char)s1[i] - (unsigned char)s2[i];
        if (s1[i] == '\0') return 0;
    }
    return 0;
}

static int prompt_start_row = 0;
static int prompt_start_col = 0;
static int prev_cursor_row = -1;
static int prev_cursor_col = -1;
static uint16_t prev_cell_value = 0;
static int cursor_enabled = 1;
static int cursor_drawn = 0;
static uint32_t last_blink_tick = 0;
static shell_clear_sink_t redirected_clear_sink = NULL;
static void* redirected_output_ctx = NULL;

static int shell_output_redirect_active(void) {
    return redirected_clear_sink != NULL;
}

void shell_set_redirect(kernel_print_sink_t write_sink, shell_clear_sink_t clear_sink, void* ctx) {
    kernel_set_print_sink(write_sink, ctx);
    redirected_clear_sink = clear_sink;
    redirected_output_ctx = ctx;
}

void shell_clear_redirect(void) {
    kernel_clear_print_sink();
    redirected_clear_sink = NULL;
    redirected_output_ctx = NULL;
}

static void shell_clear_display(void) {
    if (redirected_clear_sink) {
        redirected_clear_sink(redirected_output_ctx);
        return;
    }
    /* The text console layer mirrors to whichever backend is active (VGA
     * 0xB8000 on legacy BIOS, the GOP framebuffer on UEFI x64). Fall back
     * to the legacy direct clear when no backend was ever bound -- that
     * preserves the pre-textcon behaviour for the rare early-boot path
     * where shell_clear_display is hit before shell_init. */
    if (con_ready()) {
        con_clear(0x0Fu);
    } else {
        clear_screen();
    }
}

static void put_cell(int r, int c, char ch, uint8_t attr) {
    if (r < 0 || c < 0 || r >= SCREEN_ROWS || c >= SCREEN_COLS) return;
    con_put_cell(r, c, ch, attr);
}

static int input_index_to_pos(size_t idx, int* out_r, int* out_c) {
    if (idx > INPUT_BUFFER_SIZE) return -1;
    int linear = prompt_start_col + (int)idx;
    int r = prompt_start_row + linear / SCREEN_COLS;
    int c = linear % SCREEN_COLS;
    if (r < 0 || c < 0 || r >= SCREEN_ROWS || c >= SCREEN_COLS) return -1;
    *out_r = r; *out_c = c;
    return 0;
}

static void ensure_scroll() {
    while (cursor_row >= SCREEN_ROWS) {
        /* Scroll the whole console up one row through the text-console
         * abstraction so the active backend (VGA cell plane on legacy
         * BIOS, or the GOP framebuffer on UEFI x64) sees the shift in
         * one batched call instead of cell-by-cell. */
        con_scroll_up(1, current_color);
        cursor_row--;
        if (prompt_start_row > 0) prompt_start_row--;
        if (prev_cursor_row > 0) prev_cursor_row--;
    }
}

static void restore_prev_cursor_cell() {
    if (prev_cursor_row == -1) return;
    put_cell(prev_cursor_row, prev_cursor_col,
             (char)(prev_cell_value & 0xFF),
             (uint8_t)(prev_cell_value >> 8));
    prev_cursor_row = -1;
    prev_cursor_col = -1;
    prev_cell_value = 0;
    cursor_drawn = 0;
}

static void draw_cursor() {
    if (!cursor_enabled) return;
    ensure_scroll();
    // DO NOT restore here; we will restore at safe sites before drawing text.
    if (cursor_row < 0 || cursor_col < 0 || cursor_row >= SCREEN_ROWS || cursor_col >= SCREEN_COLS) return;
    prev_cell_value = con_get_cell(cursor_row, cursor_col);
    move_cursor(cursor_row, cursor_col);
    put_cell(cursor_row, cursor_col, '_', PROMPT_COLOR);
    prev_cursor_row = cursor_row;
    prev_cursor_col = cursor_col;
    cursor_drawn = 1;
}

static void blink_cursor() {
    if (!cursor_enabled) return;
    uint32_t now = timer_ticks();
    if (last_blink_tick == 0) last_blink_tick = now;
    if ((now - last_blink_tick) >= 20) { // ~200ms at 100Hz
        last_blink_tick = now;
        if (cursor_drawn) restore_prev_cursor_cell();
        else draw_cursor();
    }
}

static void print_char_shell(char c) {
    restore_prev_cursor_cell();      // <— moved here
    if (c == '\n') {
        cursor_row++;
        cursor_col = 0;
        ensure_scroll();
    } else {
        put_cell(cursor_row, cursor_col, c, current_color);
        if (++cursor_col >= SCREEN_COLS) { cursor_col = 0; cursor_row++; ensure_scroll(); }
    }
    draw_cursor();
}

static void prompt() {
    // make sure we don't overwrite our first character later
    restore_prev_cursor_cell();

    const char* cwd = fs_get_cwd();
    const char* left = "GooberOS";
    for (size_t i = 0; left[i] != '\0'; i++) {
        put_cell(cursor_row, cursor_col, left[i], PROMPT_COLOR);
        if (++cursor_col >= SCREEN_COLS) { cursor_col = 0; cursor_row++; ensure_scroll(); }
    }
    if (cwd && cwd[0] != '\0' && strcmp_local(cwd, "/") != 0) {
        put_cell(cursor_row, cursor_col, '[', PROMPT_COLOR);
        if (++cursor_col >= SCREEN_COLS) { cursor_col = 0; cursor_row++; ensure_scroll(); }
        for (size_t i = 0; cwd[i] != '\0'; i++) {
            put_cell(cursor_row, cursor_col, cwd[i], PROMPT_COLOR);
            if (++cursor_col >= SCREEN_COLS) { cursor_col = 0; cursor_row++; ensure_scroll(); }
        }
        put_cell(cursor_row, cursor_col, ']', PROMPT_COLOR);
        if (++cursor_col >= SCREEN_COLS) { cursor_col = 0; cursor_row++; ensure_scroll(); }
    }
    put_cell(cursor_row, cursor_col, '>', PROMPT_COLOR);
    if (++cursor_col >= SCREEN_COLS) { cursor_col = 0; cursor_row++; ensure_scroll(); }
    put_cell(cursor_row, cursor_col, ' ', PROMPT_COLOR);
    if (++cursor_col >= SCREEN_COLS) { cursor_col = 0; cursor_row++; ensure_scroll(); }

    prompt_start_row = cursor_row;
    prompt_start_col = cursor_col;
    input_pos = 0;
    for (size_t i = 0; i < INPUT_BUFFER_SIZE; i++) input_buffer[i] = 0;

    cursor_enabled = 1;
    draw_cursor();
}


static void clear_input() {
    input_pos = 0;
    for (size_t i = 0; i < INPUT_BUFFER_SIZE; i++) input_buffer[i] = 0;
}

static size_t input_len(void) {
    size_t i = 0;
    while (i < INPUT_BUFFER_SIZE && input_buffer[i] != '\0') i++;
    return i;
}

static int history_index_from_offset(int offset) {
    // offset 0 = newest entry, offset 1 = previous...
    return (history_next - 1 - offset + HISTORY_SIZE) % HISTORY_SIZE;
}

static void set_input_line(const char* text) {
    size_t len = 0;
    while (text[len] && len < INPUT_BUFFER_SIZE - 1) len++;
    for (size_t i = 0; i < INPUT_BUFFER_SIZE; i++)
        input_buffer[i] = (i < len) ? text[i] : '\0';
    input_pos = len;
    int r = prompt_start_row, c = prompt_start_col;
    for (size_t i = 0; i < INPUT_BUFFER_SIZE; i++) {
        put_cell(r, c, (i < len) ? text[i] : ' ', INPUT_COLOR);
        c++;
        if (c >= SCREEN_COLS) { c = 0; r++; }
    }
    if (input_index_to_pos(input_pos, &r, &c) == 0) {
        cursor_row = r;
        cursor_col = c;
    }
    draw_cursor();
}

/*
 * Phase 3f x64 reboot: try the ACPI 5.0 RESET_REG GAS first (the spec-
 * mandated path on UEFI systems where the 8042 controller may be a
 * legacy emulation hole), fall back to the 8042 keyboard-controller
 * reset, and finally cli/hlt forever if neither path took effect.
 *
 * The ACPI parsing is intentionally conservative: BIOS RSDP search in
 * the EBDA / 0xE0000-0xFFFFF window, then walk RSDT / XSDT for the
 * FADT, validate ResetReg.Address.SpaceId is 0 (system memory) /
 * 1 (system I/O) / 2 (PCI config), and write RESET_VALUE there. PCI
 * config is uncommon on real Bay Trail-class hardware; we skip it
 * (returns 0, falls through to 8042). System memory writes through
 * the identity-mapped low 4 GiB; system I/O uses outb. Any failure
 * (no FADT, RESET_REG_SUP not in FADT flags, unsupported SpaceId,
 * malformed table) falls through.
 */
#ifdef __x86_64__
typedef struct __attribute__((packed)) {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} acpi_rsdp_t;

typedef struct __attribute__((packed)) {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  address_space_id;     /* 0=mem, 1=io, 2=pci */
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} acpi_gas_t;

#define ACPI_FADT_FLAG_RESET_REG_SUP (1u << 10)

/* Multiboot2 ACPI tag RSDP (kernel.c). Preferred on UEFI — BIOS EBDA scan
 * often finds nothing or hangs on real hardware. */
extern uintptr_t kernel_acpi_rsdp(void);

static acpi_rsdp_t* acpi_find_rsdp(void) {
    static const char sig[8] = { 'R','S','D',' ','P','T','R',' ' };
    uintptr_t fw = kernel_acpi_rsdp();
    if (fw) {
        acpi_rsdp_t* rsdp = (acpi_rsdp_t*)fw;
        int match = 1;
        for (int i = 0; i < 8; i++) {
            if (rsdp->signature[i] != sig[i]) { match = 0; break; }
        }
        if (match) return rsdp;
    }
#if defined(__i386__)
    /* Legacy BIOS EBDA + 0xE0000 scan — x86_64/UEFI must not use this path. */
    volatile uint16_t* ebda_seg_ptr = (volatile uint16_t*)(uintptr_t)0x40Eu;
    uint16_t ebda_seg = *ebda_seg_ptr;
    uintptr_t ebda_base = (uintptr_t)((uint32_t)ebda_seg << 4u);
    uintptr_t scan_ranges[][2] = {
        { ebda_base, ebda_base + 1024u },
        { 0xE0000u, 0x100000u },
    };
    for (int r = 0; r < 2; r++) {
        if (scan_ranges[r][0] == 0) continue;
        for (uintptr_t p = scan_ranges[r][0]; p + 8u <= scan_ranges[r][1]; p += 16u) {
            const char* s = (const char*)p;
            int match = 1;
            for (int i = 0; i < 8; i++) if (s[i] != sig[i]) { match = 0; break; }
            if (match) return (acpi_rsdp_t*)p;
        }
    }
#endif
    return 0;
}

static const acpi_sdt_header_t* acpi_find_table_via_rsdt(uintptr_t rsdt_addr,
                                                         const char* sig4) {
    if (!rsdt_addr) return 0;
    const acpi_sdt_header_t* rsdt = (const acpi_sdt_header_t*)rsdt_addr;
    uint32_t entries = (rsdt->length - sizeof(*rsdt)) / 4u;
    const uint32_t* arr = (const uint32_t*)((const uint8_t*)rsdt + sizeof(*rsdt));
    for (uint32_t i = 0; i < entries; i++) {
        const acpi_sdt_header_t* hdr = (const acpi_sdt_header_t*)(uintptr_t)arr[i];
        if (hdr && hdr->signature[0] == sig4[0] && hdr->signature[1] == sig4[1] &&
            hdr->signature[2] == sig4[2] && hdr->signature[3] == sig4[3]) return hdr;
    }
    return 0;
}

static const acpi_sdt_header_t* acpi_find_table_via_xsdt(uintptr_t xsdt_addr,
                                                         const char* sig4) {
    if (!xsdt_addr) return 0;
    const acpi_sdt_header_t* xsdt = (const acpi_sdt_header_t*)xsdt_addr;
    uint32_t entries = (xsdt->length - sizeof(*xsdt)) / 8u;
    const uint64_t* arr = (const uint64_t*)((const uint8_t*)xsdt + sizeof(*xsdt));
    for (uint32_t i = 0; i < entries; i++) {
        const acpi_sdt_header_t* hdr = (const acpi_sdt_header_t*)(uintptr_t)arr[i];
        if (hdr && hdr->signature[0] == sig4[0] && hdr->signature[1] == sig4[1] &&
            hdr->signature[2] == sig4[2] && hdr->signature[3] == sig4[3]) return hdr;
    }
    return 0;
}

/*
 * Try the ACPI 5.0 ResetReg path. Returns 1 if a write was issued (caller
 * then halt-loops because reset is in flight); 0 if any prerequisite was
 * missing -- caller falls through to the 8042 reset.
 */
static int acpi_reset_attempt(void) {
    acpi_rsdp_t* rsdp = acpi_find_rsdp();
    if (!rsdp) {
        print("[shell] reboot: ACPI RSDP not found; falling back\n");
        return 0;
    }
    const acpi_sdt_header_t* fadt = 0;
    if (rsdp->revision >= 2u && rsdp->xsdt_address)
        fadt = acpi_find_table_via_xsdt((uintptr_t)rsdp->xsdt_address, "FACP");
    if (!fadt && rsdp->rsdt_address)
        fadt = acpi_find_table_via_rsdt((uintptr_t)rsdp->rsdt_address, "FACP");
    if (!fadt) {
        print("[shell] reboot: ACPI FADT not found; falling back\n");
        return 0;
    }
    /* FADT layout: ACPI 2.0+ adds RESET_REG at offset 116 (GAS) and
     * RESET_VALUE at offset 128 (uint8). The "Flags" field at offset 112
     * tells us whether RESET_REG is supported. */
    const uint8_t* fadt_bytes = (const uint8_t*)fadt;
    if (fadt->length < 129u) {
        print("[shell] reboot: FADT too short for ResetReg; falling back\n");
        return 0;
    }
    uint32_t flags = (uint32_t)fadt_bytes[112]
                   | ((uint32_t)fadt_bytes[113] << 8)
                   | ((uint32_t)fadt_bytes[114] << 16)
                   | ((uint32_t)fadt_bytes[115] << 24);
    if (!(flags & ACPI_FADT_FLAG_RESET_REG_SUP)) {
        print("[shell] reboot: FADT.Flags lacks RESET_REG_SUP; falling back\n");
        return 0;
    }
    acpi_gas_t reset_reg;
    /* Copy out via byte loads to avoid any unaligned trap on hostile FADTs. */
    {
        uint8_t* dst = (uint8_t*)&reset_reg;
        for (size_t i = 0; i < sizeof(reset_reg); i++) dst[i] = fadt_bytes[116 + i];
    }
    uint8_t reset_value = fadt_bytes[128];
    print("[shell] reboot: ACPI RESET_REG attempt\n");

    if (reset_reg.address_space_id == 1u) {       /* system I/O */
        outb((uint16_t)reset_reg.address, reset_value);
    } else if (reset_reg.address_space_id == 0u) { /* system memory */
        *(volatile uint8_t*)(uintptr_t)reset_reg.address = reset_value;
    } else {
        print("[shell] reboot: unsupported ACPI ResetReg space; falling back\n");
        return 0;
    }
    return 1;
}
#endif /* __x86_64__ */

void shell_reboot(void) {
    __asm__ volatile ("cli");
#ifdef __i386__
    /* Original 32-bit path: load an invalid IDT then int3 -> triple fault. */
    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) idt_ptr = {0, 0};
    __asm__ volatile ("lidt %0" : : "m"(idt_ptr));
    __asm__ volatile ("int3\nud2\n");
#else
    /*
     * x64: ACPI 5.0 RESET_REG (via Multiboot2 RSDP), then 8042 pulse,
     * then cli/hlt with a visible failure line.
     */
    if (!acpi_reset_attempt()) {
        print("[shell] reboot: pulsing 8042 keyboard-controller reset (x64 fallback)\n");
        for (int i = 0; i < 16; i++) {
            if ((inb(0x64) & 0x02) == 0) break;
            (void)inb(0x60);
        }
        outb(0x64, 0xFE);
    }
    print("[shell] reboot: reset did not take effect; halted\n");
#endif
    while (1) __asm__ volatile ("hlt");
}

static int shell_is_live(void) {
    return !fs_is_persistent();
}

static void list_games(void) {
    if (shell_is_live()) {
        print("Start menu -> Games: Minesweeper, CubeDip, SnakeGame, DoomRay (GooberC)\n");
        print("doom.exe (native GooberDoom)\n");
        return;
    }
    print("snakeGame.exe\n");
    print("cubeDip.exe\n");
    print("pong.exe\n");
    print("doom.exe\n");
}

static int parse_hex(const char* s, uint32_t* out) {
    uint32_t val = 0;
    if (!s || *s == '\0') return -1;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (*s == '\0') return -1;
    while (*s) {
        char c = *s++;
        uint8_t d;
        if (c >= '0' && c <= '9') d = (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint8_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint8_t)(c - 'A' + 10);
        else return -1;
        val = (val << 4) | d;
    }
    *out = val;
    return 0;
}

static void print_hex_u32(uint32_t value, int pad_to_eight) {
    char out[9];
    int started = pad_to_eight ? 1 : 0;
    int pos = 0;

    for (int shift = 28; shift >= 0; shift -= 4) {
        uint8_t nibble = (uint8_t)((value >> shift) & 0xF);
        if (!started && nibble == 0 && shift != 0) continue;
        started = 1;
        out[pos++] = (char)(nibble < 10 ? ('0' + nibble) : ('a' + (nibble - 10)));
    }

    if (pos == 0) out[pos++] = '0';
    out[pos] = '\0';
    print(out);
}

static void print_u64(uint64_t value) {
    uint32_t high = (uint32_t)(value >> 32);
    print("0x");
    if (high != 0) {
        print_hex_u32(high, 0);
        print_hex_u32((uint32_t)(value & 0xFFFFFFFFU), 1);
        return;
    }
    print_hex_u32((uint32_t)(value & 0xFFFFFFFFU), 0);
}

static uint16_t read_le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int parse_u32_token(const char* s, uint32_t* out) {
    if (!s || !*s || !out) return -1;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return parse_hex(s, out);
    *out = (uint32_t)atoi(s);
    return 0;
}

/*
 * Phase 3f: the storage / install / disk command helpers reach into the
 * drivers/storage stack (storage.c / sdhci.c on both arches; bios_disk.c
 * is real-mode-only and stays x86-gated). The 3e shell_deferred_3f shim
 * is gone; both arches see the same command bodies. The bios_disk path
 * is preserved x86-only via #ifdef inside the install flow if needed.
 */
static const storage_device_info_t* get_storage_device_by_id(int device_index) {
    storage_scan();
    return storage_get(device_index);
}

static size_t install_image_size_bytes(void) {
#if EMBED_INSTALL_ISO
    return (size_t)(&INSTALL_IMAGE_END - &INSTALL_IMAGE_START);
#else
    return 0;
#endif
}

static size_t install_image_sector_count(void) {
    size_t bytes = install_image_size_bytes();
    return bytes == 0 ? 0 : (bytes + 511) / 512;
}

static int install_image_available(void) {
#if EMBED_INSTALL_ISO
    return install_image_size_bytes() > 0;
#else
    return 0;
#endif
}

static void list_devices() {
    storage_scan();
    int count = storage_count();
    print("devices: build=2026-07-13-byt-fastep0\n");
    storage_print_pci_inventory();

    /* USB class devices are not the PCI controller line — report attach state. */
    print("USB peripherals:\n");
    {
        char buf[16];
        print("  HID pointer: ");
        print(usb_has_pointer_device() ? "yes" : "no");
        print(usb_has_touchpad_device() ? " (touchpad)" : "");
        print("\n");
        print("  HID keyboard: ");
        print(usb_has_keyboard_device() ? "yes" : "no");
        print("\n");
        print("  MSC stick: ");
        if (usb_msc_is_attached()) {
            print("yes port=");
            itoa(usb_msc_attached_port(), buf, 10);
            print(buf);
            print(" storage_index=");
            itoa(usb_msc_storage_index(), buf, 10);
            print(buf);
            print("\n");
        } else {
            print("no (needs enum + BOT; sticks appear after mouse path works)\n");
        }
    }
    usb_host_print_usb2_route(print);
    touchpad_print_status(print);

    if (count <= 0) {
        print("No storage hardware detected in storage table.\n");
        print("If PCI inventory above shows 8086:f14 or class 8/5, report that line.\n");
        return;
    }
    print("Storage hardware:\n");
    for (int i = 0; i < count; i++) {
        const storage_device_info_t* d = storage_get(i);
        if (!d || !d->present) continue;
        print("  [");
        char buf[16];
        itoa(i, buf, 10);
        print(buf);
        print("] ");
        print(storage_type_name(d->type));
        print(" on ");
        print(storage_bus_name(d->bus));
        print(" (");
        print(d->location);
        print(")");
        if (d->model[0] != '\0') {
            print(" - ");
            print(d->model);
        }
        print(" [");
        print(storage_backend_name(d->backend));
        print(", ");
        print(storage_install_state_name(d->install_state));
        print("]");
        if (d->selectable) {
            print(" [install target]");
        }
        if (d->vendor_id) {
            print(" id=");
            itoa((int)d->vendor_id, buf, 16); print(buf); print(":");
            itoa((int)d->device_id, buf, 16); print(buf);
        }
        print("\n");
    }
}

static void list_pending_install_paths(void) {
    int count = storage_count();
    int printed = 0;

    for (int i = 0; i < count; i++) {
        const storage_device_info_t* d = storage_get(i);
        if (!d || !d->present || d->install_state == STORAGE_INSTALL_STATE_READY) continue;
        if (!printed) {
            print("Blocked storage paths:\n");
            printed = 1;
        }
        print("  ");
        print(storage_type_name(d->type));
        print(" - ");
        print(d->location);
        print(" [");
        print(storage_backend_name(d->backend));
        print("] ");
        print(storage_install_state_reason(d));
        print("\n");
    }
}

/* Rescan storage and lazily bring up SDHCI/eMMC (safe at boot with storage=ata). */
static void install_storage_refresh(void) {
    int i;
    int n;
    print("install: storage refresh build=bsw8-20260719-sdr12\n");
    storage_scan();
    print("install: probing SDHCI/eMMC...\n");
    storage_probe_sdhci();
    n = storage_count();
    for (i = 0; i < n; i++) {
        const storage_device_info_t* d = storage_get(i);
        char buf[16];
        if (!d || !d->present || d->backend != STORAGE_BACKEND_SDHCI) continue;
        if (d->install_state == STORAGE_INSTALL_STATE_READY) continue;
        print("install: eMMC probe incomplete at ");
        print(d->location);
        print(" (step ");
        itoa((int)d->init_step, buf, 10);
        print(buf);
        print("): ");
        print(storage_install_state_reason(d));
        print("\n");
    }
}

static void list_install_targets(void) {
    int count;
    install_storage_refresh();
    count = storage_target_count();

    if (count <= 0) {
        print("No installable storage targets detected in-kernel.\n");
        list_pending_install_paths();
        return;
    }

    print("Install targets:\n");
    for (int i = 0; i < count; i++) {
        const storage_device_info_t* d = storage_get_target(i);
        char buf[16];
        if (!d) continue;

        print("  [");
        itoa(i, buf, 10);
        print(buf);
        print("] ");
        print(storage_type_name(d->type));
        print(" - ");
        print(d->model[0] ? d->model : "Unnamed device");
        print(" (");
        print(d->location);
        print(") [");
        print(storage_backend_name(d->backend));
        print("]\n");
    }
    list_pending_install_paths();
}

static void print_install_help(void) {
    print("install commands:\n");
    print("  install list\n");
    print("  install info <target-id>\n");
    print("  install fat32 <target-id> YES MBR|GPT\n");
    print("  install memory\n");
    print("  install write <target-id> YES\n");
    print("  install host\n");
    print("\n");
    print("  install fat32  MBR = BIOS/legacy, GPT = UEFI\n");
    print("                 Reboot from the target disk when done.\n");
    print("  install memory Memory-only root (not persistent).\n");
}

static void print_storage_device_info(const char* heading, const storage_device_info_t* d, int display_index) {
    char buf[16];

    if (!d) {
        return;
    }

    print(heading);
    print(" [");
    itoa(display_index, buf, 10);
    print(buf);
    print("]\n");
    print("  Type: ");
    print(storage_type_name(d->type));
    print("\n");
    print("  Backend: ");
    print(storage_backend_name(d->backend));
    print("\n");
    print("  Install state: ");
    print(storage_install_state_name(d->install_state));
    print("\n");
    print("  Bus: ");
    print(storage_bus_name(d->bus));
    print("\n");
    if (d->bus == STORAGE_BUS_PCI) {
        print("  PCI: ");
        itoa((int)d->pci_bus, buf, 10);
        print(buf);
        print(":");
        itoa((int)d->pci_slot, buf, 10);
        print(buf);
        print(":");
        itoa((int)d->pci_func, buf, 10);
        print(buf);
        print("\n");
        print("  BAR0: 0x");
        print_hex_u32(d->bar0, 0);
        print("\n");
        print("  Class: 0x");
        print_hex_u32(d->class_code, 0);
        print(" / Subclass: 0x");
        print_hex_u32(d->sub_class, 0);
        print(" / ProgIF: 0x");
        print_hex_u32(d->prog_if, 0);
        print("\n");
    }
    print("  Location: ");
    print(d->location);
    print("\n");
    print("  Model: ");
    print(d->model[0] ? d->model : "Unknown");
    print("\n");
    print("  Sector size: ");
    itoa((int)d->sector_size, buf, 10);
    print(buf);
    print("\n");
    print("  Sectors: ");
    print_u64(d->sectors);
    print("\n");
    print("  Reason: ");
    print(storage_install_state_reason(d));
    print("\n");
    if (d->backend == STORAGE_BACKEND_SDHCI) {
        print("  Init step: ");
        itoa((int)d->init_step, buf, 10);
        print(buf);
        print("\n");
        print("  Last status: 0x");
        print_hex_u32(d->last_status, 0);
        print("\n");
    }
    print("  Direct kernel install: ");
    if (!d->direct_install_supported) {
        print("not supported on this device path\n");
    } else if (!install_image_available()) {
        print("installer image not embedded in this build\n");
    } else {
        print("ready\n");
    }
    print("  Install image sectors: ");
    itoa((int)install_image_sector_count(), buf, 10);
    print(buf);
    print("\n");
}

static void print_install_target_info(int target_index) {
    const storage_device_info_t* d;
    install_storage_refresh();
    d = storage_get_target(target_index);
    if (!d) {
        print("install: target not found\n");
        return;
    }
    print_storage_device_info("Target", d, target_index);
}

static void print_disk_device_info(int device_index) {
    const storage_device_info_t* d = get_storage_device_by_id(device_index);
    if (!d) {
        print("disk: device not found\n");
        return;
    }
    print_storage_device_info("Device", d, device_index);
}

static void print_host_install_help(void) {
    print("Host-side install (recommended for first boot test):\n");
    print("  1. Build: ./build.sh x86\n");
    print("  2. Raw disk image (VirtualBox/QEMU):\n");
    print("     ./scripts/make-installed-disk.sh build/GooberOS-installed.img\n");
    print("  3. Or mount FAT32 + grub-install:\n");
    print("     ./build.sh list-devices\n");
    print("     ./build.sh install --device /dev/sdX --mount /mnt/goober\n");
    print("\n");
    print("In-OS install (inside live ISO):\n");
    print("  install list\n");
    print("  install fat32 <target-id> YES MBR|GPT\n");
    print("  Reboot from the target disk (not the ISO).\n");
}

static void print_disk_help(void) {
    print("disk commands:\n");
    print("  disk info <device-id>\n");
    print("  disk partitions <device-id>\n");
    print("  disk probe-write <device-id> YES\n");
    print("  disk wipe-table <device-id> YES\n");
    print("  disk zero <device-id> <start-lba> <count> YES\n");
    print("\n");
    print("Device IDs come from the `devices` command, not `install list`.\n");
}

static void disk_show_partitions(int device_index) {
    const storage_device_info_t* d = get_storage_device_by_id(device_index);
    if (!d) {
        print("disk: device not found\n");
        return;
    }
    partition_print_table(d, print);
}

static void print_mount_status(void) {
    const boot_config_t* cfg = boot_get_config();
    print("Filesystem backend: ");
    print(fs_backend_name());
    print("\nMount: ");
    print(fs_mount_description());
    print("\nPersistent: ");
    print(fs_is_persistent() ? "yes\n" : "no (live memfs)\n");
    print("gooberos.root=");
    print((cfg && cfg->root[0]) ? cfg->root : "live");
    print("\n");
}

static void disk_zero_range(const storage_device_info_t* d, uint32_t start_lba, uint32_t count) {
    uint8_t zero_sector[512];
    char buf[16];

    memset(zero_sector, 0, sizeof(zero_sector));
    if (!d) {
        print("disk: device not found\n");
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (storage_write_sector(d, start_lba + i, zero_sector) != 0) {
            print("disk: write failed at LBA ");
            itoa((int)(start_lba + i), buf, 10);
            print(buf);
            print("\n");
            return;
        }
    }
    if (storage_flush(d) != 0) {
        print("disk: flush failed after zeroing\n");
        return;
    }
    print("disk: zero complete\n");
}

static void disk_probe_write(int device_index) {
    const storage_device_info_t* d = get_storage_device_by_id(device_index);
    uint8_t original[512];
    uint8_t modified[512];
    uint8_t verify[512];
    uint32_t lba = 1;

    if (!d) {
        print("disk: device not found\n");
        return;
    }
    if (d->sectors != 0 && d->sectors <= 1) lba = 0;

    if (storage_read_sector(d, lba, original) != 0) {
        print("disk: failed to read probe sector\n");
        return;
    }

    memcpy(modified, original, sizeof(modified));
    modified[0] ^= 0x5A;
    modified[1] ^= 0xA5;
    modified[2] ^= 0x3C;
    modified[3] ^= 0xC3;

    if (storage_write_sector(d, lba, modified) != 0) {
        print("disk: probe write failed\n");
        return;
    }
    if (storage_flush(d) != 0) {
        print("disk: probe flush failed\n");
        return;
    }
    if (storage_read_sector(d, lba, verify) != 0) {
        print("disk: probe verify read failed\n");
        return;
    }
    if (verify[0] != modified[0] || verify[1] != modified[1] ||
        verify[2] != modified[2] || verify[3] != modified[3]) {
        print("disk: probe verify mismatch\n");
    } else {
        print("disk: probe write verified\n");
    }

    if (storage_write_sector(d, lba, original) != 0 || storage_flush(d) != 0) {
        print("disk: failed to restore original probe sector\n");
        return;
    }
    print("disk: original probe sector restored\n");
}

static void disk_wipe_table(int device_index) {
    const storage_device_info_t* d = get_storage_device_by_id(device_index);
    uint32_t head_count;

    if (!d) {
        print("disk: device not found\n");
        return;
    }
    head_count = (d->sectors != 0 && d->sectors < 34) ? (uint32_t)d->sectors : 34U;
    disk_zero_range(d, 0, head_count);
    if (d->sectors > 34) {
        uint32_t tail_count = d->sectors > 33 ? 33U : (uint32_t)d->sectors;
        disk_zero_range(d, (uint32_t)(d->sectors - tail_count), tail_count);
    }
}

static int install_parse_style(const char* s, install_partition_style_t* out) {
    char a, b, c, d;
    if (!s || !out || !s[0]) return -1;
    a = s[0]; if (a >= 'a' && a <= 'z') a = (char)(a - 32);
    b = s[1]; if (b >= 'a' && b <= 'z') b = (char)(b - 32);
    c = s[2]; if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    d = s[3]; if (d >= 'a' && d <= 'z') d = (char)(d - 32);

    if (a == 'M' && b == 'B' && c == 'R' && d == '\0') {
        *out = INSTALL_STYLE_MBR;
        return 0;
    }
    if (a == 'G' && b == 'P' && c == 'T' && d == '\0') {
        *out = INSTALL_STYLE_GPT;
        return 0;
    }
    return -1;
}

static void install_fat32_target(int target_index, install_partition_style_t style) {
    const storage_device_info_t* d;

    install_storage_refresh();
    d = storage_get_target(target_index);
    if (!d) {
        print("install: target not found (see `install list`)\n");
        return;
    }
    install_fat32_to_device(d, style);
}

static void install_write_target(int target_index) {
    const storage_device_info_t* d;
    char buf[16];
    (void)buf;  /* used only when EMBED_INSTALL_ISO=1 */

    storage_scan();
    d = storage_get_target(target_index);
    if (!d) {
        print("install: target not found\n");
        return;
    }
    if (!d->direct_install_supported) {
        print("install: selected target does not support direct kernel install\n");
        return;
    }
    if (!install_image_available()) {
        print("install: installer image not embedded in this build.\n");
        print("Rebuild with EMBED_INSTALL_ISO=1 to enable direct disk installs.\n");
        return;
    }

#if EMBED_INSTALL_ISO
    {
        unsigned char* image = &INSTALL_IMAGE_START;
        size_t image_bytes = install_image_size_bytes();
        size_t image_sectors = install_image_sector_count();
        uint8_t sector[512];

        if (image_sectors == 0) {
            print("install: embedded image is empty\n");
            return;
        }
        if (d->sectors != 0 && d->sectors < image_sectors) {
            print("install: target is smaller than installer image\n");
            return;
        }

        print("install: writing hybrid boot image to target [");
        itoa(target_index, buf, 10);
        print(buf);
        print("] ");
        print(d->model[0] ? d->model : "Unnamed device");
        print("\n");

        for (size_t sector_index = 0; sector_index < image_sectors; sector_index++) {
            size_t offset = sector_index * 512;
            size_t remaining = image_bytes - offset;
            size_t copy = remaining >= 512 ? 512 : remaining;

            memset(sector, 0, sizeof(sector));
            memcpy(sector, image + offset, copy);

            if (storage_write_sector(d, (uint32_t)sector_index, sector) != 0) {
                print("install: write failed at sector ");
                itoa((int)sector_index, buf, 10);
                print(buf);
                print("\n");
                return;
            }

            if ((sector_index & 127U) == 0) {
                print(".");
            }
        }

        if (storage_flush(d) != 0) {
            print("\ninstall: disk flush failed\n");
            return;
        }

        print("\ninstall: complete\n");
#ifdef __x86_64__
        print("install: the target should now boot on UEFI-capable x64 systems.\n");
#else
        print("install: the target should now boot in BIOS/legacy mode.\n");
#endif
    }
#endif
}

void execute_command(const char* cmd) {
    int redirected = shell_output_redirect_active();
    if (!cmd) return;

    if (!redirected) {
        restore_prev_cursor_cell();
        move_cursor(cursor_row, cursor_col);
        cursor_enabled = 0;
    }

    if (cmd[0] == '\0') {
        if (!redirected) {
            cursor_enabled = 1;
            draw_cursor();
        }
        return;
    }
    {
        size_t len = 0;
        while (cmd[len] && len < INPUT_BUFFER_SIZE) len++;
        if (len > 0) {
            for (size_t i = 0; i < len && i < INPUT_BUFFER_SIZE - 1; i++)
                history[history_next][i] = cmd[i];
            history[history_next][(len < INPUT_BUFFER_SIZE - 1 ? len : INPUT_BUFFER_SIZE - 1)] = '\0';
            history_next = (history_next + 1) % HISTORY_SIZE;
            if (history_count < HISTORY_SIZE) history_count++;
        }
        history_nav_offset = -1;
    }

    if (!strcmp_local(cmd, "help")) {
        if (shell_is_live()) {
            print("Available commands (live memfs):\nhelp\ncls\necho\nls\ncd\nexit\nreboot\ngames\ntaskview\ndevices\nlspci\ndisplay\nboot\nlogs\ndriverlog\ninstall\ndisk\npartitions\nmount\nedit\nnew\nwrite\nmkdir\nrename\ndel\nrmdir\nread\ngui\ncolor\nrun\nrundos\ngooberc\n");
            print("(umount/sync/install memory/legacy VGA games disabled on live)\n");
        } else {
            print("Available commands:\nhelp\ncls\necho\nls\ncd\nexit\nreboot\ngames\ntaskview\ndevices\nlspci\ndisplay\nboot\nlogs\ndriverlog\ninstall\ndisk\npartitions\nmount\numount\nsync\nedit\nnew\nwrite\nmkdir\nrename\ndel\nrmdir\nread\ngui\ncolor\nrun\nrundos\ngooberc\n");
        }
    } else if (!strncmp_local(cmd, "run ", 4)) {
        const char* path = cmd + 4;
        while (*path == ' ') path++;
        if (!*path) print("run: usage: run <path.gob>\n");
        else if (gob_exec(path) != 0) print("run: failed\n");
    } else if (!strncmp_local(cmd, "rundos", 6) && (cmd[6] == '\0' || cmd[6] == ' ')) {
        const char* path = cmd + 6;
        while (*path == ' ') path++;
        {
            extern int dos_exec(const char* path);
            if (dos_exec(*path ? path : NULL) != 0) print("rundos: failed\n");
        }
    } else if (!strncmp_local(cmd, "gooberc ", 8)) {
        /* gooberc <src.gc> -o <out.gob> */
        const char* arg = cmd + 8;
        char src[64] = {0};
        char out[64] = {0};
        size_t i = 0;
        while (arg[i] && arg[i] != ' ' && i + 1 < sizeof(src)) {
            src[i] = arg[i];
            i++;
        }
        src[i] = '\0';
        while (arg[i] == ' ') i++;
        if (arg[i] == '-' && arg[i + 1] == 'o') {
            i += 2;
            while (arg[i] == ' ') i++;
            size_t j = 0;
            while (arg[i] && arg[i] != ' ' && j + 1 < sizeof(out)) {
                out[j++] = arg[i++];
            }
            out[j] = '\0';
        }
        if (!src[0] || !out[0])
            print("gooberc: usage: gooberc <src.gc> -o <out.gob>\n");
        else if (gooberc_compile(src, out) != 0)
            print("gooberc: compile failed\n");
    } else if (!strcmp_local(cmd, "boot")) {
        const boot_config_t* cfg = boot_get_config();
        const boot_smart_profile_t* smart = boot_smart_profile();
        print("Boot request: ");
        print(cfg->boot);
        print("\n");
        print("Display: ");
        print(cfg->display);
        print("\n");
        print("Storage: ");
        print(cfg->storage[0] ? cfg->storage : "(default)");
        print("\n");
        if (smart && smart->active) {
            print("Smart profile: ");
            print(smart->reason);
            print("\n");
        } else {
            print("Smart profile: (inactive)\n");
        }
        print("Cmdline: ");
        print(cfg->cmdline[0] ? cfg->cmdline : "(empty)");
        print("\n");
    } else if (!strcmp_local(cmd, "display") || !strcmp_local(cmd, "xrandr")) {
        const display_mode_info_t* mode = display_get_mode();
        char buf[16];
        int i, n;
        const boot_smart_profile_t* smart = boot_smart_profile();
        if (smart && smart->active) {
            print("Smart boot: ");
            print(smart->reason);
            print("\n");
        }
        print("Display driver: ");
        print(display_driver_name(mode->driver));
        print("\n");
        if (mode->available) {
            print("Mode: ");
            itoa((int)mode->width, buf, 10); print(buf); print("x");
            itoa((int)mode->height, buf, 10); print(buf); print("x");
            itoa((int)mode->bpp, buf, 10); print(buf);
            print(" (");
            print(display_format_name(mode->format));
            print(")\n");
        }
        if (display_connectors_count() == 0) {
            const display_mode_info_t* m = display_get_mode();
            if (intel_gfx_is_bay_trail_class() && m && m->available) {
                display_connectors_stub_firmware_panel(m->width, m->height);
            } else {
                display_connectors_scan_ex(0);
                if (m && m->available)
                    display_connector_add_simplefb(m->width, m->height, "Active-FB");
            }
        }
        n = display_connectors_count();
        print("Connectors:\n");
        for (i = 0; i < n; i++) {
            const display_connector_t* c = display_connector_get(i);
            if (!c) continue;
            print("  ");
            print(c->name);
            print(" ");
            print(display_connector_status_name(c->status));
            if (c->preferred_width && c->preferred_height) {
                print(" ");
                itoa((int)c->preferred_width, buf, 10); print(buf); print("x");
                itoa((int)c->preferred_height, buf, 10); print(buf);
            }
            if (c->monitor_name[0]) {
                print(" \"");
                print(c->monitor_name);
                print("\"");
            }
            print("\n");
        }
    } else if (!strcmp_local(cmd, "gui")) {
        if (redirected) {
            print("Already in GUI mode.\n");
        } else {
            gui_run();
            prompt();
        }
    } else if (!strncmp_local(cmd, "color ", 6)) {
        const char* args = cmd + 6;
        uint32_t val;
        if (parse_hex(args, &val) == 0) {
            current_color = (uint8_t)val;
            print("Color changed.\n");
        } else {
            print("Usage: color <hex>\nExample: color 0A (Green on Black)\n");
        }
    } else if (!strcmp_local(cmd, "cls")) {
        shell_clear_display();
        cursor_row = 0;
        cursor_col = 0;
    } else if (!strncmp_local(cmd, "echo ", 5)) {
        print(cmd + 5);
        print("\n");
    } else if (!strncmp_local(cmd, "mount ", 6)) {
        const char* arg = cmd + 6;
        int dev_idx = -1;
        int part_idx = 0;
        const char* colon = arg;
        while (*colon && *colon != ':') colon++;
        if (*colon == ':') {
            char dev_buf[12];
            size_t n = (size_t)(colon - arg);
            size_t i;
            if (n == 0 || n >= sizeof(dev_buf)) {
                print("Usage: mount N:P   (device index : partition index)\n");
            } else {
                for (i = 0; i < n; i++) dev_buf[i] = arg[i];
                dev_buf[n] = '\0';
                dev_idx = atoi(dev_buf);
                part_idx = atoi(colon + 1);
                if (fat32_mount_device_loose(dev_idx, part_idx) == 0) {
                    print("Mounted ");
                    print(fat32_mount_description());
                    print("\n");
                } else {
                    print("mount failed (need READY device with FAT32).\n");
                }
            }
        } else {
            print("Usage: mount N:P   (or 'mount' for status)\n");
        }
    } else if (!strcmp_local(cmd, "mount")) {
        print_mount_status();
    } else if (!strcmp_local(cmd, "umount") || !strcmp_local(cmd, "unmount")) {
        if (shell_is_live() && !fat32_is_mounted()) {
            print("umount: not available on live memfs\n");
        } else if (!fat32_is_mounted()) {
            print("Nothing mounted.\n");
        } else {
            fat32_unmount();
            print("Unmounted.\n");
        }
    } else if (!strcmp_local(cmd, "sync")) {
        if (shell_is_live() && !fat32_is_mounted()) {
            print("sync: no persistent filesystem (live memfs)\n");
        } else if (fs_sync() == 0) {
            print("Filesystem synced.\n");
        } else {
            print("sync failed.\n");
        }
    } else if (!strcmp_local(cmd, "ls")) {
        fs_list();
    } else if (!strncmp_local(cmd, "cd ", 3)) {
        const char* path = cmd + 3;
        if (!strcmp_local(path, "..")) {
            if (fs_cd_up() == 0) print("Moved up one directory\n");
            else print("Already at root directory\n");
        } else if (path[0] == '\0') {
            print("cd: Directory required\n");
        } else {
            if (fs_change_dir(path) == 0) {
                print("Changed directory to ");
                print(path);
                print("\n");
            } else {
                print("cd: Directory not found: ");
                print(path);
                print("\n");
            }
        }
    } else if (!strncmp_local(cmd, "new ", 4)) {
        const char* filename = cmd + 4;
        if (filename[0] == '\0') {
            print("new: Filename required\n");
        } else {
            if (fs_create(filename) == 0) {
                print("Created ");
                print(filename);
                print("\n");
            } else {
                print("new: Failed to create ");
                print(filename);
                print("\n");
            }
        }
    } else if (!strncmp_local(cmd, "write ", 6)) {
        const char* rest = cmd + 6;
        size_t i = 0;
        while (rest[i] && rest[i] != ' ') i++;
        char filename[INPUT_BUFFER_SIZE] = {0};
        size_t fn_len = i < INPUT_BUFFER_SIZE - 1 ? i : INPUT_BUFFER_SIZE - 1;
        for (size_t j = 0; j < fn_len; j++) filename[j] = rest[j];
        filename[fn_len] = '\0';
        const char* content = rest + i;
        while (*content == ' ') content++;
        if (filename[0] == '\0') {
            print("write: Filename required\n");
        } else {
            if (fs_write(filename, (const uint8_t*)content, strlen(content)) == 0) {
                print("Wrote ");
                print(filename);
                print("\n");
            } else {
                print("write: Failed to write ");
                print(filename);
                print("\n");
            }
        }
    } else if (!strncmp_local(cmd, "mkdir ", 6)) {
        const char* arg = cmd + 6;
        size_t len = 0;
        while (arg[len] != '\0') len++;
        int trailing = (len > 0 && arg[len - 1] == '/');
        char dirname[INPUT_BUFFER_SIZE] = {0};
        size_t copy_len = trailing ? len - 1 : len;
        if (copy_len >= INPUT_BUFFER_SIZE) copy_len = INPUT_BUFFER_SIZE - 1;
        for (size_t j = 0; j < copy_len; j++) dirname[j] = arg[j];
        dirname[copy_len] = '\0';
        if (dirname[0] == '\0') {
            print("mkdir: Directory name required\n");
        } else {
            if (fs_create_dir(dirname) == 0) {
                print("Created ");
                print(dirname);
                print(" directory\n");
            } else {
                print("mkdir: Failed to create ");
                print(dirname);
                print("\n");
            }
        }
    } else if (!strncmp_local(cmd, "rename ", 7)) {
        const char* arg = cmd + 7;
        char old_name[INPUT_BUFFER_SIZE] = {0};
        char new_name[INPUT_BUFFER_SIZE] = {0};
        size_t i = 0;
        size_t j = 0;
        while (arg[i] == ' ') i++;
        while (arg[i] && arg[i] != ' ' && j < sizeof(old_name) - 1)
            old_name[j++] = arg[i++];
        old_name[j] = '\0';
        while (arg[i] == ' ') i++;
        j = 0;
        while (arg[i] && arg[i] != ' ' && j < sizeof(new_name) - 1)
            new_name[j++] = arg[i++];
        new_name[j] = '\0';
        if (old_name[0] == '\0' || new_name[0] == '\0') {
            print("rename: usage rename <old-name> <new-name>\n");
        } else if (fs_rename(old_name, new_name) == 0) {
            print("Renamed ");
            print(old_name);
            print(" to ");
            print(new_name);
            print("\n");
        } else {
            print("rename: failed to rename ");
            print(old_name);
            print("\n");
        }
    } else if (!strncmp_local(cmd, "del ", 4)) {
        const char* target = cmd + 4;
        size_t len = 0;
        while (target[len] != '\0') len++;
        if (len > 1 && target[len - 1] == '/') {
            char dirname[INPUT_BUFFER_SIZE];
            size_t copy_len = len - 1 < INPUT_BUFFER_SIZE - 1 ? len - 1 : INPUT_BUFFER_SIZE - 1;
            for (size_t i = 0; i < copy_len; i++) dirname[i] = target[i];
            dirname[copy_len] = '\0';
            if (fs_delete_dir(dirname) == 0) {
                print("(deleted ");
                print(dirname);
                print(" directory)\n");
            } else {
                print("del: Directory not found or failed: ");
                print(dirname);
                print("\n");
            }
        } else if (target[0] == '\0') {
            print("del: Filename or directory required\n");
        } else {
            if (fs_delete(target) == 0) {
                print("Erased ");
                print(target);
                print("\n");
            } else {
                print("del: File not found or failed: ");
                print(target);
                print("\n");
            }
        }
    } else if (!strncmp_local(cmd, "rmdir ", 6)) {
        const char* dirname = cmd + 6;
        if (dirname[0] == '\0') {
            print("rmdir: Directory name required\n");
        } else {
            if (fs_delete_dir(dirname) == 0) {
                print("Removed directory ");
                print(dirname);
                print("\n");
            } else {
                print("rmdir: failed to remove ");
                print(dirname);
                print("\n");
            }
        }
    } else if (!strncmp_local(cmd, "read ", 5)) {
        const char* filename = cmd + 5;
        if (filename[0] == '\0') {
            print("read: Filename required\n");
        } else {
            FileHandle* fh = fs_open(filename);
            if (!fh) {
                print("read: File not found: ");
                print(filename);
                print("\n");
            } else {
                uint8_t buffer[128];
                size_t n;
                while ((n = fs_read(fh, buffer, sizeof(buffer) - 1)) > 0) {
                    buffer[n] = 0;
                    print((const char*)buffer);
                }
                fs_close(fh);
                print("\n");
            }
        }
    } else if (!strcmp_local(cmd, "devices")) {
        list_devices();
    } else if (!strcmp_local(cmd, "lspci") || !strcmp_local(cmd, "pci")) {
        storage_print_pci_inventory();
    } else if (!strcmp_local(cmd, "logs") || !strcmp_local(cmd, "driverlog")) {
        driver_log_sync_desktop_file();
        driver_log_dump(print);
    } else if (!strcmp_local(cmd, "disk")) {
        print_disk_help();
    } else if (!strcmp_local(cmd, "partitions")) {
        print("partitions: device id required\n");
    } else if (!strncmp_local(cmd, "partitions ", 11)) {
        disk_show_partitions(atoi(cmd + 11));
    } else if (!strcmp_local(cmd, "disk info")) {
        print("disk: device id required\n");
    } else if (!strncmp_local(cmd, "disk info ", 10)) {
        print_disk_device_info(atoi(cmd + 10));
    } else if (!strcmp_local(cmd, "disk partitions")) {
        print("disk: device id required\n");
    } else if (!strncmp_local(cmd, "disk partitions ", 16)) {
        disk_show_partitions(atoi(cmd + 16));
    } else if (!strncmp_local(cmd, "disk probe-write ", 17)) {
        const char* arg = cmd + 17;
        char device_buf[16] = {0};
        size_t i = 0;
        while (arg[i] && arg[i] != ' ' && i < sizeof(device_buf) - 1) {
            device_buf[i] = arg[i];
            i++;
        }
        device_buf[i] = '\0';
        while (arg[i] == ' ') i++;
        if (device_buf[0] == '\0') {
            print("disk: device id required\n");
        } else if (!(arg[i] == 'Y' && arg[i + 1] == 'E' && arg[i + 2] == 'S' && arg[i + 3] == '\0')) {
            print("disk: add YES to confirm probe-write\n");
        } else {
            disk_probe_write(atoi(device_buf));
        }
    } else if (!strncmp_local(cmd, "disk wipe-table ", 16)) {
        const char* arg = cmd + 16;
        char device_buf[16] = {0};
        size_t i = 0;
        while (arg[i] && arg[i] != ' ' && i < sizeof(device_buf) - 1) {
            device_buf[i] = arg[i];
            i++;
        }
        device_buf[i] = '\0';
        while (arg[i] == ' ') i++;
        if (device_buf[0] == '\0') {
            print("disk: device id required\n");
        } else if (!(arg[i] == 'Y' && arg[i + 1] == 'E' && arg[i + 2] == 'S' && arg[i + 3] == '\0')) {
            print("disk: add YES to confirm partition-table wipe\n");
        } else {
            disk_wipe_table(atoi(device_buf));
        }
    } else if (!strncmp_local(cmd, "disk zero ", 10)) {
        const char* arg = cmd + 10;
        char device_buf[16] = {0};
        char start_buf[16] = {0};
        char count_buf[16] = {0};
        uint32_t start_lba = 0;
        uint32_t count = 0;
        size_t i = 0;
        size_t j = 0;

        while (arg[i] && arg[i] != ' ' && i < sizeof(device_buf) - 1) {
            device_buf[i] = arg[i];
            i++;
        }
        device_buf[i] = '\0';
        while (arg[i] == ' ') i++;
        while (arg[i] && arg[i] != ' ' && j < sizeof(start_buf) - 1) {
            start_buf[j++] = arg[i++];
        }
        start_buf[j] = '\0';
        while (arg[i] == ' ') i++;
        j = 0;
        while (arg[i] && arg[i] != ' ' && j < sizeof(count_buf) - 1) {
            count_buf[j++] = arg[i++];
        }
        count_buf[j] = '\0';
        while (arg[i] == ' ') i++;

        if (device_buf[0] == '\0' || start_buf[0] == '\0' || count_buf[0] == '\0') {
            print("disk: usage disk zero <device-id> <start-lba> <count> YES\n");
        } else if (!(arg[i] == 'Y' && arg[i + 1] == 'E' && arg[i + 2] == 'S' && arg[i + 3] == '\0')) {
            print("disk: add YES to confirm zero write\n");
        } else if (parse_u32_token(start_buf, &start_lba) != 0 || parse_u32_token(count_buf, &count) != 0 || count == 0) {
            print("disk: invalid start/count\n");
        } else {
            disk_zero_range(get_storage_device_by_id(atoi(device_buf)), start_lba, count);
        }
    } else if (!strcmp_local(cmd, "install")) {
        print_install_help();
    } else if (!strcmp_local(cmd, "install list")) {
        list_install_targets();
    } else if (!strcmp_local(cmd, "install info")) {
        print("install: target id required\n");
    } else if (!strcmp_local(cmd, "install host")) {
        print_host_install_help();
    } else if (!strcmp_local(cmd, "install memory")) {
        if (shell_is_live()) {
            print("install memory: already running on live memfs\n");
        } else if (install_memory_only() == 0) {
            print("install memory: done\n");
        } else {
            print("install memory: failed\n");
        }
    } else if (!strncmp_local(cmd, "install info ", 13)) {
        const char* arg = cmd + 13;
        if (*arg == '\0') {
            print("install: target id required\n");
        } else {
            print_install_target_info(atoi(arg));
        }
    } else if (!strncmp_local(cmd, "install write ", 14)) {
        const char* arg = cmd + 14;
        char target_buf[16] = {0};
        size_t i = 0;

        while (arg[i] && arg[i] != ' ' && i < sizeof(target_buf) - 1) {
            target_buf[i] = arg[i];
            i++;
        }
        target_buf[i] = '\0';
        while (arg[i] == ' ') i++;

        if (target_buf[0] == '\0') {
            print("install: target id required\n");
        } else if (!(arg[i] == 'Y' && arg[i + 1] == 'E' && arg[i + 2] == 'S' && arg[i + 3] == '\0')) {
            print("install: add YES to confirm destructive disk write\n");
        } else {
            install_write_target(atoi(target_buf));
        }
    } else if (!strncmp_local(cmd, "install fat32 ", 14)) {
        const char* arg = cmd + 14;
        char device_buf[16] = {0};
        size_t i = 0;
        install_partition_style_t style;

        while (arg[i] && arg[i] != ' ' && i < sizeof(device_buf) - 1) {
            device_buf[i] = arg[i];
            i++;
        }
        device_buf[i] = '\0';
        while (arg[i] == ' ') i++;

        if (device_buf[0] == '\0') {
            print("install: target id required (see `install list`)\n");
        } else if (!(arg[i] == 'Y' && arg[i + 1] == 'E' && arg[i + 2] == 'S' &&
                     (arg[i + 3] == '\0' || arg[i + 3] == ' '))) {
            print("install: usage: install fat32 <id> YES MBR|GPT\n");
        } else {
            i += 3;
            while (arg[i] == ' ') i++;
            if (arg[i] == '\0') {
                print("install: choose partition style: MBR or GPT\n");
            } else if (install_parse_style(arg + i, &style) != 0) {
                print("install: unknown style (use MBR or GPT)\n");
            } else {
                install_fat32_target(atoi(device_buf), style);
            }
        }
    } else if (!strncmp_local(cmd, "edit ", 5)) {
        const char* filename = cmd + 5;
        while (*filename == ' ') filename++;
        if (*filename == '\0') {
            print("edit: Filename required\n");
        } else {
            run_editor(filename);
            shell_clear_display();
            vga_set_text_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            cursor_row = 0;
            cursor_col = 0;
            print("Exited editor\n");
            vga_set_text_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        }
    } else if (!strcmp_local(cmd, "reboot") || !strcmp_local(cmd, "exit")) {
        /* Text-mode: exit and reboot both reboot. VESA GooberShell overrides
         * exit to close the window before execute_command is reached. */
        print("Rebooting...\n");
        shell_reboot();
    } else if (!strcmp_local(cmd, "games")) {
        list_games();
    } else if (!strcmp_local(cmd, "snakeGame.exe") ||
               !strcmp_local(cmd, "cubeDip.exe") ||
               !strcmp_local(cmd, "pong.exe")) {
        if (shell_is_live()) {
            print("Legacy VGA games disabled on live ISO.\n");
            print("Use Start -> Games for GooberC Minesweeper/CubeDip/SnakeGame/DoomRay.\n");
        } else if (!strcmp_local(cmd, "snakeGame.exe")) {
            print("Launching snakeGame.exe... Press ESC to quit.\n");
            run_snake_game();
            print("Exited snakeGame.exe\n");
        } else if (!strcmp_local(cmd, "cubeDip.exe")) {
            print("Launching cubeDip.exe... Press ESC to quit.\n");
            run_cubeDip_game();
            print("Exited cubeDip.exe\n");
        } else {
            print("Launching Pong... Press ESC to quit.\n");
            run_pong_game();
            print("Exited Pong\n");
        }
    } else if (!strcmp_local(cmd, "doom.exe")) {
        print("Launching Doom prototype... Press ESC to quit.\n");
        run_doom_game();
        print("Exited Doom\n");
    } else if (!strcmp_local(cmd, "taskview")) {
        run_task_manager();
        print("Exited task manager\n");
    } else {
        print("Unknown command: ");
        print(cmd);
        print("\n");
    }



    if (!redirected) {
        cursor_enabled = 1;
        draw_cursor();
    }
}



void shell_init() {
    /* Make sure the text-console layer is bound before the prompt tries
     * to write its first cell. The x64 VGA-compat boot path binds the
     * console explicitly in framebuffer_bringup() (VGA on legacy BIOS,
     * the GOP framebuffer on UEFI). For every other path -- and as a
     * defensive fallback -- bind the VGA backend here so shell output
     * always lands somewhere visible. con_init_vga is idempotent. */
    if (!con_ready()) {
        con_init_vga();
    }
    clear_input();
    prompt();
}

void shell_run() {
    char c = keyboard_read_char();
    if (!c) {
        blink_cursor();
        return;
    }

    if (c == '\r' || c == '\n') {
        restore_prev_cursor_cell();        // <— restore first
        print_char_shell('\n');            // moves to new line cleanly
        input_buffer[input_pos] = '\0';
        execute_command(input_buffer);     // printing is safe (cursor disabled inside)
        clear_input();
        prompt();                          // prompt restores before drawing itself
    } else if (c == '\b' || c == 127) {
        if (input_pos > 0) {
            size_t len = input_len();
            for (size_t i = input_pos - 1; i < len && i + 1 < INPUT_BUFFER_SIZE; i++)
                input_buffer[i] = input_buffer[i + 1];
            input_buffer[len > 0 ? len - 1 : 0] = '\0';
            input_pos--;
            restore_prev_cursor_cell();
            set_input_line(input_buffer);
        }
    } else if ((unsigned char)c == KEY_F1) {
        restore_prev_cursor_cell();       // safety
        run_task_manager();
        print("Exited task manager\n");
        prompt();
    } else if ((unsigned char)c == KEY_UP) {
        // Up arrow: older command
        if (history_count > 0) {
            if (history_nav_offset < 0) {
                strcpy(saved_input, input_buffer);
                history_nav_offset = 0;
            } else if (history_nav_offset < history_count - 1) {
                history_nav_offset++;
            }

            int idx = history_index_from_offset(history_nav_offset);
            strcpy(input_buffer, history[idx]);
            input_pos = strlen(input_buffer);
            restore_prev_cursor_cell();
            set_input_line(input_buffer);
        }
    } else if ((unsigned char)c == KEY_DOWN) {
        // Down arrow: newer command
        if (history_count > 0 && history_nav_offset >= 0) {
            history_nav_offset--;
            if (history_nav_offset < 0) {
                strcpy(input_buffer, saved_input);
            } else {
                int idx = history_index_from_offset(history_nav_offset);
                strcpy(input_buffer, history[idx]);
            }
            input_pos = strlen(input_buffer);
            restore_prev_cursor_cell();
            set_input_line(input_buffer);
        }
    } else if ((unsigned char)c == KEY_LEFT) {
        // Left arrow: move cursor left
        if (input_pos > 0) {
            input_pos--;
            int r, col;
            input_index_to_pos(input_pos, &r, &col);
            restore_prev_cursor_cell();
            move_cursor(r, col);
            draw_cursor();
        }
    } else if ((unsigned char)c == KEY_RIGHT) {
        // Right arrow: move cursor right
        size_t len = input_len();
        if (input_pos < len) {
            input_pos++;
            int r, col;
            input_index_to_pos(input_pos, &r, &col);
            restore_prev_cursor_cell();
            move_cursor(r, col);
            draw_cursor();
        }
    } else if (input_pos < INPUT_BUFFER_SIZE - 1) {
        size_t len = input_len();
        if (input_pos < len && len < INPUT_BUFFER_SIZE - 1) {
            for (size_t i = len + 1; i > input_pos; i--) input_buffer[i] = input_buffer[i - 1];
            input_buffer[input_pos] = c;
            input_pos++;
            restore_prev_cursor_cell();
            set_input_line(input_buffer);
        } else if (len < INPUT_BUFFER_SIZE - 1) {
            int r = cursor_row, col = cursor_col;
            restore_prev_cursor_cell();
            input_buffer[input_pos++] = c;
            put_cell(r, col, c, INPUT_COLOR);
            cursor_row = r;
            cursor_col = col;
            if (++cursor_col >= SCREEN_COLS) { cursor_col = 0; cursor_row++; ensure_scroll(); }
            draw_cursor();
        }
    }
}
