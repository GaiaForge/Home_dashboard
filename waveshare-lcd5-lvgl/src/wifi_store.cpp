/**
 * WiFi network store — NVS-backed. See wifi_store.h.
 */
#include "wifi_store.h"
#include <Preferences.h>

#define NS "wifinets"

void WifiStore::load()
{
    Preferences p;
    p.begin(NS, true);                 /* read-only */
    int c = p.getInt("count", 0);
    m_n = 0;
    for (int i = 0; i < c && m_n < WIFI_STORE_MAX; i++) {
        char k[8];
        snprintf(k, sizeof k, "s%d", i);
        String s = p.getString(k, "");
        snprintf(k, sizeof k, "p%d", i);
        String pw = p.getString(k, "");
        if (s.length()) {
            strlcpy(m_nets[m_n].ssid, s.c_str(), sizeof m_nets[m_n].ssid);
            strlcpy(m_nets[m_n].pass, pw.c_str(), sizeof m_nets[m_n].pass);
            m_n++;
        }
    }
    p.end();
}

void WifiStore::persist()
{
    Preferences p;
    p.begin(NS, false);
    p.clear();
    p.putInt("count", m_n);
    for (int i = 0; i < m_n; i++) {
        char k[8];
        snprintf(k, sizeof k, "s%d", i);
        p.putString(k, m_nets[i].ssid);
        snprintf(k, sizeof k, "p%d", i);
        p.putString(k, m_nets[i].pass);
    }
    p.end();
}

bool WifiStore::find(const char *ssid, char *pass_out) const
{
    for (int i = 0; i < m_n; i++) {
        if (strcmp(m_nets[i].ssid, ssid) == 0) {
            strlcpy(pass_out, m_nets[i].pass, 65);
            return true;
        }
    }
    return false;
}

void WifiStore::add(const char *ssid, const char *pass)
{
    SavedNet tmp[WIFI_STORE_MAX];
    int w = 0;
    strlcpy(tmp[0].ssid, ssid, sizeof tmp[0].ssid);
    strlcpy(tmp[0].pass, pass, sizeof tmp[0].pass);
    w = 1;
    for (int i = 0; i < m_n && w < WIFI_STORE_MAX; i++) {
        if (strcmp(m_nets[i].ssid, ssid) != 0) tmp[w++] = m_nets[i];
    }
    memcpy(m_nets, tmp, sizeof(SavedNet) * w);
    m_n = w;
    persist();
}

void WifiStore::remove(const char *ssid)
{
    int w = 0;
    for (int i = 0; i < m_n; i++) {
        if (strcmp(m_nets[i].ssid, ssid) != 0) m_nets[w++] = m_nets[i];
    }
    m_n = w;
    persist();
}
