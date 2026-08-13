#ifndef data_sync_net_h_
#define data_sync_net_h_

#include <string>

// The socket half of fetching track data from the relay. Windows only, like
// everything that touches winsock; the half that decides what may be written
// lives in data-sync.cpp and is tested on Linux.
//
// Called once from main(), before the reference lap is loaded, so the files on
// disk are already current by the time anything opens them. That ordering is
// the whole feature: sync-then-load needs no runtime reload path, and the rig
// keeps loading lap.csv beside the exe exactly as it always has.
namespace datasyncnet
{
    struct Result
    {
        bool contacted = false; // the relay answered at all
        int updated = 0;        // files replaced with newer bytes
        int current = 0;        // files the relay agreed were already right
        int failed = 0;         // answered but could not be installed
    };

    // Blocks for at most `timeoutMs` in total. Every failure path leaves the
    // cached files untouched: a relay that is down, busy with an older
    // connection, or serving a truncated file all end with the rig running on
    // what it already had. Starting with last week's reference is bad; not
    // starting is worse.
    //
    // `log` receives one human-readable line per file, for the rig's own log.
    Result fetch(const std::string &host, int port, const std::string &dir,
                 int timeoutMs, std::string *log);
}

#endif
