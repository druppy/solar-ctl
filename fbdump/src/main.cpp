// fbdump - capture the solar-ctl framebuffer and emit a PNG.
//
// Design rules:
//   * Zero dependencies beyond libc + the Linux fbdev ioctls. The PNG
//     container and its zlib stream are hand-rolled below using DEFLATE
//     *stored* (uncompressed) blocks, so no libpng/zlib needs to enter the
//     512 MB rootfs. Output is ~1.5x raw size - fine for a 142x428 panel
//     and fine over SSH.
//   * /dev/fb0 IS what the glass shows: panel-mipi-dbi streams it verbatim
//     (LVGL does all rotation in software). This PNG is display ground truth.
//   * Device defaults are baked in at compile time from CMake (FBDUMP_*);
//     the runtime ioctl reports the real geometry and wins over the baked-in
//     fallback; CLI flags win over everything.
//   * stdout carries only PNG bytes (add -b for base64); all chatter goes to
//     stderr, so `ssh board fbdump > shot.png` is clean. Command-form ssh
//     allocates no pty, so it is binary-safe; use -b when the bytes must
//     cross an interactive pty (raw RGB565 through one survives ~4 of
//     123 KB - see fw/docs/display-nv3007.md).
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/fb.h>

using namespace std;

#ifndef FBDUMP_DEV
#define FBDUMP_DEV "/dev/fb0"
#endif
#ifndef FBDUMP_W
#define FBDUMP_W 142
#endif
#ifndef FBDUMP_H
#define FBDUMP_H 428
#endif
#ifndef FBDUMP_BPP
#define FBDUMP_BPP 16
#endif

namespace
{

// ---------------------------------------------------------------- PNG parts

// IEEE 802.3 CRC32 (PNG chunks). Callers thread the pre-inverted state
// through successive chunks and finalize with one more bit of ~.
uint32_t crc32_iter(uint32_t state, const uint8_t* p, size_t n)
{
    static constexpr auto table = []
    {
        array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    for (; n--; ++p)
        state = table[(state ^ *p) & 0xFFu] ^ (state >> 8);
    return state;
}

uint32_t adler32(const vector<uint8_t>& d)
{
    uint32_t a = 1, b = 0;
    for (uint8_t v : d) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

void push_be32(vector<uint8_t>& v, uint32_t x)
{
    v.insert(v.end(),
             {uint8_t(x >> 24), uint8_t(x >> 16), uint8_t(x >> 8), uint8_t(x)});
}

// Minimal zlib stream: 2-byte header + DEFLATE *stored* blocks (BTYPE=00) +
// big-endian Adler-32. No compression: PNG accepts any valid zlib level, and
// level 0 buys zero-dep for ~15 lines. Raw pixels are incompressible enough
// anyway for a screenshot tool.
vector<uint8_t> zlib_stored(const vector<uint8_t>& raw)
{
    vector<uint8_t> z;
    z.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
    z.push_back(0x78);  // CM=8 (deflate), 32K window
    z.push_back(0x01);  // FCHECK completes the %31 header check; FLEVEL fastest
    size_t off = 0;
    do {  // at least one block, even for empty input (LEN=0, BFINAL=1)
        const size_t len = min<size_t>(raw.size() - off, 65535);
        z.push_back(off + len == raw.size() ? 0x01 : 0x00);  // BFINAL, BTYPE=00
        z.push_back(uint8_t(len & 0xFF));
        z.push_back(uint8_t(len >> 8));
        const auto nlen = uint16_t(~uint16_t(len));
        z.push_back(uint8_t(nlen & 0xFF));
        z.push_back(uint8_t(nlen >> 8));
        z.insert(z.end(), raw.begin() + ptrdiff_t(off),
                 raw.begin() + ptrdiff_t(off + len));
        off += len;
    } while (off < raw.size());
    push_be32(z, adler32(raw));
    return z;
}

// Byte sink: raw, or base64-wrapped in 76-char lines so the PNG can cross an
// interactive pty. `total` counts the RAW bytes fed in (pre-base64).
struct Sink
{
    Sink(FILE* stream, bool b64) : f(stream), base64(b64) {}

    bool write(const void* p, size_t n)
    {
        total += n;
        if (!base64)
            return fwrite(p, 1, n, f) == n;
        const auto* b = static_cast<const uint8_t*>(p);
        for (; n--; ++b)
            if (!byte(*b))
                return false;
        return true;
    }

    bool close()
    {
        if (base64 && ntri_ && !flush_tri())
            return false;
        if (base64 && col_ && !raw("\n", 1))
            return false;
        return fflush(f) == 0 && !ferror(f);
    }

    FILE* f;
    bool base64;
    uint64_t total = 0;

private:
    uint8_t tri_[3]{};
    int ntri_ = 0, col_ = 0;

    bool raw(const char* p, size_t n) { return fwrite(p, 1, n, f) == n; }

    bool byte(uint8_t b)
    {
        tri_[ntri_++] = b;
        if (ntri_ < 3)
            return true;
        return flush_tri();
    }

    bool flush_tri()
    {
        static constexpr char A[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const uint32_t v = (uint32_t(tri_[0]) << 16)
                              | (ntri_ > 1 ? uint32_t(tri_[1]) << 8 : 0)
                              | (ntri_ > 2 ? uint32_t(tri_[2]) : 0);
        const char c[4] = {A[(v >> 18) & 63], A[(v >> 12) & 63],
                           ntri_ > 1 ? A[(v >> 6) & 63] : '=',
                           ntri_ > 2 ? A[v & 63] : '='};
        ntri_ = 0;
        if (!raw(c, 4))
            return false;
        col_ += 4;
        if (col_ == 76) {
            col_ = 0;
            return raw("\n", 1);
        }
        return true;
    }
};

bool png_chunk(Sink& s, const char type[4], const vector<uint8_t>& data)
{
    vector<uint8_t> head;
    push_be32(head, uint32_t(data.size()));
    head.insert(head.end(), type, type + 4);
    uint32_t crc = crc32_iter(0xFFFFFFFFu,
                                   reinterpret_cast<const uint8_t*>(type), 4);
    crc = crc32_iter(crc, data.data(), data.size());
    vector<uint8_t> tail;
    push_be32(tail, ~crc);  // CRC trailer: length, type, data, crc - in that order
    return s.write(head.data(), head.size())
        && (data.empty() || s.write(data.data(), data.size()))
        && s.write(tail.data(), tail.size());
}

// ------------------------------------------------------------ framebuffer

struct Geo
{
    unsigned w = FBDUMP_W, h = FBDUMP_H, bpp = FBDUMP_BPP;
    unsigned ro = 11, rl = 5, go = 5, gl = 6, bo = 0, bl = 5;  // RGB565
    unsigned line_length = w * bpp / 8;
    unsigned xoff = 0, yoff = 0;  // pan/origin into the virtual screen
};

// Channel layout matching the compiled-in bpp (only used on the fallback and
// -g paths, where no fbioctl reported the real one).
void set_default_channels(Geo& g)
{
    switch (g.bpp) {
        case 32:
        case 24: g.ro = 16, g.rl = 8, g.go = 8, g.gl = 8, g.bo = 0, g.bl = 8; break;
        case 8:  g.ro = 0,  g.rl = 8, g.go = 0, g.gl = 8, g.bo = 0, g.bl = 8; break;  // gray
        default: g.bpp = 16; [[fallthrough]];
        case 16: g.ro = 11, g.rl = 5, g.go = 5, g.gl = 6, g.bo = 0, g.bl = 5; break;
    }
}

uint32_t chan(uint32_t p, unsigned off, unsigned len)
{
    if (!len)
        return 0;
    const uint32_t mask = len >= 32 ? ~0u : (1u << len) - 1;
    const uint32_t val = (p >> off) & mask;
    if (len >= 8)
        return val >> (len - 8);
    return (val * 255 + mask / 2) / mask;  // rescale 5/6-bit to 8, rounded
}

// Pixel word read, little-endian (all fbdev drivers in play, including
// panel-mipi-dbi's fb emulation, are LSB-first; msb_right is only warned on).
uint32_t pixel(const uint8_t* p, unsigned bpp)
{
    switch (bpp) {
        case 8:  return *p;
        case 16: { uint16_t v; memcpy(&v, p, 2); return v; }
        case 24: return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
        default: { uint32_t v; memcpy(&v, p, 4); return v; }
    }
}

// Convert an fb-sized buffer to RGB scanlines and emit the whole PNG through
// the sink (the caller still owns sink.close()). Geo by value: the yoff clamp
// is an internal correction, not something to leak back to the caller.
bool emit_png(Sink& sink, Geo g, const vector<uint8_t>& fb)
{
    // Visible window only; origin offsets apply if the backing store is
    // actually big enough to contain them.
    if (fb.size() < size_t(g.yoff + g.h) * g.line_length)
        g.yoff = 0;
    const size_t pxb = g.bpp / 8;

    vector<uint8_t> rows;
    rows.reserve(size_t(g.h) * (1 + size_t(g.w) * 3));
    for (unsigned y = 0; y < g.h; ++y) {
        rows.push_back(0);  // PNG scanline filter: None
        const uint8_t* rp
            = fb.data() + size_t(y + g.yoff) * g.line_length + size_t(g.xoff) * pxb;
        for (unsigned x = 0; x < g.w; ++x) {
            const uint32_t p = pixel(rp + size_t(x) * pxb, g.bpp);
            rows.push_back(uint8_t(chan(p, g.ro, g.rl)));
            rows.push_back(uint8_t(chan(p, g.go, g.gl)));
            rows.push_back(uint8_t(chan(p, g.bo, g.bl)));
        }
    }

    vector<uint8_t> ihdr;
    push_be32(ihdr, g.w);
    push_be32(ihdr, g.h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit, truecolor RGB, deflate, no interlace

    const vector<uint8_t> idat = zlib_stored(rows);
    bool ok = sink.write("\x89PNG\r\n\x1a\n", 8);
    ok = png_chunk(sink, "IHDR", ihdr) && ok;
    ok = png_chunk(sink, "IDAT", idat) && ok;
    return png_chunk(sink, "IEND", {}) && ok;
}

void usage(const char* argv0)
{
    fprintf(stderr,
        "usage: %s [-d DEV] [-o OUT] [-g WxH[@BPP]] [-b] [-h]\n"
        "  -d DEV   framebuffer device or raw capture file (baked default: %s)\n"
        "  -o OUT   output PNG file; '-' = stdout (default: stdout, so\n"
        "           'ssh board fbdump > shot.png' just works)\n"
        "  -g WxH[@BPP]  skip the fb ioctls (for raw dumps via -d); default:\n"
        "           query the device, fallback baked-in %dx%d@%u\n"
        "  -b       base64 the PNG (76-char lines) to cross an interactive pty\n"
        "  -h       this help\n",
        argv0, FBDUMP_DEV, FBDUMP_W, FBDUMP_H, FBDUMP_BPP);
}

}  // namespace

// The unit tests (tests/test_png.cpp) #include this file to reach the
// anonymous-namespace helpers and bring their own main (Catch2), so the
// real one compiles out there - same pattern as inv_ctl's test_net.cpp.
#ifndef FBDUMP_TESTS_BUILD
int main(int argc, char** argv)
{
    const char* dev = FBDUMP_DEV;
    const char* out = "-";
    bool base64 = false, geom_forced = false;
    Geo g;

    for (int i = 1; i < argc; ++i) {
        const string_view a = argv[i];
        auto next = [&](void) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "fbdump: %.*s needs an argument\n", int(a.size()), a.data());
                exit(2);
            }
            return argv[++i];
        };
        if (a == "-d" || a == "--dev")
            dev = next();
        else if (a == "-o" || a == "--out")
            out = next();
        else if (a == "-g" || a == "--geometry") {
            unsigned at = 0;
            if (sscanf(next(), "%ux%u@%u", &g.w, &g.h, &at) < 2) {
                fprintf(stderr, "fbdump: bad geometry (want WxH[@BPP])\n");
                return 2;
            }
            if (at)
                g.bpp = at;
            geom_forced = true;
        } else if (a == "-b" || a == "--base64")
            base64 = true;
        else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "fbdump: unknown argument '%.*s'\n", int(a.size()), a.data());
            usage(argv[0]);
            return 2;
        }
    }
    set_default_channels(g);

    FILE* outf = stdout;
    if (strcmp(out, "-") != 0 && !(outf = fopen(out, "wb"))) {
        fprintf(stderr, "fbdump: open %s: %s\n", out, strerror(errno));
        return 1;
    }
    Sink sink{outf, base64};

    const int fd = ::open(dev, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "fbdump: open %s: %s\n", dev, strerror(errno));
        if (outf != stdout) fclose(outf);
        return 1;
    }

    if (!geom_forced) {
        fb_var_screeninfo var{};
        fb_fix_screeninfo fix{};
        if (::ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0 && var.bits_per_pixel % 8 == 0) {
            ::ioctl(fd, FBIOGET_FSCREENINFO, &fix);  // best effort; fallback below
            const unsigned ll
                = fix.line_length ? fix.line_length : var.xres * var.bits_per_pixel / 8;
            g = Geo{.w = var.xres,
                    .h = var.yres,
                    .bpp = var.bits_per_pixel,
                    .ro = var.red.offset,
                    .rl = var.red.length,
                    .go = var.green.offset,
                    .gl = var.green.length,
                    .bo = var.blue.offset,
                    .bl = var.blue.length,
                    .line_length = ll,
                    .xoff = var.xoffset,
                    .yoff = var.yoffset};
            if (var.red.msb_right || var.green.msb_right || var.blue.msb_right)
                fprintf(stderr, "fbdump: warning: msb_right layout assumed lsb\n");
        } else {
            fprintf(stderr,
                "fbdump: %s: no usable fb geometry (%s) - using baked-in %ux%u@%u\n",
                dev, strerror(errno), g.w, g.h, g.bpp);
        }
    } else {
        g.line_length = g.w * g.bpp / 8;
    }

    vector<uint8_t> fb(size_t(g.h) * g.line_length);
    for (size_t off = 0; off < fb.size();) {
        const ssize_t r = ::read(fd, fb.data() + off, fb.size() - off);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "fbdump: read %s: %s\n", dev, strerror(errno));
            ::close(fd);
            if (outf != stdout) fclose(outf);
            return 1;
        }
        if (r == 0) {
            fprintf(stderr, "fbdump: short read (%zu of %zu bytes)\n", off, fb.size());
            break;
        }
        off += size_t(r);
    }
    ::close(fd);

    bool ok = emit_png(sink, g, fb);
    ok = sink.close() && ok;

    if (outf != stdout) fclose(outf);
    if (!ok) {
        fprintf(stderr, "fbdump: write to %s failed\n", out);
        return 1;
    }
    fprintf(stderr, "fbdump: %ux%u@%u -> %llu bytes PNG%s\n", g.w, g.h, g.bpp,
                 static_cast<unsigned long long>(sink.total), base64 ? " (base64)" : "");
    return 0;
}
#endif  // FBDUMP_TESTS_BUILD
