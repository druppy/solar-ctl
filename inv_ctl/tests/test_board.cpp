/* Unit tests for inv_ctl's board identity probe (src/board.cpp).

   Like test_net.cpp, we #include the TU to reach its anonymous-namespace
   helpers - do NOT also compile src/board.cpp into this target.

   The golden inputs are byte-exact od -c captures from the bench Zero W
   (2026-10-06): both DT properties are ONE NUL-terminated string with no
   newline. Unlike the net fixtures these bytes contain an embedded NUL,
   so the stream fixture writes an explicit length. */
#include <catch2/catch_test_macros.hpp>

#include "../src/board.cpp"

#include <cstdio>

using namespace std;

namespace {

/* Replay arbitrary bytes (NULs included) as a stdio stream for the parse
 * functions; tmpfile() keeps it pure C stdio (see test_net.cpp rationale). */
class MemoryStream
{
public:
    explicit MemoryStream(const string_view bytes)
    {
        f_ = tmpfile();
        if (f_ && !bytes.empty()) {
            fwrite(bytes.data(), 1, bytes.size(), f_);
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

} // namespace

TEST_CASE("parse_dt_string: bench golden captures (embedded NUL)")
{
    const string_view model{"Raspberry Pi Zero W Rev 1.1\0", 28};
    const string_view serial{"0000000047ec1e46\0", 17};

    const MemoryStream ms_model{model};
    CHECK(parse_dt_string(ms_model.get()) == "Raspberry Pi Zero W Rev 1.1");

    const MemoryStream ms_serial{serial};
    CHECK(parse_dt_string(ms_serial.get()) == "0000000047ec1e46");
}

TEST_CASE("parse_dt_string: terminator variants and degenerate input")
{
    SECTION("trailing newline is tolerated (echo-created fixtures)")
    {
        const MemoryStream ms{"Raspberry Pi 4 Model B Rev 1.4\n"};
        CHECK(parse_dt_string(ms.get()) == "Raspberry Pi 4 Model B Rev 1.4");
    }
    SECTION("no trailing terminator at all")
    {
        const MemoryStream ms{"Raspberry Pi Zero W"};
        CHECK(parse_dt_string(ms.get()) == "Raspberry Pi Zero W");
    }
    SECTION("empty file (property present but blank)")
    {
        const MemoryStream ms{""};
        CHECK(parse_dt_string(ms.get()).empty());
    }
    SECTION("lone NUL byte")
    {
        const MemoryStream ms{string_view{"", 1}};
        CHECK(parse_dt_string(ms.get()).empty());
    }
}

TEST_CASE("looks_like_raspberry_pi: model gate")
{
    CHECK(looks_like_raspberry_pi("Raspberry Pi Zero W Rev 1.1"));
    CHECK(looks_like_raspberry_pi("Raspberry Pi"));  // bare prefix still a Pi
    CHECK_FALSE(looks_like_raspberry_pi(""));         // laptop: no DT model
    CHECK_FALSE(looks_like_raspberry_pi("Raspberry")); // incomplete token
    CHECK_FALSE(looks_like_raspberry_pi("SomeVendor Raspberry Pi clone"));
    CHECK_FALSE(looks_like_raspberry_pi("raspberry pi zero w")); // case exact
}

TEST_CASE("read_dt: absent file degrades to empty")
{
    CHECK(read_dt("/sys/firmware/devicetree/base/solar-no-such-property")
              .empty());
}

TEST_CASE("board_query: never contradicts its own gate")
{
    /* Environment-dependent by nature (a DT exists iff the machine has
     * one), so assert the invariant instead of a value: an empty BoardInfo
     * (laptop/CI, non-Pi board) or a model that is a Raspberry Pi. */
    const BoardInfo board = board_query();
    CHECK(board.model.empty() == board.serial.empty());
    CHECK((board.model.empty() || looks_like_raspberry_pi(board.model)));
}
