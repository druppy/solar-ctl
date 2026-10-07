#pragma once

#include "update.hpp"

#include <glibmm.h>

#include <functional>
#include <string>
#include <vector>

/* glibmm listener for the swupdate progress socket (the socket + protocol
 * facts are documented in update.hpp). Every received frame - including
 * everything that is NOT progress (info payloads, DONE, unknown statuses)
 * - is logged via g_message so journald keeps the debugging trail; all
 * messages are then forwarded to the handler, which owns the UI policy.
 *
 * Connect/disconnect is a non-event: while the daemon is unreachable (boot
 * ordering, daemon restarts) it retries every kRetryMs. One instance,
 * owned by main, lives as long as the main loop. */
class Updater
{
public:
    using Handler = std::function<void (const progress_msg &)>;

    explicit Updater(std::string socket_path) : path_(std::move(socket_path)) {}

    void start(Handler on_msg);

private:
    void attempt();
    void schedule_retry();
    void drop(std::string_view why);
    /* Single IOCondition argument: the slot signature glibmm 2.66 (meta-oe,
     * target) and 2.90 (host) agree on. Returning false lets glib remove
     * the source itself - there is no connection handle to store, which is
     * good because that handle's TYPE differs between those glibmm
     * generations. */
    bool on_io(Glib::IOCondition cond);
    bool consume(); /* false = connection dropped while parsing */

    std::string path_;
    Handler on_msg_;
    int fd_ = -1;
    std::vector<std::byte> rx_;
    bool acked_ = false;
};
