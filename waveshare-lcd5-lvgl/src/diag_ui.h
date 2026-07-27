/**
 * GaiaForge Field Diagnostics — UI mockup
 *
 * Screens only: every value below is synthetic (see diag_mock.cpp). The point
 * of this file is the DATA MODEL — these structs are deliberately shaped like
 * what a real transport would return, so swapping mock data for a live BLE /
 * RS485 fetch means implementing a fill function, not rewriting the UI.
 *
 * Field-tool constraints baked into the layout:
 *   - Touch targets >= 60 px tall (gloved hands).
 *   - High contrast, no thin light-on-light text (outdoor glare).
 *   - Nothing critical hidden behind a gesture; taps only.
 */
#pragma once

#include <lvgl.h>
#include <stdint.h>

/** Health of a unit, worst-case across its recent history. */
typedef enum {
    DIAG_OK = 0,
    DIAG_WARN,
    DIAG_PANIC,
    DIAG_OFFLINE,
} diag_status_t;

/**
 * One entry from the device's reset-cause ring buffer.
 * On real hardware this comes out of NVS — see the field-debuggability-first
 * pattern: boot count + reset causes persisted across reboots.
 */
typedef struct {
    const char   *ts;        /**< "2026-07-24 03:12" — device RTC, not ours */
    const char   *cause;     /**< "Brownout", "Panic (LoadProhibited)", ... */
    const char   *detail;    /**< free text, may be "" */
    diag_status_t severity;
} diag_event_t;

/** Everything the tool knows about one unit after a fetch. */
typedef struct {
    const char   *serial;        /**< ORF-BAS-003 etc. */
    const char   *product;       /**< "Orpheus Basic" */
    const char   *fw;            /**< "1.40" */
    diag_status_t status;
    int           rssi;          /**< dBm, link quality at fetch time */
    uint32_t      boot_count;
    const char   *uptime;        /**< preformatted, e.g. "6d 04h" */
    int           battery_pct;   /**< -1 if the unit doesn't report one */

    /* Retained-RAM breadcrumb: what the unit was doing when it died. */
    const char   *breadcrumb;

    bool          has_coredump;
    uint32_t      coredump_bytes;

    const char   *last_seen;     /**< "just now", "3d ago" */

    const diag_event_t *events;
    uint8_t             event_count;
} diag_device_t;

/** Mock inventory (defined in diag_mock.cpp). */
extern const diag_device_t DIAG_DEVICES[];
extern const uint8_t       DIAG_DEVICE_COUNT;

/** Build the whole UI on the active screen. Call with the LVGL lock held. */
void diag_ui_create(void);
