/* Unit tests for inv_ctl's network probes.

   We include net.cpp directly: its query and parse helpers live in an
   anonymous namespace (correct for the app; invisible to a test link), and
   including the TU keeps production code untouched. Do NOT also compile
   src/net.cpp into this target - duplicate symbols.

   The golden inputs below are byte-exact captures (od -c) from the bench
   Zero W (kernel 6.18, brcmfmac), plus the older padded-name variant.
   These encode traps found the hard way on the glass, 2026-10-02:
   leading space, padded name, "44/70" quality, "-67." trailing-dot level,
   0000 status on an associated mac80211 iface, "--"/"99/99" markers. */
#include <catch2/catch_test_macros.hpp>

#include "../src/net.cpp"

#include <cstdio>
#include <cstring>

using namespace std;

namespace {

/* Replay NUL-free text as a stdio stream for the parse functions.
 * tmpfile() keeps this pure C stdio: fmemopen would be hidden under the
 * strict -std=c++23 feature-test macros glibc applies. */
class MemoryStream
{
public:
    explicit MemoryStream(const char * text)
    {
        f_ = tmpfile();
        if (f_) {
            fwrite(text, 1, strlen(text), f_);
            rewind(f_);
        }
    }
    ~MemoryStream()
    {
        if (f_)
            fclose(f_);
    }
    MemoryStream(const MemoryStream &) = delete;
    MemoryStream & operator=(const MemoryStream &) = delete;
    FILE * get() const { return f_; }

private:
    FILE * f_ = nullptr;
};

bool parse(const char * text, const char * ifname, int & dbm)
{
    const MemoryStream ms(text);
    return parse_wext(ms.get(), ifname, dbm);
}

constexpr const char * kHeader =
    "Inter-| sta-|   Quality        |   Discarded packets               | Missed | WE\n"
    " face | tus | link level noise |  nwid  crypt   frag  retry   misc | beacon | 22\n";

/* Bench capture, /proc/net/wireless, kernel 6.18 + brcmfmac, associated. */
constexpr const char * kBenchLine = " wlan0: 0000   43.  -67.  -256        0      0      0      1      0        0\n";

} // namespace

TEST_CASE("parse_wext: bench golden capture")
{
    const string all = string(kHeader) + kBenchLine;
    int dbm = 0;
    REQUIRE(parse(all.c_str(), "wlan0", dbm));
    CHECK(dbm == -67);
}

TEST_CASE("parse_wext: iface name column variants")
{
    int dbm = 0;
    SECTION("classic padded name (\" %-8s:\")")
    {
        REQUIRE(parse(" wlan0   : 0000   44/70  -50.  -256        0      0      0      0      0        0\n",
                      "wlan0", dbm));
        CHECK(dbm == -50); // also proves "44/70" quality is skipped
    }
    SECTION("no padding (bench 6.18)")
    {
        REQUIRE(parse(kBenchLine, "wlan0", dbm));
        CHECK(dbm == -67);
    }
    SECTION("no leading space")
    {
        REQUIRE(parse("wlan0: 0000   40.  -44.  -256        0\n", "wlan0", dbm));
        CHECK(dbm == -44);
    }
}

TEST_CASE("parse_wext: picks the requested iface among several")
{
    const string multi = string(kHeader) +
        " eth0  : 0000   10.  -11.  -256        0      0      0      0      0        0\n" +
        kBenchLine;
    int dbm = 0;
    REQUIRE(parse(multi.c_str(), "wlan0", dbm));
    CHECK(dbm == -67);
    REQUIRE(parse(multi.c_str(), "eth0", dbm));
    CHECK(dbm == -11);
    CHECK_FALSE(parse(multi.c_str(), "wlan1", dbm));
}

TEST_CASE("parse_wext: status column is ignored (mac80211 trap)")
{
    /* associated mac80211 prints status 0000; link state must NOT be
     * inferred from it - parse_wext simply never looks at it. */
    int dbm = 0;
    REQUIRE(parse(kBenchLine, "wlan0", dbm));
    CHECK(dbm == -67);
}

TEST_CASE("parse_wext: invalid level markers are rejected")
{
    int dbm = 12345;
    SECTION("brcmfmac while disconnected: '--'")
    {
        CHECK_FALSE(parse(" wlan0: 0000   99/99  --  --\n", "wlan0", dbm));
    }
    SECTION("slash pair '99/99' is not a plain number")
    {
        CHECK_FALSE(parse(" wlan0: 0000   99/99  99/99  -256\n", "wlan0", dbm));
    }
    SECTION("double trailing dot is garbage, not a number")
    {
        CHECK_FALSE(parse(" wlan0: 0000   43.  -67..  -256\n", "wlan0", dbm));
    }
    SECTION("level token missing entirely (short line)")
    {
        CHECK_FALSE(parse(" wlan0: 0000   43.\n", "wlan0", dbm));
    }
    CHECK(dbm == 12345); // output untouched on rejection
}

TEST_CASE("parse_wext: level without trailing dot also parses")
{
    int dbm = 0;
    REQUIRE(parse(" wlan0: 0000   70  -55  -256\n", "wlan0", dbm));
    CHECK(dbm == -55);
}

TEST_CASE("parse_wext: degenerate input")
{
    int dbm = 0;
    CHECK_FALSE(parse("", "wlan0", dbm));
    CHECK_FALSE(parse(kHeader, "wlan0", dbm));
    CHECK_FALSE(parse(" wlan0 no colon at all\n", "wlan0", dbm));
    CHECK_FALSE(parse(kBenchLine, "wlan0 ", dbm)); // exact match only
}

TEST_CASE("query_* degrade cleanly on an absent iface")
{
    CHECK(query_ssid("solar-if-9") == "--");
    CHECK(query_ipv4("solar-if-9") == "--");
    CHECK_FALSE(query_carrier("solar-if-9"));
    int dbm = 42;
    CHECK_FALSE(query_signal("solar-if-9", dbm));
}

TEST_CASE("net_refresh: absent iface fully degrades")
{
    NetStatus s;
    net_refresh(s, "solar-if-9");
    CHECK(s.ssid == "--");
    CHECK(s.ipv4 == "--");
    CHECK_FALSE(s.linked);
    CHECK(s.signal_dbm == 0);
}

TEST_CASE("net_refresh: loopback sanity")
{
    /* The runner always has lo up with 127.0.0.1: exercises the real
     * getifaddrs path. Note we do NOT assert linked - lo's carrier is 1. */
    NetStatus s;
    net_refresh(s, "lo");
    CHECK(s.ipv4 == "127.0.0.1");
    CHECK(s.ssid == "--");    // lo has no wireless extensions
    CHECK(s.signal_dbm == 0); // lo never appears in /proc/net/wireless
}

TEST_CASE("query_carrier: consistent with link presence, never crashes")
{
    /* lo carrier is 1 exactly when lo is UP - same truth as its IPv4. */
    CHECK(query_carrier("lo") == (query_ipv4("lo") == "127.0.0.1"));
}
