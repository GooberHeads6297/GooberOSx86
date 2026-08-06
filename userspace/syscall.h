#ifndef GOOBER_SYSCALL_H
#define GOOBER_SYSCALL_H

#include <stdint.h>

/* GooberOS syscall numbers (int 0x80). Stable ABI for GooberC / .gob. */
#define SYS_EXIT              1
#define SYS_WRITE             2   /* fd, buf, len — fd 1 = console */
#define SYS_YIELD             3
#define SYS_OPEN              4
#define SYS_CLOSE             5
#define SYS_READ              6
#define SYS_GUI_WIN_CREATE    20  /* title, w, h -> win id */
#define SYS_GUI_WIN_TEXT      21  /* win, x, y, text */
#define SYS_GUI_WIN_WAIT      22  /* win — block until dismissed */
#define SYS_GUI_WIN_CLOSE     23

#define GOB_MAGIC             0x00424F47u  /* 'GOB\0' little-endian */
#define GOB_VERSION           1
#define GOB_ARCH_X86_64       2
#define GOB_ARCH_I386         1
#define GOB_KIND_CONSOLE      1
#define GOB_KIND_GUI          2
#define GOB_KIND_AUTO         3
#define GOB_KIND_GFX3D        4
#define GOB_FLAG_BYTECODE     1u

/* Compact bytecode opcodes (v1 IR; maps to syscalls). */
#define GBC_NOP               0
#define GBC_EXIT              1   /* u32 code */
#define GBC_WRITE             2   /* u32 str_off, u32 len */
#define GBC_YIELD             3
#define GBC_GUI_CREATE        4   /* u32 title_off, u16 w, u16 h */
#define GBC_GUI_TEXT          5   /* u32 win_slot, u16 x, u16 y, u32 str_off */
#define GBC_GUI_WAIT          6   /* u32 win_slot */
#define GBC_GUI_CLOSE         7   /* u32 win_slot */
#define GBC_SLEEP_MS          8   /* u32 ms — stub: yield frames */
#define GBC_GFX3D_CLEAR       9   /* u32 rgba — stub: no-op / reserved */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint8_t  arch;
    uint8_t  kind;
    uint32_t flags;
    uint32_t entry;       /* bytecode offset from start of code, or native entry RVA */
    uint32_t code_size;
    uint32_t rodata_size;
    uint32_t reserved;
} gob_header_t;

#endif
