#ifndef GOOBER_DOSEMU_PRIV_H
#define GOOBER_DOSEMU_PRIV_H

#include <stdint.h>
#include <stddef.h>
#include "../fs/filesystem.h"
#include "../gui/vesa_window.h"

/* 8 MiB guest arena: 1 MiB conventional + extended for DOS/4GW / Doom */
#define DOS_MEM_SIZE     (8u * 1024u * 1024u)
#define DOS_TEXT_COLS    80
#define DOS_TEXT_ROWS    25
#define DOS_KEYQ_MAX     64
#define DOS_MAX_FILES    32
#define DOS_PSP_SEG      0x0050u
#define DOS_ENV_SEG      0x0060u
#define DOS_COM_IP       0x0100u
#define DOS_LOAD_SEG     0x1000u
#define DOS_IVT_IRET     0xF000u
#define DOS_B800_LINEAR  0xB8000u
#define DOS_A000_LINEAR  0xA0000u
#define DOS_MEM_TOP_SEG  0x9FFFu
#define DOS_EXT_KB       ((DOS_MEM_SIZE / 1024u) > 1024u ? (DOS_MEM_SIZE / 1024u) - 1024u : 0u)

typedef struct {
    uint16_t ax, bx, cx, dx, si, di, bp, sp;
    uint16_t cs, ds, es, ss, ip;
    uint16_t flags;
} cpu8086_t;

/* Cached segment descriptor (PM) */
typedef struct {
    uint32_t base;
    uint32_t limit;
    uint16_t sel;
    uint8_t  dpl;
    uint8_t  present;
    uint8_t  code;      /* 1=code 0=data */
    uint8_t  use32;     /* D/B bit: 32-bit default size */
    uint8_t  readable;
    uint8_t  writable;
} pm_seg_t;

typedef struct {
    int used;
    FileHandle* fh;
    int is_std;
    uint32_t pos;
    char host_path[160];
} dos_file_t;

/* DPMI memory block */
#define DPMI_MAX_BLOCKS 32
typedef struct {
    int used;
    uint32_t linear;
    uint32_t pages; /* 4K pages */
    uint32_t handle;
} dpmi_block_t;

typedef struct dos_session {
    int used;
    int halted;
    int pid;
    VWindow* win;
    cpu8086_t cpu;
    uint8_t* mem;

    /* 386 / protected-mode state */
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp32;
    uint32_t eip;
    uint32_t eflags;
    uint16_t fs, gs;
    uint32_t cr0;
    uint32_t gdtr_base;
    uint16_t gdtr_limit;
    uint32_t idtr_base;
    uint16_t idtr_limit;
    pm_seg_t seg_cs, seg_ds, seg_es, seg_ss, seg_fs, seg_gs;
    int pe;           /* CR0.PE cached */
    int cpu32;        /* executing with 32-bit code segment */
    int op32;         /* 66 prefix toggles for this insn */
    int addr32;       /* 67 prefix */
    int pm_enabled;   /* feature flag: allow PE transition */

    /* Soft x87 (enough for DOS/4GW FNINIT / CW probes) */
    uint16_t fpu_cw;
    uint16_t fpu_sw;
    uint16_t fpu_tw;
    int fpu_top;

    /* DPMI */
    int dpmi_installed;
    uint16_t dpmi_version; /* 0x005A = 0.90 */
    dpmi_block_t dpmi_blocks[DPMI_MAX_BLOCKS];
    uint32_t dpmi_next_handle;
    uint32_t himem_brk; /* next free linear above 1MB */

    /* video */
    char text[DOS_TEXT_ROWS][DOS_TEXT_COLS];
    uint8_t attr[DOS_TEXT_ROWS][DOS_TEXT_COLS];
    int cursor_r, cursor_c;
    int video_mode;
    uint8_t* vga13;
    uint8_t active_page;
    uint8_t cur_attr;
    uint8_t vga_dac_idx;
    uint8_t vga_dac_comp;
    uint8_t vga_pal[256][3];
    uint8_t vga_status_toggle;
    uint8_t cmos_index;
    uint8_t cmos_data[128];
    uint8_t pit_mode;       /* last 8253 command (port 43h) */
    uint8_t pit_latch_lo;   /* next OUT 40h is low byte */
    uint16_t pit_reload;
    uint16_t pit_count;

    uint16_t keyq[DOS_KEYQ_MAX];
    int keyq_r, keyq_w;

    int mouse_shown;
    int mouse_x, mouse_y;
    int mouse_buttons;
    int16_t mouse_mickey_x, mouse_mickey_y;
    uint16_t mouse_min_x, mouse_max_x, mouse_min_y, mouse_max_y;
    uint16_t mouse_handler_seg, mouse_handler_off;
    uint16_t mouse_handler_mask;

    dos_file_t files[DOS_MAX_FILES];
    uint16_t dta_seg, dta_off;
    char find_pat[14];
    int find_index;
    char guest_cwd[96];
    uint8_t verify_flag;
    uint8_t break_flag;
    uint16_t return_code;

    uint16_t seg_override;
    int has_seg_override;
    int rep_prefix;

    uint32_t bios_ticks;
    uint32_t steps_total;
    uint32_t steps_since_tick;
    uint16_t mcb_first;
    uint32_t cycles_per_tick;
    int cycles_preset;

    int at_shell;
    int shell_reentry;
    int shell_want_close;
    char shell_line[128];
    int shell_len;
    int shell_echo;

    char path[96];
    uint16_t psp_seg;
    uint16_t env_seg;
} dos_session_t;

/* memory */
int dos_mem_alloc(dos_session_t* s);
void dos_mem_free(dos_session_t* s);
uint8_t dos_read8(dos_session_t* s, uint32_t linear);
uint16_t dos_read16(dos_session_t* s, uint32_t linear);
uint32_t dos_read32(dos_session_t* s, uint32_t linear);
void dos_write8(dos_session_t* s, uint32_t linear, uint8_t v);
void dos_write16(dos_session_t* s, uint32_t linear, uint16_t v);
void dos_write32(dos_session_t* s, uint32_t linear, uint32_t v);
uint32_t dos_seg_off(uint16_t seg, uint16_t off);
void dos_setup_ivt(dos_session_t* s);
void dos_setup_psp(dos_session_t* s, const char* cmdline);
void dos_mcb_init(dos_session_t* s, uint16_t first_seg, uint16_t end_seg);
int dos_mcb_alloc(dos_session_t* s, uint16_t paras, uint16_t* out_seg);
int dos_mcb_free(dos_session_t* s, uint16_t seg);
int dos_mcb_resize(dos_session_t* s, uint16_t seg, uint16_t paras, uint16_t* out_max);
int dos_ivt_is_default(dos_session_t* s, uint8_t vec);
void dos_soft_int(dos_session_t* s, uint8_t vec);

/* cpu */
void cpu8086_reset(cpu8086_t* c, uint16_t cs, uint16_t ip, uint16_t ss, uint16_t sp);
int cpu8086_step(dos_session_t* s);
void dos_out_port(dos_session_t* s, uint16_t port, uint8_t val);
uint8_t dos_in_port(dos_session_t* s, uint16_t port);
void cpu386_sync_from_16(dos_session_t* s);
void cpu386_sync_to_16(dos_session_t* s);
int cpu386_step(dos_session_t* s); /* when pe && cpu32 */
int cpu_exec_0f(dos_session_t* s); /* 0F escape from RM or PM16 */
int cpu386_rm_after_prefix(dos_session_t* s, uint8_t op);
int cpu_exec_fpu(dos_session_t* s, uint8_t esc_op); /* D8–DF x87 escape */

/* protected mode */
void pm_init_session(dos_session_t* s);
void pm_sync_real_segs(dos_session_t* s);
int pm_load_selector(dos_session_t* s, uint16_t sel, pm_seg_t* out);
void pm_set_cr0(dos_session_t* s, uint32_t v);
uint32_t pm_ea(dos_session_t* s, const pm_seg_t* seg, uint32_t off);
int pm_far_jump(dos_session_t* s, uint16_t new_cs, uint32_t new_eip);

/* DPMI */
void dpmi_init(dos_session_t* s);
int dpmi_int31(dos_session_t* s);
int dos_bios_int2f(dos_session_t* s); /* multiplex / DPMI detect */

/* services */
void dos_video_init(dos_session_t* s);
void dos_video_set_mode(dos_session_t* s, uint8_t mode);
void dos_video_putc(dos_session_t* s, char ch);
void dos_video_puts(dos_session_t* s, const char* str);
void dos_video_render(dos_session_t* s, int cx, int cy, int cw, int ch);
void dos_video_sync_from_b800(dos_session_t* s);
void dos_video_sync_to_b800(dos_session_t* s);
void dos_key_push(dos_session_t* s, char key);
void dos_key_push_word(dos_session_t* s, uint16_t word);
int dos_key_pop(dos_session_t* s, char* out);
int dos_key_pop_word(dos_session_t* s, uint16_t* out);
int dos_key_peek(dos_session_t* s, char* out);
int dos_key_peek_word(dos_session_t* s, uint16_t* out);
int dos_handle_int(dos_session_t* s, uint8_t vec);
void dos_timer_tick(dos_session_t* s);
void dos_mouse_update(dos_session_t* s);

int dos_bios_int10(dos_session_t* s);
int dos_bios_int16(dos_session_t* s);
int dos_bios_int1a(dos_session_t* s);
int dos_bios_int15(dos_session_t* s);
int dos_bios_int33(dos_session_t* s);

int dos_guest_to_host(const char* guest, char* host, size_t host_sz);
FileHandle* dos_open_host_path(const char* path);
Directory* dos_resolve_host_dir(const char* host_dir);
int dos_host_mkdir(const char* host_path);
int dos_host_rmdir(const char* host_path);
int dos_host_rename(const char* old_host, const char* new_host);
int dos_host_write(const char* host_path, const uint8_t* data, size_t size);
int dos_host_delete(const char* host_path);
int dos_file_seek(dos_session_t* s, int fd, uint32_t pos);
int dos_file_write(dos_session_t* s, int fd, uint32_t addr, uint16_t count);

void dos_shell_boot(dos_session_t* s);
void dos_shell_show_prompt(dos_session_t* s);
int dos_shell_on_key(dos_session_t* s, char key);
void dos_shell_on_guest_exit(dos_session_t* s);
int dos_shell_launch(dos_session_t* s, const char* name, const char* args);
void dos_cycles_adjust(dos_session_t* s, int delta);
const char* dos_cycles_label(dos_session_t* s);

int dos_load_program_bytes(dos_session_t* s, const uint8_t* data, size_t len,
                           const char* cmdline, int is_exe);

extern dos_session_t g_dos_session;

#endif
