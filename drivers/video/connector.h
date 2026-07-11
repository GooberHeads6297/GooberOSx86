#ifndef CONNECTOR_H
#define CONNECTOR_H

#include <stdint.h>
#include "edid.h"

/*
 * Ubuntu/Windows Basic Display-style connector inventory.
 *
 * Phase 1 detects outputs (eDP, HDMI, DP, VGA, SimpleFB) via Intel GMBUS/HPD
 * or synthesizes a firmware SimpleFB connector. No DDI link training / cold
 * modeset — inherit firmware scanout only.
 */

#define DISPLAY_MAX_CONNECTORS 8

typedef enum {
    DISPLAY_CONN_UNKNOWN = 0,
    DISPLAY_CONN_EDP,
    DISPLAY_CONN_LVDS,
    DISPLAY_CONN_HDMI,
    DISPLAY_CONN_DP,
    DISPLAY_CONN_VGA,
    DISPLAY_CONN_SIMPLEFB
} display_connector_type_t;

typedef enum {
    DISPLAY_CONN_STATUS_UNKNOWN = 0,
    DISPLAY_CONN_STATUS_CONNECTED,
    DISPLAY_CONN_STATUS_DISCONNECTED
} display_connector_status_t;

typedef struct {
    int present;
    char name[16];                 /* e.g. "eDP-1", "HDMI-A-1", "DP-1" */
    display_connector_type_t type;
    display_connector_status_t status;
    uint32_t gmbus_pin;            /* 0 if not Intel GMBUS-backed */
    uint32_t port_index;           /* ordinal within type, 1-based in name */
    uint16_t preferred_width;
    uint16_t preferred_height;
    char monitor_name[14];
    int edid_valid;
    int hpd_known;                 /* 1 if hardware HPD bit was readable */
    int hpd_live;                  /* 1 if HPD asserted (when hpd_known) */
} display_connector_t;

/* Clear registry and rescan. allow_gmbus=0 skips Intel GMBUS/DDC writes —
 * required when a firmware LFB is already live (Bay Trail / Braswell / Lenovo
 * 80M4 hang and leave GRUB's blue gfxterm forever). */
void display_connectors_reset(void);
int  display_connectors_scan(void);              /* allow_gmbus=1 */
int  display_connectors_scan_ex(int allow_gmbus);

int  display_connectors_count(void);
const display_connector_t* display_connector_get(int index);

/* First connected connector with a preferred mode, or NULL. */
const display_connector_t* display_connector_preferred(void);

const char* display_connector_type_name(display_connector_type_t type);
const char* display_connector_status_name(display_connector_status_t status);

/* Ubuntu-style lines to serial / print sink. */
void display_connectors_log(void);

/* Register synthetic connectors when Intel display MMIO is unsafe (Bay Trail
 * with live firmware LFB). eDP reflects the active panel; HDMI/DP stay unknown
 * until a deferred scan from the shell after boot. */
void display_connectors_stub_firmware_panel(uint32_t width, uint32_t height);

/* Register a synthetic SimpleFB/Firmware connector from loader geometry. */
void display_connector_add_simplefb(uint32_t width, uint32_t height,
                                    const char* monitor_name);

#endif
