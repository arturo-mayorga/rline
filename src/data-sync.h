#ifndef data_sync_h_
#define data_sync_h_

#include <stdint.h>

#include <string>
#include <vector>

// The rig fetches its track data from the relay instead of being handed a
// folder.
//
// The problem this removes: the track is not a runtime setting. main.cpp opens
// lap.csv beside the exe and corner-coach-sys.cpp opens corner-names.txt, both
// by fixed name, so changing track meant copying rline-dist to a machine with
// no dev tools. It was missed on 2026-08-09 and again on 2026-08-11, and a
// Mugello reference with Road America names speaks the wrong corner at every
// one of them - indistinguishable from a driver ignoring his coaching.
//
// So the rig asks. On startup, before it loads anything, it opens a short-lived
// connection to the relay, says which files it already has and what they hash
// to, and takes back any that differ. Then it loads from disk exactly as it
// always did. Only the executable is copied by hand now, and only when the
// executable itself changes.
//
// Three properties this has to keep:
//
//   fail soft   The relay may be down, or busy with an older connection. A
//               sync that does not complete leaves the cached files untouched
//               and the rig runs on what it already had. Coaching with last
//               week's reference is bad; refusing to start is worse.
//
//   verified    Bytes are hashed after arrival and compared with the hash the
//               relay advertised. A truncated download must never replace a
//               good file, so the write goes to a temp name and is renamed only
//               once the hash matches.
//
//   bounded     Names come off the wire, and the rig writes what it is told to
//               a directory it cares about. Only the two known filenames are
//               ever accepted - see kSyncFiles - which removes path traversal
//               as a category rather than trying to sanitise it.
//
// This file is free of sockets and of windows.h so it builds and is tested on
// Linux, like everything else that decides what reaches the wire.
namespace datasync
{
    // A distinct magic on the same port. The relay branches on the first four
    // bytes: telemetry connections open with wire::kMagic, these with this. A
    // dedicated connection rather than lines interleaved into the telemetry
    // stream, because file bytes are large, arrive at startup, and must not
    // share a socket with a 60 Hz loop.
    const uint32_t kMagic = 0x53314C52; // 'RL1S'

    const uint16_t kVersion = 1;

    // Every file the rig may accept, and the only names it will write. The rig
    // reads these by fixed name and nothing else, so an allowlist costs nothing
    // and makes a malicious or garbled name unrepresentable.
    extern const char *const kSyncFiles[];
    extern const int kSyncFileCount;

    bool isSyncFile(const std::string &name);

    // Bounds what one download can cost. The Road America reference is 6.4 MB,
    // the largest thing that legitimately moves, so 32 MB is far above any real
    // file and far below a memory problem.
    const uint32_t kMaxFileBytes = 32u * 1024u * 1024u;

    struct FileState
    {
        std::string name;
        std::string hash; // buildid::hashFile of the cached copy, empty if absent
    };

    // Rig -> relay, one line: what the rig has now.
    //
    //   "WANT|v=1|lap.csv=455bbfae|corner-names.txt="
    //
    // An empty hash means "I do not have this at all", which is different from
    // having it and it not matching: the first is a fresh install, the second
    // is a stale one, and the log should be able to tell them apart.
    std::string wantLine(const std::vector<FileState> &have);
    bool parseWant(const std::string &line, std::vector<FileState> *out);

    // Relay -> rig, one line per file, in the order the rig asked.
    //
    //   "SAME|name=lap.csv"                              - your copy is current
    //   "NONE|name=lap.csv"                              - the relay has not got it
    //   "DATA|name=lap.csv|hash=455bbfae|len=788134"     - then exactly len raw bytes
    //
    // Raw bytes rather than base64: this connection carries nothing else, so
    // there is no framing to protect and no reason to pay 33% for encoding.
    enum Reply
    {
        kReplyBad = 0,
        kReplySame,
        kReplyNone,
        kReplyData
    };

    struct Header
    {
        Reply reply = kReplyBad;
        std::string name;
        std::string hash;
        uint32_t len = 0;
    };

    std::string sameLine(const std::string &name);
    std::string noneLine(const std::string &name);
    std::string dataLine(const std::string &name, const std::string &hash, uint32_t len);

    // Rejects anything it cannot fully account for: an unknown verb, a name not
    // in kSyncFiles, a DATA line without a hash, a length over kMaxFileBytes.
    // Returning false is always safe - the caller keeps its cached file.
    bool parseHeader(const std::string &line, Header *out);

    // True when `bytes` is what `h` promised. Checked before anything is
    // renamed into place.
    bool verify(const Header &h, const void *bytes, size_t len);
}

#endif
