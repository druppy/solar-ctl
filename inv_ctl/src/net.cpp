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
 * the name, skip quality wholesale, take level as the third token. */
bool query_signal(const char * ifname, int & dbm, bool & linked)
{
    FILE * f = std::fopen("/proc/net/wireless", "re");
    if (!f)
        return false;

    bool found = false;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        char * sep = std::strchr(line, ':');
        if (!sep)
            continue;
        char * end = sep;
        while (end > line && std::isspace(static_cast<unsigned char>(end[-1])))
            --end;
        *end = '\0';
        if (std::strcmp(line, ifname) != 0)
            continue;

        char tok_status[32] = {}, tok_level[32] = {};
        if (std::sscanf(sep + 1, "%31s %*31s %31s", tok_status, tok_level) != 2)
            continue;
        char * endp = nullptr;
        const double level = std::strtod(tok_level, &endp);
        if (endp == tok_level || *endp != '\0')
            continue; // invalid marker ("99/99", "--"), not a plain number
        dbm = static_cast<int>(level);
        linked = std::atoi(tok_status) != 0;
        found = true;
    }
    std::fclose(f);
    return found;
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
    if (!query_signal(ifname.c_str(), status.signal_dbm, status.linked)) {
        status.signal_dbm = 0;
        status.linked = false;
    }
}
