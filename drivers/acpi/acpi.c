#include "acpi.h"
#include <stddef.h>
#include "../../lib/string.h"

extern void print(const char* str);

typedef struct {
    char signature[8];
    uint8_t checksum;
    char oemid[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oemid[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

static acpi_touchpad_info_t g_touchpad_info;

static int memeq(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static uint8_t acpi_checksum(const uint8_t* p, uint32_t len) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + p[i]);
    return sum;
}

static acpi_rsdp_t* find_rsdp_range(uintptr_t start, uintptr_t end) {
    for (uintptr_t p = start; p + 20 <= end; p += 16) {
        acpi_rsdp_t* rsdp = (acpi_rsdp_t*)p;
        if (!memeq(rsdp->signature, "RSD PTR ", 8)) continue;
        if (acpi_checksum((const uint8_t*)rsdp, 20) != 0) continue;
        return rsdp;
    }
    return NULL;
}

static acpi_rsdp_t* find_rsdp(void) {
    volatile uint16_t* ebda_seg_ptr = (volatile uint16_t*)(uintptr_t)0x40E;
    __asm__ volatile ("" : "+r"(ebda_seg_ptr));
    uint16_t ebda_seg = *ebda_seg_ptr;
    uintptr_t ebda = ((uintptr_t)ebda_seg) << 4;
    acpi_rsdp_t* rsdp = NULL;

    if (ebda >= 0x80000 && ebda < 0xA0000) {
        rsdp = find_rsdp_range(ebda, ebda + 1024);
        if (rsdp) return rsdp;
    }

    return find_rsdp_range(0xE0000, 0x100000);
}

static int table_valid(const acpi_sdt_header_t* h) {
    if (!h) return 0;
    if (h->length < sizeof(acpi_sdt_header_t) || h->length > 0x200000) return 0;
    return acpi_checksum((const uint8_t*)h, h->length) == 0;
}

static int table_contains(const acpi_sdt_header_t* h, const char* needle) {
    const uint8_t* p = (const uint8_t*)h;
    uint32_t len = h->length;
    uint32_t nlen = (uint32_t)strlen(needle);
    if (nlen == 0 || len < nlen) return 0;
    for (uint32_t i = 0; i + nlen <= len; i++) {
        uint32_t j = 0;
        while (j < nlen && p[i + j] == (uint8_t)needle[j]) j++;
        if (j == nlen) return 1;
    }
    return 0;
}

static void scan_aml_table(const acpi_sdt_header_t* h) {
    if (!table_valid(h)) return;
    if (table_contains(h, "ELAN0") || table_contains(h, "ELAN1")) {
        g_touchpad_info.elan0601_found = 1;
    }
    if (table_contains(h, "PNP0C50")) g_touchpad_info.pnp0c50_found = 1;
    if (table_contains(h, "SYNA") || table_contains(h, "SYN0")) {
        g_touchpad_info.pnp0c50_found = 1;
    }
    if (table_contains(h, "80860F41")) g_touchpad_info.baytrail_i2c_found = 1;
}

static void scan_table_pointer(uintptr_t addr);

static void scan_rsdt(const acpi_sdt_header_t* rsdt) {
    if (!table_valid(rsdt)) return;
    uint32_t entries = (rsdt->length - sizeof(acpi_sdt_header_t)) / 4;
    const uint32_t* p = (const uint32_t*)((const uint8_t*)rsdt + sizeof(acpi_sdt_header_t));
    for (uint32_t i = 0; i < entries && i < 64; i++) {
        scan_table_pointer((uintptr_t)p[i]);
    }
}

static void scan_xsdt(const acpi_sdt_header_t* xsdt) {
    if (!table_valid(xsdt)) return;
    uint32_t entries = (xsdt->length - sizeof(acpi_sdt_header_t)) / 8;
    const uint64_t* p = (const uint64_t*)((const uint8_t*)xsdt + sizeof(acpi_sdt_header_t));
    for (uint32_t i = 0; i < entries && i < 64; i++) {
        if (sizeof(uintptr_t) < sizeof(uint64_t) && (p[i] >> 32) != 0) continue;
        scan_table_pointer((uintptr_t)p[i]);
    }
}

static void scan_table_pointer(uintptr_t addr) {
    if (!addr) return;
    acpi_sdt_header_t* h = (acpi_sdt_header_t*)addr;
    if (!table_valid(h)) return;
    if (memeq(h->signature, "DSDT", 4) || memeq(h->signature, "SSDT", 4)) {
        scan_aml_table(h);
    }
}

static void print_bool(const char* label, int v) {
    print(label);
    print(v ? "yes\n" : "no\n");
}

void acpi_init(void) {
    g_touchpad_info.acpi_found = 0;
    g_touchpad_info.elan0601_found = 0;
    g_touchpad_info.pnp0c50_found = 0;
    g_touchpad_info.baytrail_i2c_found = 0;
    g_touchpad_info.touchpad_i2c_addr = 0;
    g_touchpad_info.hid_desc_reg = 0x0001;

    acpi_rsdp_t* rsdp = find_rsdp();
    if (!rsdp) {
        print("[acpi] RSDP not found; I2C touchpad discovery disabled.\n");
        return;
    }

    g_touchpad_info.acpi_found = 1;

    if (rsdp->revision >= 2 && rsdp->length >= sizeof(acpi_rsdp_t) &&
        acpi_checksum((const uint8_t*)rsdp, rsdp->length) == 0 &&
        rsdp->xsdt_address != 0) {
        if (sizeof(uintptr_t) == sizeof(uint64_t) || (rsdp->xsdt_address >> 32) == 0) {
            scan_xsdt((const acpi_sdt_header_t*)(uintptr_t)rsdp->xsdt_address);
        }
    }
    if (rsdp->rsdt_address != 0) {
        scan_rsdt((const acpi_sdt_header_t*)(uintptr_t)rsdp->rsdt_address);
    }

    if (g_touchpad_info.elan0601_found || g_touchpad_info.pnp0c50_found) {
        /* Lenovo S21e-20 Type 80M4 / ELAN0601 common HID-I2C address. */
        g_touchpad_info.touchpad_i2c_addr = 0x15;
    }

    print("[acpi] touchpad discovery:\n");
    print_bool("  ELAN0601: ", g_touchpad_info.elan0601_found);
    print_bool("  PNP0C50: ", g_touchpad_info.pnp0c50_found);
    print_bool("  80860F41 I2C: ", g_touchpad_info.baytrail_i2c_found);
}

const acpi_touchpad_info_t* acpi_get_touchpad_info(void) {
    return &g_touchpad_info;
}
