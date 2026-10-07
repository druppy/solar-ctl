#include "net.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>

#include <linux/wireless.h>

using namespace std;

namespace {

string query_ssid(const char * ifname)
{
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return "--";

    char buf[IW_ESSID_MAX_SIZE + 1] = {};
    struct iwreq wrq = {};
    strncpy(wrq.ifr_name, ifname, IFNAMSIZ);
    wrq.u.essid.pointer = buf;
    wrq.u.essid.length = sizeof(buf);

    string ssid = "--";
    if (ioctl(fd, SIOCGIWESSID, &wrq) == 0 && buf[0] != '\0')
        ssid = buf;

    close(fd);
    return ssid;
}

/* /proc/net/wireless (net/wireless/wext-proc.c). Bench-captured bytes on
 * kernel 6.18 + brcmfmac (od -c), iface line:
 *     " wlan0: 0000   43.  -67.  -256        0 ...\n"
 * i.e. a LEADING space, the iface token, ':' (older kernels space-pad the
 * name column to width 8 — trim the name at BOTH ends either way), then
 * status / link-quality / level(dBm) / noise. Traps, all fixed 2026-10-02
 * after the glass reported them:
 *   - the link-quality column may print as a slash pair ("44/70");
 *   - the level prints WITH a trailing dot ("-66.") — strtod eats it;
 *     invalid markers ("--", "99/99") must be rejected;
 *   - the status token is 0000 for ALL mac80211 drivers (brcmfmac
 *     included) even when fully associated — it is a legacy ad-hoc-only
 *     field, never use it for link state; query_carrier() is the
 *     authoritative link indicator.
 * Pure parse over an open stream so unit tests can feed captured bytes. */
bool parse_wext(FILE * f, const char * ifname, int & dbm)
{
    /* One line, readable: optional leading space, iface name, ':' (older
     * kernels pad the name column to width 8), then status and quality
     * tokens - skipped blind - then the level: a signed integer with an
     * OPTIONAL trailing dot (brcmfmac prints "-67."). The (?:\s|$) tail is
     * load-bearing: it is what rejects the invalid markers ("--", "99/99")
     * AND a malformed "-67..", because the level token must end exactly at
     * whitespace or end-of-line. Status is never inspected: mac80211
     * drivers leave it at 0000 even when associated (query_carrier() is
     * the link indicator). */
    static const regex line_re(
        R"(^\s*([^:\s]+)\s*:\s*\S+\s+\S+\s+(-?[0-9]+\.?)(?:\s|$))");

    bool found = false;
    char line[256] = {};
    while (fgets(line, sizeof(line), f)) {
        cmatch m; // cmatch, not smatch: raw char* line, not string
        if (!regex_search(line, m, line_re) || m[1].str() != ifname)
            continue; // header row, other iface, or level not a plain number
        dbm = static_cast<int>(strtol(m[2].str().c_str(), nullptr, 10));
        found = true;
    }
    return found;
}

bool query_signal(const char * ifname, int & dbm)
{
    FILE * f = fopen("/proc/net/wireless", "re");
    if (!f)
        return false;
    const bool found = parse_wext(f, ifname, dbm);
    fclose(f);
    return found;
}

/* cfg80211 drives carrier on association, so /sys/.../carrier is 1 exactly
 * when associated. Reading it while the iface is down fails with EINVAL,
 * which is simply "no link". */
bool parse_carrier(FILE * f)
{
    int carrier = 0;
    if (fscanf(f, "%d", &carrier) != 1)
        carrier = 0;
    return carrier == 1;
}

bool query_carrier(const char * ifname)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", ifname);
    FILE * f = fopen(path, "re");
    if (!f)
        return false;
    const bool linked = parse_carrier(f);
    fclose(f);
    return linked;
}

string query_ipv4(const char * ifname)
{
    struct ifaddrs * addrs = nullptr;
    if (getifaddrs(&addrs) != 0)
        return "--";

    string ip = "--";
    for (const struct ifaddrs * a = addrs; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET ||
            strcmp(a->ifa_name, ifname) != 0)
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

void net_refresh(NetStatus & status, string_view ifname)
{
    /* C boundary: the wext ioctl and the sysfs path want null-terminated
     * char arrays; string_view carries no such guarantee, so materialize
     * one fixed-size copy (iface names are IFNAMSIZ-bounded anyway). */
    char ifn[IFNAMSIZ] = {};
    memcpy(ifn, ifname.data(), min(ifname.size(), size_t(IFNAMSIZ - 1)));

    status.ssid = query_ssid(ifn);
    status.ipv4 = query_ipv4(ifn);
    status.linked = query_carrier(ifn);
    if (!query_signal(ifn, status.signal_dbm))
        status.signal_dbm = 0;
}
