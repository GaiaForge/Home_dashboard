/**
 * Synthetic fleet data for the diagnostics UI mockup.
 *
 * ALL VALUES ARE FAKE. Serial formats follow the real convention
 * (ORF-BAS-XXX / ORF-PRO-XXX) so the layout is exercised at realistic text
 * widths. The failure modes chosen here are the ones actually worth designing
 * for: brownout on solar units, watchdog resets that mean "alive but not
 * working", and a failed OTA rollback.
 */
#include "diag_ui.h"

/* ---- ORF-BAS-003: the interesting one — died mid-sync, coredump waiting -- */
static const diag_event_t EV_BAS_003[] = {
    {"2026-07-25 04:41", "Panic (LoadProhibited)", "PC 0x420131ac, task: sync",  DIAG_PANIC},
    {"2026-07-25 04:41", "Boot",                   "coredump written, 64 KB",    DIAG_WARN},
    {"2026-07-24 22:08", "Power-on",               "scheduled wake",             DIAG_OK},
    {"2026-07-23 19:55", "Brownout",               "VBAT 3.21 V under load",     DIAG_WARN},
    {"2026-07-23 06:30", "Power-on",               "scheduled wake",             DIAG_OK},
};

/* ---- ORF-PRO-001: watchdog loop — the "alive but not working" failure ---- */
static const diag_event_t EV_PRO_001[] = {
    {"2026-07-25 09:14", "Watchdog (task)",  "audio task starved 8.2 s", DIAG_WARN},
    {"2026-07-25 07:02", "Watchdog (task)",  "audio task starved 7.9 s", DIAG_WARN},
    {"2026-07-25 01:47", "Watchdog (task)",  "audio task starved 8.4 s", DIAG_WARN},
    {"2026-07-24 20:30", "Power-on",         "manual reset",             DIAG_OK},
};

/* ---- ORF-BAS-007: healthy baseline ------------------------------------- */
static const diag_event_t EV_BAS_007[] = {
    {"2026-07-22 05:00", "Power-on", "scheduled wake", DIAG_OK},
    {"2026-07-15 05:00", "Power-on", "scheduled wake", DIAG_OK},
};

/* ---- ORF-BAS-005: went dark after an OTA ------------------------------- */
static const diag_event_t EV_BAS_005[] = {
    {"2026-07-22 11:20", "OTA rollback", "1.41 failed verify, back on 1.40", DIAG_PANIC},
    {"2026-07-22 11:12", "OTA reboot",   "applying 1.41",                    DIAG_WARN},
    {"2026-07-20 05:00", "Power-on",     "scheduled wake",                   DIAG_OK},
};

/* ---- HG pilot unit ------------------------------------------------------ */
static const diag_event_t EV_HG_02[] = {
    {"2026-07-25 06:00", "Power-on", "hive 2, north row", DIAG_OK},
};

const diag_device_t DIAG_DEVICES[] = {
    {
        "ORF-BAS-003", "Orpheus Basic", "1.40", DIAG_PANIC, -67, 47, "0d 05h", 41,
        "state=RECORDING  cmd=SYNC_START  buf=14/16",
        true, 65536, "just now",
        EV_BAS_003, sizeof(EV_BAS_003) / sizeof(EV_BAS_003[0]),
    },
    {
        "ORF-PRO-001", "Orpheus Pro", "1.40", DIAG_WARN, -72, 203, "0d 02h", 88,
        "state=ANALYZING  cmd=none  buf=16/16",
        false, 0, "just now",
        EV_PRO_001, sizeof(EV_PRO_001) / sizeof(EV_PRO_001[0]),
    },
    {
        "ORF-BAS-005", "Orpheus Basic", "1.40", DIAG_PANIC, -81, 118, "2d 22h", 63,
        "state=OTA_APPLY  cmd=OTA_BEGIN  slot=B",
        true, 65536, "12m ago",
        EV_BAS_005, sizeof(EV_BAS_005) / sizeof(EV_BAS_005[0]),
    },
    {
        "ORF-BAS-007", "Orpheus Basic", "1.40", DIAG_OK, -58, 12, "6d 04h", 94,
        "state=IDLE  cmd=none  buf=0/16",
        false, 0, "just now",
        EV_BAS_007, sizeof(EV_BAS_007) / sizeof(EV_BAS_007[0]),
    },
    {
        "HG-PILOT-02", "HiveGuard", "0.9.3", DIAG_OK, -64, 3, "0d 09h", 97,
        "state=LISTENING  cmd=none",
        false, 0, "just now",
        EV_HG_02, sizeof(EV_HG_02) / sizeof(EV_HG_02[0]),
    },
    {
        "ORF-BAS-002", "Orpheus Basic", "1.39", DIAG_OFFLINE, -99, 88, "—", -1,
        "—",
        false, 0, "3d ago",
        nullptr, 0,
    },
};

const uint8_t DIAG_DEVICE_COUNT = sizeof(DIAG_DEVICES) / sizeof(DIAG_DEVICES[0]);
