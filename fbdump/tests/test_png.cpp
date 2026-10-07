// Catch2 unit tests for fbdump - the anti-drift net for the hand-rolled
// PNG/zlib/base64 machinery (the reason the tool has zero deps is also the
// reason a regression would silently produce unopenable images).
//
// Harness pattern (same as inv_ctl/tests/test_net.cpp): #include the file
// under test to reach its anonymous-namespace helpers; FBDUMP_TESTS_BUILD
// compiles main.cpp's main() out (Catch2 provides main). Do NOT also compile
// src/main.cpp into this target - duplicate symbols.
#define FBDUMP_TESTS_BUILD 1
#include "../src/main.cpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>

using namespace std;

#ifndef FBDUMP_TEST_DIR
#define FBDUMP_TEST_DIR "."
#endif

namespace
{

string slurp(FILE* f)
{
    fflush(f);
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    rewind(f);
    string s(size_t(n > 0 ? n : 0), '\0');
    if (!s.empty() && fread(s.data(), 1, s.size(), f) != s.size())
        return {};
    return s;
}

string slurp(string_view path)
{
    // No ifstream(string_view) overload exists (C++23 included) - one
    // materialized copy at this boundary, same rule as every other sink.
    ifstream in(string(path), ios::binary);
    return string((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
}

uint32_t u32be(string_view s, size_t o)
{
    return (uint32_t(uint8_t(s[o])) << 24) | (uint32_t(uint8_t(s[o + 1])) << 16)
         | (uint32_t(uint8_t(s[o + 2])) << 8) | uint8_t(s[o + 3]);
}

string b64_of(string_view raw)
{
    FILE* tmp = tmpfile();
    REQUIRE(tmp != nullptr);
    Sink s{tmp, true};
    REQUIRE(s.write(raw.data(), raw.size()));
    REQUIRE(s.close());
    return slurp(tmp);
}

string png_of(const Geo& g, const vector<uint8_t>& fb)
{
    FILE* tmp = tmpfile();
    REQUIRE(tmp != nullptr);
    Sink s{tmp, false};
    REQUIRE(emit_png(s, g, fb));
    REQUIRE(s.close());
    return slurp(tmp);
}

Geo geo_4x4_rgb565()
{
    return Geo{.w = 4,
               .h = 4,
               .bpp = 16,
               .ro = 11,
               .rl = 5,
               .go = 5,
               .gl = 6,
               .bo = 0,
               .bl = 5,
               .line_length = 4 * 2,
               .xoff = 0,
               .yoff = 0};
}

// Independent walk over a zlib stream of STORED blocks: validates framing
// (header, LEN/NLEN complement, BFINAL, big-endian Adler) and returns the
// payload - deliberately not sharing code with zlib_stored's writer.
vector<uint8_t> unstore(const vector<uint8_t>& z)
{
    REQUIRE(z.size() >= 11);
    REQUIRE(z[0] == 0x78);
    REQUIRE(z[1] == 0x01);
    vector<uint8_t> out;
    size_t p = 2;
    bool final_block = false;
    while (!final_block) {
        final_block = z[p] & 0x01;
        REQUIRE((z[p] & 0x06) == 0);  // BTYPE must be 00 (stored)
        const uint16_t len = uint16_t(z[p + 1] | (z[p + 2] << 8));
        const uint16_t nlen = uint16_t(z[p + 3] | (z[p + 4] << 8));
        REQUIRE(uint16_t(~nlen) == len);
        out.insert(out.end(), z.begin() + ptrdiff_t(p) + 5,
                   z.begin() + ptrdiff_t(p + 5 + len));
        p += 5 + len;
    }
    REQUIRE(z.size() == p + 4);
    const uint32_t ad = (uint32_t(z[p]) << 24) | (uint32_t(z[p + 1]) << 16)
                           | (uint32_t(z[p + 2]) << 8) | z[p + 3];
    REQUIRE(ad == adler32(out));
    return out;
}

}  // namespace

TEST_CASE("crc32 matches the IEEE 802.3 check vector", "[crc]")
{
    const char s[] = "123456789";
    REQUIRE(~crc32_iter(0xFFFFFFFFu, reinterpret_cast<const uint8_t*>(s), 9)
            == 0xCBF43926u);
    // threading a split stream must equal the one-shot result
    const uint32_t a = crc32_iter(0xFFFFFFFFu, reinterpret_cast<const uint8_t*>(s), 4);
    REQUIRE(~crc32_iter(a, reinterpret_cast<const uint8_t*>(s + 4), 5) == 0xCBF43926u);
}

TEST_CASE("adler32 matches the zlib manual vector", "[zlib]")
{
    const string s = "Wikipedia";
    REQUIRE(adler32({s.begin(), s.end()}) == 0x11E60398u);
    const vector<uint8_t> empty;
    REQUIRE(adler32(empty) == 1u);  // initial state, never 0
}

TEST_CASE("zlib_stored framing round-trips, block-splits at 64 KiB", "[zlib]")
{
    for (const size_t n : {size_t(0), size_t(1), size_t(65535),
                                size_t(65536), size_t(200000)}) {
        vector<uint8_t> in(n);
        for (size_t i = 0; i < n; ++i)
            in[i] = uint8_t(i * 31 + 7);
        CHECK(unstore(zlib_stored(in)) == in);
    }
}

TEST_CASE("Sink base64 follows RFC 4648 vectors and wraps at 76 columns", "[base64]")
{
    CHECK(b64_of("") == "");
    CHECK(b64_of("f") == "Zg==\n");     // close() terminates the stream
    CHECK(b64_of("fo") == "Zm8=\n");
    CHECK(b64_of("foo") == "Zm9v\n");
    CHECK(b64_of("foobar") == "Zm9vYmFy\n");
    // 57 raw bytes = exactly one 76-char line wrapped MID-stream (not only
    // at close): pty-safety depends on short lines, not just a final flush.
    CHECK(b64_of(string(57, '\0')) == string(76, 'A') + "\n");
    CHECK(b64_of(string(60, '\0')) == string(76, 'A') + "\nAAAA\n");
}

TEST_CASE("Sink counts raw bytes, not base64 bytes", "[base64]")
{
    FILE* tmp = tmpfile();
    REQUIRE(tmp != nullptr);
    Sink s{tmp, true};
    REQUIRE(s.write("foobar", 6));
    CHECK(s.total == 6);
}

TEST_CASE("chan rescales fb channel widths to 8-bit", "[fb]")
{
    // RGB565 anchors and the 1-LSB steps of the rounded *255/mask curve
    CHECK(chan(0x0000u, 11, 5) == 0);
    CHECK(chan(0xF800u, 11, 5) == 255);
    CHECK(chan(0x07E0u, 5, 6) == 255);
    CHECK(chan(0x001Fu, 0, 5) == 255);
    CHECK(chan(1u << 11, 11, 5) == 8);  // (255+15)/31
    CHECK(chan(1u << 5, 5, 6) == 4);    // (255+31)/63
    // 8-bit passthrough and a zero-length channel (no blue in gray modes)
    CHECK(chan(0x00FF0000u, 16, 8) == 255);
    CHECK(chan(0x00123456u, 0, 0) == 0);
}

TEST_CASE("pixel reads little-endian words per bpp", "[fb]")
{
    const uint8_t p16[] = {0x1F, 0x00};
    const uint8_t p24[] = {0x78, 0x56, 0x34};
    const uint8_t p32[] = {0x11, 0x22, 0x33, 0x44};
    const uint8_t p8[] = {0x99};
    CHECK(pixel(p16, 16) == 0x001Fu);
    CHECK(pixel(p24, 24) == 0x345678u);
    CHECK(pixel(p32, 32) == 0x44332211u);
    CHECK(pixel(p8, 8) == 0x99u);
}

TEST_CASE("set_default_channels matches bpp or coerces to RGB565", "[fb]")
{
    Geo g;
    g.bpp = 16;
    set_default_channels(g);
    CHECK((g.ro == 11 && g.rl == 5 && g.go == 5 && g.gl == 6 && g.bo == 0 && g.bl == 5));
    g.bpp = 32;
    set_default_channels(g);
    CHECK((g.ro == 16 && g.rl == 8 && g.go == 8 && g.gl == 8 && g.bo == 0 && g.bl == 8));
    g.bpp = 15;  // not byte-aligned: coerced to the 565 fallback
    set_default_channels(g);
    CHECK(g.bpp == 16);
    CHECK((g.rl == 5 && g.gl == 6 && g.bl == 5));
}

TEST_CASE("png_chunk frames length/type/data/crc in spec order", "[png]")
{
    FILE* tmp = tmpfile();
    REQUIRE(tmp != nullptr);
    Sink s{tmp, false};
    REQUIRE(png_chunk(s, "abcd", {0xAA, 0x00}));
    REQUIRE(s.close());
    const string out = slurp(tmp);
    REQUIRE(out.size() == 4 + 4 + 2 + 4);
    CHECK(u32be(out, 0) == 2);  // BE payload length
    CHECK(out.substr(4, 4) == "abcd");
    CHECK(uint8_t(out[8]) == 0xAA);  // data after type
    CHECK(uint8_t(out[9]) == 0x00);
    const string td = out.substr(4, 6);  // CRC covers type+data only
    CHECK(u32be(out, 10)
          == ~crc32_iter(0xFFFFFFFFu, reinterpret_cast<const uint8_t*>(td.data()),
                         td.size()));
}

TEST_CASE("emit_png reproduces the checked-in golden PNG byte-exactly", "[golden]")
{
    const string raw = slurp(string(FBDUMP_TEST_DIR) + "/gradient_4x4.raw");
    REQUIRE(raw.size() == 32);  // fixture sanity
    const vector<uint8_t> fb(raw.begin(), raw.end());

    const string want = slurp(string(FBDUMP_TEST_DIR) + "/golden_4x4.png");
    REQUIRE_FALSE(want.empty());

    CHECK(png_of(geo_4x4_rgb565(), fb) == want);
}

TEST_CASE("golden PNG stands on its own: chunk CRCs, stored IDAT, pixel values", "[golden]")
{
    // The golden is the byte-exact anchor; this test validates the anchor
    // itself, so it cannot rot into a corrupt-but-unchanged file.
    const string d = slurp(string(FBDUMP_TEST_DIR) + "/golden_4x4.png");
    REQUIRE(d.size() > 8);
    REQUIRE(d.substr(0, 8) == string("\x89PNG\r\n\x1a\n", 8));

    size_t off = 8, chunks = 0;
    vector<uint8_t> idat;
    while (off + 12 <= d.size()) {
        const uint32_t len = u32be(d, off);
        REQUIRE(off + 12 + len <= d.size());
        const string type = d.substr(off + 4, 4);
        const string td = d.substr(off + 4, 4 + len);
        REQUIRE(u32be(d, off + 8 + len)
                == ~crc32_iter(0xFFFFFFFFu,
                               reinterpret_cast<const uint8_t*>(td.data()), td.size()));
        if (type == "IDAT")
            idat.insert(idat.end(), td.begin() + 4, td.end());
        ++chunks;
        off += 12 + len;
    }
    CHECK(chunks == 3);  // IHDR, IDAT, IEND
    CHECK(off == d.size());

    const vector<uint8_t> rows = unstore(idat);
    REQUIRE(rows.size() == 4 * (1 + 4 * 3));  // 4 scanlines, filter byte + RGB888
    for (int y = 0; y < 4; ++y)
        CHECK(rows[size_t(y) * 13] == 0);  // filter: None

    // Spot-check the conversion against the fixture: px(1,0) = RGB565
    // r=8,g=0,b=4 -> rounded 8-bit (66, 0, 33).
    CHECK(rows[1 + 3 * 1 + 0] == 66);
    CHECK(rows[1 + 3 * 1 + 1] == 0);
    CHECK(rows[1 + 3 * 1 + 2] == 33);
}
