#include <stdint.h>
#include <stddef.h>

static uint16_t* const VIDEO_MEMORY = (uint16_t*)0xb8000;
static uint8_t cursor_row = 0;
static uint8_t cursor_col = 0;

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
extern void isr32_stub();

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define ICW1_INIT    0x10
#define ICW1_ICW4    0x01
#define ICW4_8086    0x01

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void pic_remap() {
    uint8_t a1 = inb(PIC1_DATA);
    uint8_t a2 = inb(PIC2_DATA);

    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);

    outb(PIC1_DATA, 0x20);
    outb(PIC2_DATA, 0x28);

    outb(PIC1_DATA, 4);
    outb(PIC2_DATA, 2);

    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);

    outb(PIC1_DATA, 0xFD);  // Unmask only IRQ1 (keyboard)
    outb(PIC2_DATA, 0xFF);  // Mask all on Slave PIC
}

void set_idt_entry(int index, uint32_t base, uint16_t selector, uint8_t type_attr) {
    idt[index].base_low  = base & 0xFFFF;
    idt[index].selector  = selector;
    idt[index].zero      = 0;
    idt[index].type_attr = type_attr;
    idt[index].base_high = (base >> 16) & 0xFFFF;
}

void print(const char* str);

#define IRQ0 32
#define IRQ1 33

volatile int keyboard_interrupt_flag = 0;

void irq1_handler_main() {
    volatile uint8_t scancode = inb(0x60);
    (void)scancode;
    keyboard_interrupt_flag = 1;
}

void idt_init() {
    pic_remap();

    set_idt_entry(IRQ0, (uint32_t)isr32_stub,       0x08, 0x8E);
    set_idt_entry(IRQ1, (uint32_t)irq1_handler_asm,  0x08, 0x8E);

    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint32_t)&idt;
    load_idt(&idt_ptr);
}

static void scroll_screen() {
    for (size_t row = 1; row < 25; row++) {
        for (size_t col = 0; col < 80; col++) {
            VIDEO_MEMORY[(row - 1) * 80 + col] = VIDEO_MEMORY[row * 80 + col];
        }
    }
    for (size_t col = 0; col < 80; col++) {
        VIDEO_MEMORY[24 * 80 + col] = ((uint16_t)0x0F << 8) | ' ';
    }
    if (cursor_row > 0) cursor_row--;
}

static void clear_screen() {
    for (size_t row = 0; row < 25; row++)
        for (size_t col = 0; col < 80; col++)
            VIDEO_MEMORY[row * 80 + col] = ((uint16_t)0x0F << 8) | ' ';
    cursor_row = 0;
    cursor_col = 0;
}

static void print_char(char c) {
    if (c == '\n') {
        cursor_row++;
        cursor_col = 0;
    } else {
        VIDEO_MEMORY[cursor_row * 80 + cursor_col] = ((uint16_t)0x0F << 8) | c;
        cursor_col++;
        if (cursor_col >= 80) {
            cursor_col = 0;
            cursor_row++;
        }
    }
    if (cursor_row >= 25) scroll_screen();
}

void print(const char* str) {
    for (size_t i = 0; str[i] != '\0'; i++)
        print_char(str[i]);
}

static void delay() {
    for (volatile int i = 0; i < 100000000; i++)
        __asm__("nop");
}

void kernel_main() {
    clear_screen();
    print("GooberOS -- x86 Kernel\n");
    print("VGA output Success.\n");
    print("Testing scrolling...\n");

    delay();

    char buffer[8];
    for (int i = 1; i <= 3; i++) {
        buffer[0] = '/';
        buffer[1] = ':';
        buffer[2] = '0' + (i / 10);
        buffer[3] = '0' + (i % 10);
        buffer[4] = '\n';
        buffer[5] = '\0';
        print(buffer);
    }

    idt_init();
    __asm__ volatile("sti");

    while (1) {
        if (keyboard_interrupt_flag) {
            keyboard_interrupt_flag = 0;
            print("Keyboard Interrupt Detected!\n");
        }
        __asm__("hlt");
    }
}
