#ifndef build_id_h_
#define build_id_h_

#include <stdint.h>

#include <string>

// What the rig is actually running, reported to the relay the moment it
// connects.
//
// This exists because of a whole evening lost to a question nobody could
// answer: the driver copies a folder to a machine with no dev tools, and from
// the analysis side there was no way to tell whether he had copied the new one.
// A Mugello reference with Road America corner names speaks the wrong corner at
// every single one, and it looks exactly like a driver who ignores coaching.
//
// So the rig says what it loaded, not what it was built from. Every runtime
// field is measured from the data structures actually in use - if a stale
// lap.csv is sitting beside the exe, this reports the stale one, which is the
// entire point. The two fields that settle it in one glance are `first`, the
// first corner name the coach will speak, and `ref`, a hash of the reference
// file's bytes.
//
// Free of windows.h so it builds and is tested on Linux, like everything else
// that decides what reaches the wire.
namespace buildid
{
    // FNV-1a. Not cryptographic and does not need to be: it distinguishes two
    // files a human chose between, and eight hex characters is short enough to
    // read out over voice if it ever comes to that.
    uint32_t hashBytes(const void *data, size_t len);

    // Hash a whole file, streamed. Returns an empty string if it cannot be
    // read, so a missing file is reported as missing rather than as a hash of
    // nothing - "" and a hash of zero bytes must not look alike.
    std::string hashFile(const std::string &path);

    std::string hex8(uint32_t h);

    struct Info
    {
        std::string track;    // the track CMake built this exe to ship
        std::string exeHash;  // hash of the running binary - exact, never stale
        std::string built;    // compile timestamp, advisory only
        int proto = 0;        // wire::kVersion

        // Measured from the loaded RefLine, not from the file on disk.
        std::string refHash;
        size_t refPoints = 0;
        float refLength = 0;
        float refLapTime = 0;
        int refCorners = 0;

        // Measured from the names CornerCoach will actually speak.
        int nameCount = 0;
        std::string firstName;
    };

    // "BUILD|track=mugello|proto=2|exe=1a2b3c4d|..." - flat key=value fields so
    // the protocol stays splittable on '|' and '=' with no parser, and an older
    // reader ignores fields it does not know rather than failing.
    std::string buildLine(const Info &in);

    // Returns false for anything that is not a BUILD line. Missing fields are
    // left at their defaults, so a newer rig talking to an older relay - or the
    // reverse - degrades to partial information rather than nothing.
    bool parseLine(const std::string &line, Info *out);

    // One human-readable block for the relay's log. Multi-line, no trailing
    // newline. `expectTrack` is what the analysis machine believes it deployed;
    // when it disagrees with what arrived, that mismatch is the headline.
    std::string describe(const Info &in, const std::string &expectTrack = std::string());
}

#endif
