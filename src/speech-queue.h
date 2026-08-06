#ifndef speech_queue_h_
#define speech_queue_h_

#include <string>
#include <vector>

// What the rig says out loud, in order, without talking over itself.
//
// Two independent sources produce notes: CornerCoachSystem's feed-forward cues
// (six to nine a lap) and lines pushed from the relay. They used to share one
// slot in CoachMessageComponent, and the speaker used SPF_PURGEBEFORESPEAK, so
// whichever arrived second cut the first off mid-sentence - and if two arrived
// between ticks, the first was silently overwritten and never spoken at all.
//
// The rules, in the order they matter:
//
//   Nothing interrupts. A sentence that has started is always finished. Being
//   cut off mid-word is worse than being a second late, because a half-heard
//   cue has to be guessed at while driving.
//
//   Time-critical notes go first. A corner cue refers to a corner that is
//   arriving; it beats a conversational note that will keep.
//
//   Stale notes are dropped, not spoken late. A corner cue delivered after the
//   corner is worse than silence - it sends the driver looking at the wrong
//   piece of road. Each item carries its own shelf life.
//
// This half is deliberately free of windows.h: it decides what the driver hears
// and in what order, which is worth testing on any machine.
namespace speech
{
    enum Priority
    {
        // A reply, an answer to a question, an observation. Will keep.
        PriorityNote = 0,
        // A cue about a corner that is arriving. Short shelf life.
        PriorityCorner = 1,
    };

    struct Item
    {
        std::string text;
        int priority = PriorityNote;
        float ttlMs = 0;         // 0 means it never goes stale
        float displaySecs = 5.0f;
        float ageMs = 0;         // maintained by age()
    };

    class Queue
    {
    private:
        // Deep enough for a burst, shallow enough that a backlog is dropped
        // rather than read out minutes later.
        static const size_t kMax = 8;

        std::vector<Item> _items;
        int _dropped = 0;
        int _expired = 0;

    public:
        // Ignores an exact duplicate of something already waiting: the corner
        // coach can re-derive the same cue on consecutive ticks.
        void push(const std::string &text, int priority, float ttlMs, float displaySecs);

        // Advances shelf lives and discards anything that has gone stale.
        void age(float deltaMs);

        // Removes and returns the next thing to say: highest priority first,
        // and oldest first within a priority so nothing is starved. False when
        // there is nothing to say.
        bool pop(Item *out);

        size_t size() const { return _items.size(); }
        bool empty() const { return _items.empty(); }

        // Counters for diagnosing a rig that says less than expected.
        int dropped() const { return _dropped; }
        int expired() const { return _expired; }
    };
}

#endif
