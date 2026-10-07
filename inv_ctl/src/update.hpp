#pragma once

/* Client side of the SWUpdate progress IPC.
 *
 * ALL protocol definitions - struct progress_msg, struct
 * progress_connect_ack, RECOVERY_STATUS / sourcetype, PROGRESS_API_*,
 * PROGRESS_CONNECT_ACK_MAGIC, PRINFOSIZE - come from swupdate's own
 * headers, never from copies: on the target from the sysroot
 * (swupdate-dev via DEPENDS in fw/recipes-apps/inv-ctl/inv-ctl_0.1.bb),
 * natively (dev builds + CI tests) from the same pinned include/ tree
 * (see CMakeLists.txt). A swupdate bump moves the wire layout
 * automatically and this code follows it - or the compiler says so.
 * get_prog_socket() is even called from libswupdate itself on the
 * target, so socket resolution cannot drift from the daemon's.
 *
 * Frame semantics (verified against the pinned sources): the daemon
 * accepts any number of progress clients (core/progress_thread.c
 * broadcasts to a list), sends each new connection an ACK, then
 * fixed-size progress_msg frames, and only while an install runs
 * (idle = silence). Fields are native-LE; upstream's own clients read
 * the frames straight into the packed struct, and so do we.
 *
 * This TU is libc-only (plus those headers): no glib, no LVGL;
 * tests/test_update.cpp includes the .cpp directly. */

#include <progress_ipc.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

/* Frame sizes derived from the upstream types themselves. */
inline constexpr std::size_t kAckSize = sizeof(progress_connect_ack);
inline constexpr std::size_t kMsgSize = sizeof(progress_msg);

/* Both parsers require an exact-size frame; any other span means the
 * connection is not framed as expected and the caller must reconnect. */
std::optional<progress_connect_ack> parse_connect_ack(std::span<const std::byte> frame);
std::optional<progress_msg> parse_progress_msg(std::span<const std::byte> frame);

bool api_major_ok(std::uint32_t api_version);

/* Wire strings are NUL-terminated upstream (strlcpy/snprintf) but the
 * parse functions hand out the raw structs, so print/display sites go
 * through this bounded reader instead of trusting the sender. */
std::string bounded_str(const char * field, std::size_t extent);

/* What the UI should do with a message (everything, including the Ignored
 * statuses, is logged by the Updater regardless). */
enum class Action { Ignore, Upgrade, Succeeded, Failed };
Action action_for(const progress_msg & msg);

std::string_view state_name(std::uint32_t status);
std::string_view source_name(std::uint32_t source);


/* One honest bar out of the daemon's TWO parallel axes (core/
 * progress_thread.c): dwl_percent = share of the archive streamed into
 * the install pipe; cur_percent = percent of the CURRENT image
 * (swupdate_progress_inc_step resets it to 0 per image of the set).
 * Neither alone is "the upgrade's progress": picking one per status made
 * the arc walk backwards at every axis switch. Combine them: the image
 * overall is ((cur_step-1)*100 + cur_percent)/nsteps, the visible value
 * is whichever axis is ahead, and it never decreases below `prev`
 * (pass the previous return value; reset it on START). */
unsigned overall_percent(const progress_msg & msg, unsigned prev);

/* Socket path: on the target this IS the daemon's get_prog_socket(),
 * linked from libswupdate. TRAP: that function resolves from the
 * environment - if ever a unit gains RuntimeDirectory=/TMPDIR= (see the
 * same note in fw/recipes-support/solar-swu-agent/files/
 * solar-swupdate-progress.service), client and daemon must still agree;
 * only then does --progress-socket save you. */
std::string prog_socket_path();

/* Fallback mirror of get_prog_socket() (ipc/progress_ipc.c), used by
 * native builds that do not link libswupdate at all (dev laptops, CI).
 * Exposed for tests; precedence: $RUNTIME_DIRECTORY, else $TMPDIR, else
 * /run/swupdate if writable, else /tmp - each + "/swupdateprog". */
std::string prog_socket_path(const char * runtime_dir, const char * tmpdir,
                             bool run_swupdate_writable);
