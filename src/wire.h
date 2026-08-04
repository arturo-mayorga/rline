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

namespace wire
{
    const uint32_t kMagic = 0x314C5243; // 'CRL1'
    const uint16_t kVersion = 1;

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

    struct ChannelDesc
    {
        char name[kNameLen];
        char unit[kUnitLen];
    };
}

#endif
