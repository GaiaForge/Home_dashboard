/**
 * Persistent store of known WiFi networks, backed by ESP32 NVS (flash).
 *
 * Survives reboots and reflashes. Most-recently-connected network is kept at
 * index 0, so auto-connect can just try get(0) first. Passwords are stored in
 * NVS in the clear — same trust model as any consumer device that remembers
 * WiFi; the flash isn't readable without physical access + debug tools.
 */
#pragma once

#include <Arduino.h>

#define WIFI_STORE_MAX 8

struct SavedNet {
    char ssid[33];
    char pass[65];
};

class WifiStore {
public:
    void load();                         /* read all saved networks from NVS */
    int  count() const { return m_n; }
    const SavedNet &get(int i) const { return m_nets[i]; }

    /** If ssid is saved, copy its password into pass_out (>=65 bytes). */
    bool find(const char *ssid, char *pass_out) const;

    /** Add or update; moves it to the front (most-recent). Persists. */
    void add(const char *ssid, const char *pass);

    /** Forget a network by SSID. Persists. */
    void remove(const char *ssid);

private:
    SavedNet m_nets[WIFI_STORE_MAX];
    int      m_n = 0;
    void     persist();
};
