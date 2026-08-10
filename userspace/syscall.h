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
#define GOB_VERSION           2
#define GOB_ARCH_X86_64       2
#define GOB_ARCH_I386         1
#define GOB_KIND_CONSOLE      1
#define GOB_KIND_GUI          2
#define GOB_KIND_AUTO         3
#define GOB_KIND_GFX3D        4
#define GOB_FLAG_BYTECODE     1u

/* Compact bytecode opcodes (v2 IR). */
#define GBC_NOP               0
#define GBC_EXIT              1   /* u32 code */
#define GBC_WRITE             2   /* u32 str_off, u32 len */
#define GBC_YIELD             3
#define GBC_GUI_CREATE        4   /* u32 title_off, u16 w, u16 h */
#define GBC_GUI_TEXT          5   /* u32 win_slot, u16 x, u16 y, u32 str_off */
#define GBC_GUI_WAIT          6   /* u32 win_slot */
#define GBC_GUI_CLOSE         7   /* u32 win_slot */
#define GBC_SLEEP_MS          8   /* u32 ms */
#define GBC_GFX3D_CLEAR       9   /* u32 rgba */
#define GBC_PUSH_I           10   /* i32 imm */
#define GBC_LOAD             11   /* u8 global slot */
#define GBC_STORE            12   /* u8 global slot */
#define GBC_ADD              13
#define GBC_SUB              14
#define GBC_MUL              15
#define GBC_DIV              16
#define GBC_CMP_EQ           17
#define GBC_CMP_NE           18
#define GBC_CMP_LT           19
#define GBC_CMP_LE           20
#define GBC_CMP_GT           21
#define GBC_CMP_GE           22
#define GBC_JMP              23   /* u32 abs IP */
#define GBC_JZ               24   /* u32 abs IP — jump if pop==0 */
#define GBC_CALL             25   /* u32 abs IP — arity 0 (v1 compat) */
#define GBC_RET              26
#define GBC_PRINT_I          27   /* pop value, print int or string */
/* v2 */
#define GBC_LOAD_LOCAL       28   /* u8 slot */
#define GBC_STORE_LOCAL      29   /* u8 slot */
#define GBC_CALL_N           30   /* u32 abs IP, u8 arity */
#define GBC_RET_V            31   /* return with value on stack */
#define GBC_PUSH_STR         32   /* u32 rodata_off — push string object */
#define GBC_LEN              33   /* pop str/list -> len */
#define GBC_LIST_NEW         34   /* u8 count; pop count ints → list */
#define GBC_LIST_PUSH        35   /* pop val, list → list */
#define GBC_LIST_GET         36   /* pop idx, list → val */
#define GBC_ALLOC            37   /* pop n → ptr handle */
#define GBC_FREE             38   /* pop ptr */
#define GBC_FS_EXISTS        39   /* pop str → 0/1 */
#define GBC_FS_READ          40   /* pop path str → data str or 0 */
#define GBC_FS_WRITE         41   /* pop data, path → 0/1 ok */
#define GBC_STR_JOIN         42   /* pop b, a → concat str */
#define GBC_DUP              43
#define GBC_POP              44
#define GBC_PRINT_RAW        45   /* pop str — print without newline */
/* v2.1 — paths, strings, larger collections, errors */
#define GBC_STR_SLICE        46   /* pop end, start, str → str */
#define GBC_STR_FIND         47   /* pop needle, hay → index or -1 */
#define GBC_PATH_JOIN        48   /* pop b, a → "a/b" */
#define GBC_PATH_DIR         49   /* pop path → dirname */
#define GBC_PATH_BASE        50   /* pop path → basename */
#define GBC_FS_LIST          51   /* pop path → list of name strs (or 0) */
#define GBC_SET              52   /* pop val, key/idx, container → container */
#define GBC_TYPEOF           53   /* pop v → 0=int 1=str 2=list 3=blob 4=map */
#define GBC_LAST_ERR         54   /* push last runtime error string */
#define GBC_MAP_NEW          55   /* push empty map */
#define GBC_DOS_RUN          56   /* pop path str → launch GooberDOS (0/1) */
/* v2.2 — interactive GUI games */
#define GBC_KEY_POLL         57   /* push key code (0=none); pumps one frame */
#define GBC_GUI_CLEAR        58   /* clear window lines (slot 0); interactive */
#define GBC_GUI_TEXT_S       59   /* pop str → append line */
#define GBC_STR_I            60   /* pop int → decimal string */
#define GBC_GUI_CLOSED       61   /* push 1 if slot-0 window closed */
/* v2.3 — colorful 2D canvas (games / artwork) */
#define GBC_GFX_FILL         62   /* pop rgb — clear canvas + bg */
#define GBC_GFX_RECT         63   /* pop rgb,h,w,y,x — filled rect */
#define GBC_GFX_LABEL        64   /* pop bg,fg,str,y,x — text at x,y */
#define GBC_GFX_PRESENT      65   /* mark window dirty */
#define GBC_NUM              66   /* pop str → parse decimal int (0 on fail) */
/* v2.4 — game-loop timing + held keys */
#define GBC_MILLIS           67   /* push monotonic ms since boot */
#define GBC_KEY_HELD         68   /* pop key code → 1 if currently held */

#define GBC_MAX_GLOBALS      128
#define GBC_MAX_LOCALS       32
#define GBC_OP_STACK         128
#define GBC_CALL_STACK       64
#define GBC_MAX_OBJECTS      512
#define GBC_HEAP_BYTES       (128 * 1024)
#define GBC_LIST_MAX         256
#define GBC_MAP_MAX          64
#define GBC_STR_MAX          4096

/* Object handles: bit31 set; low bits = object id. Plain ints keep bit31 clear
 * for typical small values; tagged ops check IS_OBJ. */
#define GBC_OBJ_BIT          0x80000000u
#define GBC_IS_OBJ(v)        ((((uint32_t)(v)) & GBC_OBJ_BIT) != 0u)
#define GBC_OBJ_ID(v)        (((uint32_t)(v)) & 0x7FFFFFFFu)
#define GBC_MAKE_OBJ(id)     ((int32_t)(GBC_OBJ_BIT | (uint32_t)(id)))

#define GBC_OBJ_STR          1
#define GBC_OBJ_LIST         2
#define GBC_OBJ_BLOB         3
#define GBC_OBJ_MAP          4

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint8_t  arch;
    uint8_t  kind;
    uint32_t flags;
    uint32_t entry;       /* bytecode offset from start of code */
    uint32_t code_size;
    uint32_t rodata_size;
    uint32_t reserved;
} gob_header_t;

#endif
