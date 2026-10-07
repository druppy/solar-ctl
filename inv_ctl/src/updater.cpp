/* glibmm wiring around the pure protocol layer (update.hpp). */
#include "updater.hpp"

#include <glib.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

using namespace std;

namespace {

constexpr int kRetryMs = 5000;
constexpr int kReadSize = 8192; /* > kMsgSize, so one read can carry a frame */

/* The full trail for future debugging (user requirement: everything that
 * is not progress gets logged too - so we simply log everything). */
void log_msg(const progress_msg & m)
{
    g_message("swupdate progress: %s step %u/%u %u%% (dwl %u%% of %llu B) image '%s' handler '%s' source %s",
              state_name(m.status).data(), m.cur_step, m.nsteps, m.cur_percent,
              m.dwl_percent, m.dwl_bytes,
              bounded_str(m.cur_image, sizeof m.cur_image).c_str(),
              bounded_str(m.hnd_name, sizeof m.hnd_name).c_str(),
              source_name(m.source).data());
    if (m.infolen)
        g_message("swupdate progress info: %s",
                  bounded_str(m.info, min<uint32_t>(m.infolen, PRINFOSIZE)).c_str());
}

} // namespace

void Updater::start(Handler on_msg)
{
    on_msg_ = move(on_msg);
    attempt();
}

void Updater::attempt()
{
    /* Non-blocking is about reads only: an AF_UNIX connect to a missing
     * socket returns ECONNREFUSED/ENOENT immediately either way. */
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        schedule_retry();
        return;
    }

    struct sockaddr_un addr {
    };
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path_.c_str());
    if (::connect(fd, reinterpret_cast<const struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        schedule_retry();
        return;
    }

    fd_ = fd;
    rx_.clear();
    acked_ = false;
    /* Return value discarded on purpose (handle type is glibmm-generation
     * dependent); the source lives until on_io returns false. */
    Glib::signal_io().connect(
        sigc::mem_fun(*this, &Updater::on_io), fd_,
        Glib::IOCondition::IO_IN | Glib::IOCondition::IO_HUP | Glib::IOCondition::IO_ERR);
    g_message("inv-ctl: subscribed to swupdate progress on %s", path_.c_str());
}

void Updater::schedule_retry()
{
    Glib::signal_timeout().connect_once([this] { attempt(); }, kRetryMs);
}

void Updater::drop(string_view why)
{
    g_message("inv-ctl: swupdate progress socket gone (%s), retrying in %d ms",
              string(why).c_str(), kRetryMs);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    rx_.clear();
    acked_ = false;
    schedule_retry();
    /* The io source is removed by the caller returning false to glib. */
}

bool Updater::on_io(Glib::IOCondition /*cond*/)
{
    if (fd_ < 0) /* already dropped within this callback round */
        return false;
    byte buf[kReadSize];
    const ssize_t n = ::read(fd_, buf, sizeof(buf));
    if (n > 0) {
        rx_.insert(rx_.end(), buf, buf + n);
        return consume();
    }
    if (n == 0) { /* daemon closed (HUP also lands here on some paths) */
        drop("peer closed");
        return false;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return true;
    drop(strerror(errno));
    return false;
}

bool Updater::consume()
{
    for (;;) {
        if (!acked_) {
            if (rx_.size() < kAckSize)
                return true;
            const optional<progress_connect_ack> ack = parse_connect_ack({rx_.data(), kAckSize});
            if (!ack || !api_major_ok(ack->apiversion)) {
                drop("bad progress ACK (magic or API major)");
                return false;
            }
            acked_ = true;
            rx_.erase(rx_.begin(), rx_.begin() + static_cast<ptrdiff_t>(kAckSize));
        }
        if (rx_.size() < kMsgSize)
            return true;
        optional<progress_msg> msg = parse_progress_msg({rx_.data(), kMsgSize});
        rx_.erase(rx_.begin(), rx_.begin() + static_cast<ptrdiff_t>(kMsgSize));
        if (!msg) { /* impossible after the size gate; framing is a lie anyway */
            drop("short progress frame");
            return false;
        }
        if (msg->apiversion != PROGRESS_API_VERSION)
            g_message("inv-ctl: progress frame API 0x%08x differs from compiled 0x%08x",
                      msg->apiversion, PROGRESS_API_VERSION);
        log_msg(*msg);
        if (on_msg_)
            on_msg_(*msg);
    }
}
