#include "acpi.h"
#include <stddef.h>
#include "../../lib/string.h"
#include "../diagnostics/driver_log.h"

extern void print(const char* str);

/* Firmware-provided RSDP copy from the multiboot2 ACPI tag (0 if none).
 * Defined in kernel.c. On UEFI this is the only valid RSDP source. */
extern uintptr_t kernel_acpi_rsdp(void);

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
    /*
     * Preferred path (works on BIOS *and* UEFI): use the RSDP copy the
     * bootloader captured from firmware and handed us via the multiboot2
     * ACPI tag. Validating and following this is always safe.
     */
    uintptr_t fw = kernel_acpi_rsdp();
    if (fw) {
        acpi_rsdp_t* rsdp = (acpi_rsdp_t*)fw;
        if (memeq(rsdp->signature, "RSD PTR ", 8) &&
            acpi_checksum((const uint8_t*)rsdp, 20) == 0) {
            return rsdp;
        }
        /*
         * The firmware handed us a tag but it failed validation. Do NOT fall
         * back to the legacy BIOS scan on 64-bit/UEFI: blindly reading
         * 0xE0000..0xFFFFF (and any pointer found there) can stall the CPU bus
         * on real Braswell hardware (Acer R3-131T), hanging the boot past the
         * watchdog.
         */
    }

#if defined(__i386__)
    /*
     * Legacy BIOS (32-bit) fallback only. The EBDA + 0xE0000 scan is the
     * canonical BIOS discovery method and is safe there. Never reached on the
     * x86_64/UEFI build.
     */
    {
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
#else
    return NULL;
#endif
}

static int table_valid(const acpi_sdt_header_t* h) {
    if (!h) return 0;
    if (h->length < sizeof(acpi_sdt_header_t) || h->length > 0x200000) return 0;
    return acpi_checksum((const uint8_t*)h, h->length) == 0;
}

/* Length-only check: some OEM DSDTs fail checksum but are still usable. */
static int table_header_ok(const acpi_sdt_header_t* h) {
    if (!h) return 0;
    if (h->length < sizeof(acpi_sdt_header_t) || h->length > 0x200000) return 0;
    return 1;
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

static void acpi_add_emmc_mmio(uint32_t base, uint32_t length) {
    int i;
    if (base < 0x100000U) return; /* reject low / nonsense */
    if (length < 0x100U || length > 0x10000U) return;
    if (g_touchpad_info.emmc_mmio_count >= ACPI_EMMC_MAX_MMIO) return;
    for (i = 0; i < g_touchpad_info.emmc_mmio_count; i++) {
        if (g_touchpad_info.emmc_mmio[i] == base) return;
    }
    g_touchpad_info.emmc_mmio[g_touchpad_info.emmc_mmio_count++] = base;
}

/*
 * Crude AML scan: after HID "80860F14", look for Memory32Fixed (0x86) large
 * resource descriptors and collect plausible SDHCI windows (0x100..0x10000).
 */
static void extract_emmc_mmio_near_hid(const acpi_sdt_header_t* h) {
    const uint8_t* p = (const uint8_t*)h;
    uint32_t len = h->length;
    const char* needle = "80860F14";
    uint32_t nlen = 8;
    uint32_t i;

    for (i = 0; i + nlen <= len; i++) {
        uint32_t j = 0;
        uint32_t end;
        uint32_t k;
        while (j < nlen && p[i + j] == (uint8_t)needle[j]) j++;
        if (j != nlen) continue;

        end = i + 768;
        if (end > len) end = len;
        for (k = i; k + 12 <= end; k++) {
            uint16_t desc_len;
            uint32_t base;
            uint32_t res_len;
            if (p[k] != 0x86U) continue; /* Memory32Fixed large item */
            desc_len = (uint16_t)(p[k + 1] | ((uint16_t)p[k + 2] << 8));
            if (desc_len < 9) continue;
            /* flags at k+3, base at k+4, length at k+8 */
            base = (uint32_t)p[k + 4] |
                   ((uint32_t)p[k + 5] << 8) |
                   ((uint32_t)p[k + 6] << 16) |
                   ((uint32_t)p[k + 7] << 24);
            res_len = (uint32_t)p[k + 8] |
                      ((uint32_t)p[k + 9] << 8) |
                      ((uint32_t)p[k + 10] << 16) |
                      ((uint32_t)p[k + 11] << 24);
            acpi_add_emmc_mmio(base, res_len);
        }
    }
}

static void scan_aml_table(const acpi_sdt_header_t* h) {
    if (!table_header_ok(h)) return;
    /* Acer R3-131T: ELAN0501; Lenovo 80M4: ELAN0601; also generic ELAN*. */
    if (table_contains(h, "ELAN0501") || table_contains(h, "ELAN0601") ||
        table_contains(h, "ELAN0") || table_contains(h, "ELAN1") ||
        table_contains(h, "ELAN")) {
        g_touchpad_info.elan0601_found = 1;
    }
    if (table_contains(h, "PNP0C50")) g_touchpad_info.pnp0c50_found = 1;
    if (table_contains(h, "SYNA") || table_contains(h, "SYN0")) {
        g_touchpad_info.pnp0c50_found = 1;
    }
    if (table_contains(h, "80860F41")) g_touchpad_info.baytrail_i2c_found = 1;
    /* Braswell / Cherry Trail LPSS I2C ACPI HIDs (808622C1..808622C7). */
    if (table_contains(h, "808622C1") || table_contains(h, "808622C2") ||
        table_contains(h, "808622C3") || table_contains(h, "808622C4") ||
        table_contains(h, "808622C5") || table_contains(h, "808622C6") ||
        table_contains(h, "808622C7") || table_contains(h, "808622C")) {
        g_touchpad_info.braswell_i2c_found = 1;
    }
    if (table_contains(h, "80860F14")) {
        g_touchpad_info.baytrail_emmc_acpi = 1;
        extract_emmc_mmio_near_hid(h);
    }
}

static void scan_table_pointer(uintptr_t addr) {
    if (!addr) return;
    acpi_sdt_header_t* h = (acpi_sdt_header_t*)addr;
    /* DSDT/SSDT: accept despite bad OEM checksums (common on Insyde). */
    if (!table_header_ok(h)) return;
    if (memeq(h->signature, "DSDT", 4) || memeq(h->signature, "SSDT", 4)) {
        scan_aml_table(h);
    }
}

/*
 * FADT holds the DSDT pointer (Dsdt / X_Dsdt). Many firmwares list FACP in
 * RSDT/XSDT but never list DSDT as a top-level entry — Acer R3-131T is one.
 * Without following FACP we never see ELAN0501 / PNP0C50 in AML.
 */
static void scan_fadt_dsdt(const acpi_sdt_header_t* fadt) {
    const uint8_t* p;
    uint32_t dsdt32 = 0;
    uint64_t dsdt64 = 0;

    if (!table_valid(fadt) || !memeq(fadt->signature, "FACP", 4)) return;
    p = (const uint8_t*)fadt;

    /* DSDT at offset 40 (after 36-byte header + FirmwareCtrl). */
    if (fadt->length >= 44) {
        dsdt32 = (uint32_t)p[40] | ((uint32_t)p[41] << 8) |
                 ((uint32_t)p[42] << 16) | ((uint32_t)p[43] << 24);
        if (dsdt32) scan_table_pointer((uintptr_t)dsdt32);
    }

    /* X_DSDT at offset 140 for ACPI 2.0+ FADTs. */
    if (fadt->length >= 148) {
        dsdt64 = (uint64_t)p[140] | ((uint64_t)p[141] << 8) |
                 ((uint64_t)p[142] << 16) | ((uint64_t)p[143] << 24) |
                 ((uint64_t)p[144] << 32) | ((uint64_t)p[145] << 40) |
                 ((uint64_t)p[146] << 48) | ((uint64_t)p[147] << 56);
        if (dsdt64 && dsdt64 != (uint64_t)dsdt32) {
            if (sizeof(uintptr_t) == sizeof(uint64_t) || (dsdt64 >> 32) == 0)
                scan_table_pointer((uintptr_t)dsdt64);
        }
    }
}

static void scan_rsdt(const acpi_sdt_header_t* rsdt) {
    if (!table_valid(rsdt)) return;
    uint32_t entries = (rsdt->length - sizeof(acpi_sdt_header_t)) / 4;
    const uint32_t* p = (const uint32_t*)((const uint8_t*)rsdt + sizeof(acpi_sdt_header_t));
    for (uint32_t i = 0; i < entries && i < 64; i++) {
        uintptr_t addr = (uintptr_t)p[i];
        acpi_sdt_header_t* h;
        if (!addr) continue;
        h = (acpi_sdt_header_t*)addr;
        if (!table_valid(h)) continue;
        if (memeq(h->signature, "FACP", 4))
            scan_fadt_dsdt(h);
        scan_table_pointer(addr);
    }
}

static void scan_xsdt(const acpi_sdt_header_t* xsdt) {
    if (!table_valid(xsdt)) return;
    uint32_t entries = (xsdt->length - sizeof(acpi_sdt_header_t)) / 8;
    const uint64_t* p = (const uint64_t*)((const uint8_t*)xsdt + sizeof(acpi_sdt_header_t));
    for (uint32_t i = 0; i < entries && i < 64; i++) {
        uintptr_t addr;
        acpi_sdt_header_t* h;
        if (sizeof(uintptr_t) < sizeof(uint64_t) && (p[i] >> 32) != 0) continue;
        addr = (uintptr_t)p[i];
        if (!addr) continue;
        h = (acpi_sdt_header_t*)addr;
        if (!table_valid(h)) continue;
        if (memeq(h->signature, "FACP", 4))
            scan_fadt_dsdt(h);
        scan_table_pointer(addr);
    }
}

static void print_bool(const char* label, int v) {
    print(label);
    print(v ? "yes\n" : "no\n");
    driver_log(label);
    driver_log_line(v ? "yes" : "no");
}

void acpi_init(void) {
    g_touchpad_info.acpi_found = 0;
    g_touchpad_info.elan0601_found = 0;
    g_touchpad_info.pnp0c50_found = 0;
    g_touchpad_info.baytrail_i2c_found = 0;
    g_touchpad_info.braswell_i2c_found = 0;
    g_touchpad_info.baytrail_emmc_acpi = 0;
    g_touchpad_info.touchpad_i2c_addr = 0;
    g_touchpad_info.hid_desc_reg = 0x0001;
    g_touchpad_info.emmc_mmio_count = 0;
    for (int i = 0; i < ACPI_EMMC_MAX_MMIO; i++) g_touchpad_info.emmc_mmio[i] = 0;

    acpi_rsdp_t* rsdp = find_rsdp();
    if (!rsdp) {
        print("[acpi] RSDP not found; I2C touchpad discovery disabled.\n");
        driver_log_line("[acpi] RSDP not found; I2C touchpad discovery disabled.");
        return;
    }

    g_touchpad_info.acpi_found = 1;
    driver_log_line("[acpi] RSDP ok; scanning FADT/DSDT/SSDT for touchpad HIDs.");

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
        /* ELAN0501 (Acer) / ELAN0601 (Lenovo 80M4) common HID-I2C address. */
        g_touchpad_info.touchpad_i2c_addr = 0x15;
    }

    print("[acpi] touchpad discovery:\n");
    driver_log_line("[acpi] touchpad discovery:");
    print_bool("  ELAN*: ", g_touchpad_info.elan0601_found);
    print_bool("  PNP0C50: ", g_touchpad_info.pnp0c50_found);
    print_bool("  80860F41 I2C: ", g_touchpad_info.baytrail_i2c_found);
    print_bool("  808622Cx I2C: ", g_touchpad_info.braswell_i2c_found);
    print_bool("  80860F14 eMMC: ", g_touchpad_info.baytrail_emmc_acpi);
    if (g_touchpad_info.emmc_mmio_count > 0) {
        char buf[16];
        print("  eMMC MMIO candidates:");
        for (int i = 0; i < g_touchpad_info.emmc_mmio_count; i++) {
            print(" ");
            itoa((int)g_touchpad_info.emmc_mmio[i], buf, 16);
            print(buf);
        }
        print("\n");
    }
}

const acpi_touchpad_info_t* acpi_get_touchpad_info(void) {
    return &g_touchpad_info;
}
