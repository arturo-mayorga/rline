#ifndef wire_h_
#define wire_h_

#include <stdint.h>

// Protocol between the rig and the relay.
//
// Rig -> relay is binary and high rate: every scalar channel iRacing exposes,
// 60 times a second. The rig deliberately sends everything rather than a
// curated subset, so that adding a new analysis later never means reinstalling
// anything on the rig. The relay runs on the analysis machine and decides what
// is worth keeping.
//
// Relay -> rig is line-based text and very low rate - coaching notes and HUD
// definitions. Text rather than JSON so the rig needs no parser beyond
// splitting on '|' and '='.
//
// Rig -> relay is *mostly* binary, with one exception: recognised speech. The
// driver holds a wheel button and talks, and the rig sends the recognised line
// back up the same socket, interleaved between telemetry frames. It gets its
// own marker rather than a side channel so it cannot arrive out of order with
// respect to the telemetry it refers to.

namespace wire
{
    const uint32_t kMagic = 0x314C5243; // 'CRL1'

    // 2 added kTextMarker. Both ends must be rebuilt together: an older relay
    // skips unknown markers a byte at a time, so it would eventually resync but
    // could read one bogus frame first if a text byte happened to be 0xF0.
    const uint16_t kVersion = 2;

    const int kDefaultPort = 7642;

    const int kNameLen = 32; // matches IRSDK_MAX_STRING
    const int kUnitLen = 16;

    // Sent once when a connection opens, so the relay can label the columns
    // without knowing anything about iRacing.
    //
    //   uint32 magic
    //   uint16 version
    //   uint16 channelCount
    //   channelCount * { char name[kNameLen]; char unit[kUnitLen]; }
    struct HelloHeader
    {
        uint32_t magic;
        uint16_t version;
        uint16_t channelCount;
    };

    // Then repeatedly:
    //
    //   uint8  marker (kFrameMarker)
    //   uint32 sequence
    //   float  values[channelCount]
    //
    // The marker lets a reader resynchronise if it ever loses its place, and
    // the sequence exposes dropped frames rather than hiding them.
    const uint8_t kFrameMarker = 0xF0;

    // Interleaved with the telemetry frames, whenever the driver has said
    // something:
    //
    //   uint8  marker (kTextMarker)
    //   uint16 length
    //   char   utf8[length]        - no trailing newline
    //
    // One line per utterance, in the same '|'-separated shape as the relay's
    // own commands. See src/voice-line.h. Length-prefixed rather than
    // newline-terminated so a reader never has to scan a binary stream for a
    // delimiter that could occur inside a float.
    const uint8_t kTextMarker = 0xF1;

    // Bounds the allocation a malformed or hostile length field can cause.
    const uint16_t kMaxTextLen = 1024;

    struct ChannelDesc
    {
        char name[kNameLen];
        char unit[kUnitLen];
    };
}

#endif
