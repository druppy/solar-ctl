#include "net.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <linux/wireless.h>

namespace {

std::string query_ssid(const char * ifname)
{
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return "--";

    char buf[IW_ESSID_MAX_SIZE + 1] = {};
    struct iwreq wrq = {};
    std::strncpy(wrq.ifr_name, ifname, IFNAMSIZ);
    wrq.u.essid.pointer = buf;
    wrq.u.essid.length = sizeof(buf);

    std::string ssid = "--";
    if (ioctl(fd, SIOCGIWESSID, &wrq) == 0 && buf[0] != '\0')
        ssid = buf;

    close(fd);
    return ssid;
}

/* /proc/net/wireless line format (net/wireless/wext-proc.c):
 *   "%-8s: "  status  link_quality  level(dBm)  noise(dBm) ...
 * The name column is space-padded BEFORE the colon and the link-quality
 * column can print as a slash pair ("44/70"), so parse token-wise: trim
 * the name, skip status and quality, take level as the third token.
 * Do NOT read the status column for link state: mac80211 drivers
 * (brcmfmac included) leave it at 0000 even when fully associated
 * (bench 2026-10-02: UI said "no link" while iw said Connected); it is
 * only ever filled by a few legacy ad-hoc drivers. query_carrier() is
 * the authoritative link indicator. */
bool query_signal(const char * ifname, int & dbm)
{
    FILE * f = std::fopen("/proc/net/wireless", "re");
    if (!f)
        return false;

    bool found = false;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        /* wext-proc prints the name as " %-8s:": LEADING space AND
         * trailing padding — trim BOTH ends before strcmp, or the line
         * never matches and the panel shows 0 dBm (bench 2026-10-02). */
        char * name = line;
        while (*name && std::isspace(static_cast<unsigned char>(*name)))
            ++name;
        char * sep = std::strchr(name, ':');
        if (!sep)
            continue;
        char * end = sep;
        while (end > name && std::isspace(static_cast<unsigned char>(end[-1])))
            --end;
        *end = '\0';
        if (std::strcmp(name, ifname) != 0)
            continue;

        char tok_level[32] = {};
        if (std::sscanf(sep + 1, "%*31s %*31s %31s", tok_level) != 1)
            continue;
        char * endp = nullptr;
        const double level = std::strtod(tok_level, &endp);
        if (endp == tok_level || *endp != '\0')
            continue; // invalid marker ("99/99", "--"), not a plain number
        dbm = static_cast<int>(level);
        found = true;
    }
    std::fclose(f);
    return found;
}

/* cfg80211 drives carrier on association, so /sys/.../carrier is 1 exactly
 * when associated. Reading it while the iface is down fails with EINVAL,
 * which is simply "no link". */
bool query_carrier(const char * ifname)
{
    char path[128];
    std::snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", ifname);
    FILE * f = std::fopen(path, "re");
    if (!f)
        return false;
    int carrier = 0;
    if (std::fscanf(f, "%d", &carrier) != 1)
        carrier = 0;
    std::fclose(f);
    return carrier == 1;
}

std::string query_ipv4(const char * ifname)
{
    struct ifaddrs * addrs = nullptr;
    if (getifaddrs(&addrs) != 0)
        return "--";

    std::string ip = "--";
    for (const struct ifaddrs * a = addrs; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET ||
            std::strcmp(a->ifa_name, ifname) != 0)
            continue;
        char buf[INET_ADDRSTRLEN] = {};
        const auto * sin = reinterpret_cast<const struct sockaddr_in *>(a->ifa_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)))
            ip = buf;
        break;
    }
    freeifaddrs(addrs);
    return ip;
}

} // namespace

void net_refresh(NetStatus & status, const std::string & ifname)
{
    status.ssid = query_ssid(ifname.c_str());
    status.ipv4 = query_ipv4(ifname.c_str());
    status.linked = query_carrier(ifname.c_str());
    if (!query_signal(ifname.c_str(), status.signal_dbm))
        status.signal_dbm = 0;
}
