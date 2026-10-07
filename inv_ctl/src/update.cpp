/* Parsing/policy helpers over swupdate's own protocol definitions (the
 * header comment in update.hpp explains where those come from). Keeping
 * this TU libc-only lets tests/test_update.cpp include it directly. */
#include "update.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

using namespace std;

/* Deliberately public (update.hpp): the parse layer hands out the raw
 * upstream structs, so every print/display site reads char arrays through
 * this instead of trusting the sender's NUL. */
string bounded_str(const char * field, size_t extent)
{
    size_t n = 0;
    while (n < extent && field[n] != '\0')
        ++n;
    return string(field, n);
}

optional<progress_connect_ack> parse_connect_ack(span<const byte> frame)
{
    if (frame.size() != kAckSize)
        return nullopt;
    progress_connect_ack ack{};
    /* LE, fixed size, no padding surprises: exactly how upstream's own
     * clients receive this frame (read() straight into the struct). */
    memcpy(&ack, frame.data(), sizeof ack);
    if (memcmp(ack.magic, PROGRESS_CONNECT_ACK_MAGIC, sizeof ack.magic) != 0)
        return nullopt; /* also rejects a missing NUL in magic[3] */
    return ack;
}

optional<progress_msg> parse_progress_msg(span<const byte> frame)
{
    if (frame.size() != kMsgSize)
        return nullopt;
    progress_msg msg{};
    memcpy(&msg, frame.data(), sizeof msg);
    return msg;
}

bool api_major_ok(uint32_t api_version)
{
    return ((api_version >> 16) & 0xFFFFu) == PROGRESS_API_MAJOR;
}

Action action_for(const progress_msg & msg)
{
    switch (msg.status) {
    case START:
    case RUN:
    case DOWNLOAD:
    case PROGRESS:
    case SUBPROCESS:
        return Action::Upgrade;
    case SUCCESS:
        return Action::Succeeded;
    case FAILURE:
        return Action::Failed;
    default: /* IDLE, DONE and any future status: log only, keep the screen. */
        return Action::Ignore;
    }
}

string_view state_name(uint32_t status)
{
    switch (status) {
    case IDLE:
        return "IDLE";
    case START:
        return "START";
    case RUN:
        return "RUN";
    case SUCCESS:
        return "SUCCESS";
    case FAILURE:
        return "FAILURE";
    case DOWNLOAD:
        return "DOWNLOAD";
    case DONE:
        return "DONE";
    case SUBPROCESS:
        return "SUBPROCESS";
    case PROGRESS:
        return "PROGRESS";
    default:
        return "UNKNOWN";
    }
}

string_view source_name(uint32_t source)
{
    switch (source) {
    case SOURCE_UNKNOWN:
        return "UNKNOWN";
    case SOURCE_WEBSERVER:
        return "WEBSERVER";
    case SOURCE_SURICATTA:
        return "SURICATTA";
    case SOURCE_DOWNLOADER:
        return "DOWNLOADER";
    case SOURCE_LOCAL:
        return "LOCAL";
    case SOURCE_CHUNKS_DOWNLOADER:
        return "CHUNKS_DOWNLOADER";
    default:
        return "UNKNOWN";
    }
}

unsigned percent_for(const progress_msg & msg)
{
    /* Streaming (the long phase while we install over HTTP) counts with
     * dwl_percent; an install step counts with cur_percent. */
    const unsigned pct = (msg.status == DOWNLOAD) ? msg.dwl_percent : msg.cur_percent;
    return min(pct, 100u);
}

string prog_socket_path()
{
#if INV_CTL_SYSTEM_SWUPDATE
    /* The daemon's own resolution, from libswupdate. The char* is
     * asprintf'd upstream and never freed upstream either; we call this
     * once at startup, so the leak is one path string, by design. */
    return get_prog_socket();
#else
    return prog_socket_path(getenv("RUNTIME_DIRECTORY"), getenv("TMPDIR"),
                            access("/run/swupdate", W_OK) == 0);
#endif
}

string prog_socket_path(const char * runtime_dir, const char * tmpdir,
                        bool run_swupdate_writable)
{
    /* Faithful to get_prog_socket(): it checks the pointers against NULL
     * only, so an empty-but-set RUNTIME_DIRECTORY really resolves to
     * "/swupdateprog" there. Do not "improve" that away. */
    const char * dir = runtime_dir ? runtime_dir
                     : tmpdir      ? tmpdir
                                   : (run_swupdate_writable ? "/run/swupdate" : "/tmp");
    return string(dir) + "/swupdateprog";
}
