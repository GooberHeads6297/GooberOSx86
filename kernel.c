#include <stdint.h>
#include <stddef.h>
#include "drivers/timer/timer.h"
#include "drivers/keyboard/keyboard.h"
#include "drivers/mouse/mouse.h"
#include "drivers/io/io.h"
#include "drivers/video/vga.h"
#include "drivers/input/input.h"
#include "drivers/pci/pci.h"
#include "drivers/usb/usb.h"
#include "taskmgr/process.h"
#include "lib/memory.h"

#define IRQ0 32
#define IRQ1 33

#define KERNEL_HEAP_SIZE (64 * 1024)  // 64KB heap, adjust as needed

volatile int keyboard_interrupt_flag = 0;

extern unsigned char _kernel_start;
extern unsigned char _kernel_end;



static int kernel_pid = -1;

static void update_kernel_process_memory();

static void register_kernel_process() {
    size_t kernel_size_bytes = (size_t)(&_kernel_end) - (size_t)(&_kernel_start);
    size_t kernel_size_kb = (kernel_size_bytes + 1023) / 1024;
    create_process("kernel.bin", kernel_size_kb);
}

struct IDTEntry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t type_attr;
    uint16_t base_high;
} __attribute__((packed));

struct IDTPointer {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct IDTEntry idt[256];
static struct IDTPointer idt_ptr;

extern void load_idt(struct IDTPointer*);
extern void irq1_handler_asm();
extern void irq12_handler_asm();
extern void isr32_stub();

static unsigned int update_counter = 0;

void pic_remap() {
    uint8_t a1 = inb(0x21);
    uint8_t a2 = inb(0xA1);

    outb(0x20, 0x11);
    outb(0xA0, 0x11);

    outb(0x21, 0x20);
    outb(0xA1, 0x28);

    outb(0x21, 4);
    outb(0xA1, 2);

    outb(0x21, 0x01);
    outb(0xA1, 0x01);

    outb(0x21, a1);
    outb(0xA1, a2);
}

void set_idt_entry(int index, uint32_t base, uint16_t selector, uint8_t type_attr) {
    idt[index].base_low  = base & 0xFFFF;
    idt[index].selector  = selector;
    idt[index].zero      = 0;
    idt[index].type_attr = type_attr;
    idt[index].base_high = (base >> 16) & 0xFFFF;
}

void irq0_handler_main() {
    // Increment counter and update kernel process memory every ~2 seconds
    update_counter++;
    if (update_counter >= 200) {
        update_kernel_process_memory();
        update_counter = 0;
    }

    timer_interrupt_handler();
}

__attribute__((naked)) void irq0_handler_asm() {
    __asm__ volatile (
        "pusha\n"
        "call irq0_handler_main\n"
        "popa\n"
        "iret\n"
    );
}

void irq1_handler_main() {
    keyboard_interrupt_handler();
}

void idt_init() {
    pic_remap();
    set_idt_entry(IRQ0, (uint32_t)irq0_handler_asm, 0x08, 0x8E);
    set_idt_entry(IRQ1, (uint32_t)irq1_handler_asm, 0x08, 0x8E);
    set_idt_entry(44, (uint32_t)irq12_handler_asm, 0x08, 0x8E);
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint32_t)&idt;
    load_idt(&idt_ptr);
}

static void print_char(char c) {
    vga_put_char(c);
}

void print(const char* str) {
    for (size_t i = 0; str[i] != '\0'; i++)
        print_char(str[i]);
}

extern void fs_init();
extern void shell_init();
extern void shell_run();

static void update_kernel_process_memory() {
    if (kernel_pid < 0) return;
    size_t kernel_size_bytes = (size_t)(&_kernel_end) - (size_t)(&_kernel_start);
    size_t kernel_size_kb = (kernel_size_bytes + 1023) / 1024;

    process_entry_t *table = get_kernel_process_table();
    int total = get_kernel_process_count();
    for (int i = 0; i < total; i++) {
        if (table[i].pid == kernel_pid && table[i].active) {
            table[i].memory_kb = kernel_size_kb;
            break;
        }
    }
}

void kernel_main() {
    vga_set_text_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    clear_screen();
    print("GooberOS -- x86 Kernel\n");
    print("VGA output Success.\n");
    print("\n");

    idt_init();
    timer_init(100);
    input_init();
    mouse_init();
    pci_init();
    usb_init();
    fs_init();

    // Initialize heap allocator
    void* heap_start = (void*)(&_kernel_end);
    memory_init(heap_start, KERNEL_HEAP_SIZE);

    kernel_pid = create_process("kernel.bin", 0);
    update_kernel_process_memory();

    shell_init();

    __asm__ volatile("sti");

    while (1) {
        usb_poll();
        shell_run();
        __asm__("hlt");
    }
}
