#pragma once

#include <string>

/* Wi-Fi / network status for the status panel, read via ioctl + procfs
 * (no extra runtime deps; every field degrades to "--"/unlinked when the
 * interface does not exist, e.g. on the laptop). */
struct NetStatus {
    std::string ssid = "--";
    std::string ipv4 = "--";
    int signal_dbm = 0;
    bool linked = false;
};

void net_refresh(NetStatus & status, const std::string & ifname);
