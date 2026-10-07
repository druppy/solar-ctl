/* Unit tests for inv_ctl's SWUpdate progress IPC client (src/update.cpp).

   Like test_net.cpp we include the .cpp directly (reaches the anonymous
   namespace; do NOT also compile src/update.cpp into this target).

   There is no local copy of the wire format to anchor against any more -
   that is the point: fixtures use swupdate's OWN structs (progress_ipc.h,
   pinned include/ tree natively, swupdate-dev in the Yocto build), so a
   swupdate bump moves fixtures and production code together. What is
   tested here is our behaviour on top of the wire: framing gates, ACK
   magic, field-bound paranoia, status→action policy, percent selection
   and the libswupdate-free socket-path fallback. */
#include <catch2/catch_test_macros.hpp>

#include "../src/update.cpp"

#include <array>
#include <span>

#include <cstring>

using namespace std;

namespace {

progress_msg make_msg()
{
    progress_msg m{};
    m.apiversion = PROGRESS_API_VERSION;
    return m;
}

span<const byte> msg_span(const progress_msg & m)
{
    return as_bytes(span{&m, 1});
}

progress_connect_ack make_ack(uint32_t apiversion)
{
    progress_connect_ack a{};
    a.apiversion = apiversion;
    memcpy(a.magic, PROGRESS_CONNECT_ACK_MAGIC, sizeof a.magic); /* "ACK\\0" */
    return a;
}

span<const byte> ack_span(const progress_connect_ack & a)
{
    return as_bytes(span{&a, 1});
}

} // namespace

TEST_CASE("connect ACK parsing")
{
    const auto good = make_ack(PROGRESS_API_VERSION);
    CHECK(parse_connect_ack(ack_span(good)).has_value());
    CHECK(parse_connect_ack(ack_span(good))->apiversion == PROGRESS_API_VERSION);

    auto bad = good;
    bad.magic[1] = 'X';
    CHECK_FALSE(parse_connect_ack(ack_span(bad)).has_value());

    /* A missing NUL in the last magic byte is not "ACK\0" either. */
    auto unterminated = good;
    unterminated.magic[sizeof unterminated.magic - 1] = '!';
    CHECK_FALSE(parse_connect_ack(ack_span(unterminated)).has_value());

    CHECK_FALSE(parse_connect_ack(as_bytes(span{&good, 1}).first(kAckSize - 1)).has_value());
    CHECK_FALSE(parse_connect_ack({}));
}

TEST_CASE("API major compatibility")
{
    CHECK(api_major_ok(PROGRESS_API_VERSION));
    CHECK(api_major_ok((PROGRESS_API_MAJOR << 16) | (7u << 8) | 3u)); /* any 2.x */
    CHECK_FALSE(api_major_ok((PROGRESS_API_MAJOR + 1u) << 16)); /* major bump = new layout */
    CHECK_FALSE(api_major_ok(0));
}

TEST_CASE("progress_msg unpack: every field round-trips")
{
    progress_msg m = make_msg();
    m.status = RUN;
    m.dwl_percent = 42;
    m.dwl_bytes = 0x123456789ABCDEF0ull; /* straddles 32-bit boundaries */
    m.nsteps = 5;
    m.cur_step = 2;
    m.cur_percent = 77;
    strcpy(m.cur_image, "core-image-solar-ctl.ext4");
    strcpy(m.hnd_name, "raw");
    m.source = SOURCE_WEBSERVER;
    m.infolen = 0;

    const auto parsed = parse_progress_msg(msg_span(m));
    REQUIRE(parsed.has_value());
    CHECK(parsed->apiversion == PROGRESS_API_VERSION);
    CHECK(parsed->status == RUN);
    CHECK(parsed->dwl_percent == 42u);
    CHECK(parsed->dwl_bytes == 0x123456789ABCDEF0ull);
    CHECK(parsed->nsteps == 5u);
    CHECK(parsed->cur_step == 2u);
    CHECK(parsed->cur_percent == 77u);
    CHECK(bounded_str(parsed->cur_image, sizeof parsed->cur_image) == "core-image-solar-ctl.ext4");
    CHECK(bounded_str(parsed->hnd_name, sizeof parsed->hnd_name) == "raw");
    CHECK(parsed->source == SOURCE_WEBSERVER);
    CHECK(parsed->infolen == 0u);
}

TEST_CASE("field-bound paranoia (bounded_str)")
{
    progress_msg m = make_msg();
    m.status = START;
    const char payload[] = "{\"1\": { \"reboot-mode\" : \"normal\"}}";
    m.infolen = static_cast<uint32_t>(strlen(payload));
    strcpy(m.info, payload);
    /* Upstream NUL-terminates its char arrays (strlcpy/snprintf); a lying
     * sender must still not push us past a field's extent. */
    memset(m.cur_image, 'x', sizeof(m.cur_image));

    const auto parsed = parse_progress_msg(msg_span(m));
    REQUIRE(parsed.has_value());

    CHECK(bounded_str(parsed->cur_image, sizeof parsed->cur_image).size() ==
          sizeof parsed->cur_image); /* no NUL: whole extent, no overrun */
    CHECK(bounded_str(parsed->hnd_name, sizeof parsed->hnd_name).empty());

    const auto info_len = min<uint32_t>(parsed->infolen, PRINFOSIZE);
    CHECK(bounded_str(parsed->info, info_len) == payload);

    /* A bogus infolen beyond the field is clamped by the consumer, not
     * over-read. */
    m.infolen = 99999;
    const auto clamped = parse_progress_msg(msg_span(m));
    REQUIRE(clamped.has_value());
    const auto clamped_len = min<uint32_t>(clamped->infolen, PRINFOSIZE);
    CHECK(clamped_len == PRINFOSIZE);
    CHECK(bounded_str(clamped->info, clamped_len).rfind("{\"1\":", 0) == 0);
}

TEST_CASE("progress_msg rejects any frame that is not exactly kMsgSize")
{
    progress_msg m = make_msg();
    CHECK(parse_progress_msg(msg_span(m)).has_value());
    CHECK_FALSE(parse_progress_msg(msg_span(m).first(kMsgSize - 1)).has_value());
}

TEST_CASE("UI action mapping")
{
    progress_msg m = make_msg();
    auto action_of = [&](RECOVERY_STATUS status) {
        m.status = status;
        return action_for(*parse_progress_msg(msg_span(m)));
    };
    CHECK(action_of(START) == Action::Upgrade);
    CHECK(action_of(RUN) == Action::Upgrade);
    CHECK(action_of(DOWNLOAD) == Action::Upgrade);
    CHECK(action_of(PROGRESS) == Action::Upgrade);
    CHECK(action_of(SUBPROCESS) == Action::Upgrade);
    CHECK(action_of(SUCCESS) == Action::Succeeded);
    CHECK(action_of(FAILURE) == Action::Failed);
    CHECK(action_of(IDLE) == Action::Ignore);
    CHECK(action_of(DONE) == Action::Ignore);
    m.status = static_cast<RECOVERY_STATUS>(99);
    CHECK(action_for(*parse_progress_msg(msg_span(m))) == Action::Ignore); /* log, don't act */
}

TEST_CASE("overall percent: two axes composed, never backwards")
{
    /* Real daemon behaviour (progress_thread.c): dwl_percent runs 0..100
     * while the archive streams in; cur_percent restarts at 0 for every
     * image of the set (here 2: rootfs + kernel). The old per-status pick
     * made the arc rewind at each alternation. */
    progress_msg m = make_msg();
    m.nsteps = 2;

    m.status = START; /* cur_step 0: nothing running yet */
    CHECK(overall_percent(*parse_progress_msg(msg_span(m)), 0) == 0u);

    m.status = DOWNLOAD;
    m.dwl_percent = 40;
    m.cur_step = 1;
    m.cur_percent = 40; /* rootfs streaming, paced by the installer */
    CHECK(overall_percent(*parse_progress_msg(msg_span(m)), 35) == 40u);

    /* Axes alternate; during the big rootfs the dwl axis leads by design
     * (each step counts equal share, so install-overall lags behind). */
    m.status = PROGRESS;
    m.dwl_percent = 70;
    m.cur_percent = 70;
    CHECK(overall_percent(*parse_progress_msg(msg_span(m)), 40) == 70u);

    /* Rootfs done, kernel (end of archive) starts: dwl ~98 (the 1 MB
     * uImage is the remaining 2%), cur_percent resets to 0 — a raw
     * per-axis display would rewind here; the clamp holds. */
    m.status = RUN;
    m.cur_step = 2;
    m.cur_percent = 0;
    m.dwl_percent = 98;
    CHECK(overall_percent(*parse_progress_msg(msg_span(m)), 70) == 98u);

    /* Tail: kernel flushes after the stream - step axis catches up. */
    m.status = PROGRESS;
    m.cur_percent = 60; /* install overall = (100 + 60) / 2 = 80 < 98 */
    CHECK(overall_percent(*parse_progress_msg(msg_span(m)), 98) == 98u);
    m.cur_percent = 100;
    CHECK(overall_percent(*parse_progress_msg(msg_span(m)), 0) == 100u);

    /* Clamps: wild wire values must still land in 0..100. */
    m.dwl_percent = 65535;
    m.cur_percent = 65535;
    CHECK(overall_percent(*parse_progress_msg(msg_span(m)), 0) == 100u);
    m.cur_step = 999; /* nonsense index saturates, never overflows */
    CHECK(overall_percent(*parse_progress_msg(msg_span(m)), 0) == 100u);
}

TEST_CASE("state and source names")
{
    CHECK(state_name(DOWNLOAD) == "DOWNLOAD");
    CHECK(state_name(PROGRESS) == "PROGRESS");
    CHECK(state_name(99) == "UNKNOWN");
    CHECK(source_name(SOURCE_WEBSERVER) == "WEBSERVER");
    CHECK(source_name(42) == "UNKNOWN");
}

TEST_CASE("socket path fallback mirrors get_prog_socket()")
{
    /* Precedence and the literal concatenation of the daemon; the target
     * build skips all of this and calls get_prog_socket() directly. */
    CHECK(prog_socket_path("/run/solar", "/tmp/x", true) == "/run/solar/swupdateprog");
    CHECK(prog_socket_path(nullptr, "/tmp/x", true) == "/tmp/x/swupdateprog");
    CHECK(prog_socket_path(nullptr, nullptr, true) == "/run/swupdate/swupdateprog");
    CHECK(prog_socket_path(nullptr, nullptr, false) == "/tmp/swupdateprog");
    /* Faithful edge: the daemon only checks pointers against NULL, so a
     * set-but-empty RUNTIME_DIRECTORY really yields "/swupdateprog". */
    CHECK(prog_socket_path("", nullptr, false) == "/swupdateprog");
}
