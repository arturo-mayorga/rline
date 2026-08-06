// The order the rig says things in, and what it refuses to say.
//
// Written because the driver reported being cut off mid-sentence: two sources
// shared one message slot and the speaker purged whatever was already talking.
// These rules are what replace that, so they are worth checking on every build.

#include "../src/speech-queue.h"

#include <cstdio>
#include <string>

static int g_fail = 0;
static void check(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) ++g_fail;
}

int main()
{
    printf("-- nothing is lost when two notes arrive together --\n");
    {
        // The original bug: the corner coach and the relay both wrote the same
        // slot, so one of them was overwritten before it was ever spoken.
        speech::Queue q;
        q.push("turn eight next, off the brake sooner", speech::PriorityCorner, 3000, 5);
        q.push("your brake point is right, it is the release", speech::PriorityNote, 0, 5);
        check(q.size() == 2, "both are held, neither overwrites the other");

        speech::Item a, b;
        check(q.pop(&a) && q.pop(&b), "both come back out");
        check(!q.pop(&a), "and then the queue is empty");
    }

    printf("\n-- time-critical goes first --\n");
    {
        speech::Queue q;
        q.push("a conversational reply", speech::PriorityNote, 0, 5);
        q.push("turn eight next", speech::PriorityCorner, 3000, 5);

        speech::Item it;
        q.pop(&it);
        check(it.priority == speech::PriorityCorner,
              "the corner cue jumps the reply that will keep");
        q.pop(&it);
        check(it.priority == speech::PriorityNote, "the reply still follows");
    }

    printf("\n-- oldest first within a priority, so nothing starves --\n");
    {
        speech::Queue q;
        q.push("first", speech::PriorityNote, 0, 5);
        q.age(100);
        q.push("second", speech::PriorityNote, 0, 5);
        q.age(100);
        q.push("third", speech::PriorityNote, 0, 5);

        speech::Item it;
        q.pop(&it); check(it.text == "first", "first in, first out");
        q.pop(&it); check(it.text == "second", "then the next oldest");
        q.pop(&it); check(it.text == "third", "then the newest");
    }

    printf("\n-- a stale corner cue is dropped, never spoken late --\n");
    {
        // Saying "turn eight next" after turn eight sends him to the wrong
        // piece of road; silence is strictly better.
        speech::Queue q;
        q.push("turn eight next, off the brake sooner", speech::PriorityCorner, 3000, 5);
        q.age(2900);
        check(q.size() == 1, "still fresh just before its shelf life");
        q.age(200);
        check(q.empty(), "dropped once stale");
        check(q.expired() == 1, "and counted, so a quiet rig can be explained");
    }

    printf("\n-- a conversational note keeps --\n");
    {
        speech::Queue q;
        q.push("that was your best clean lap", speech::PriorityNote, 0, 5);
        q.age(60000);
        check(q.size() == 1, "a ttl of zero never goes stale");
    }

    printf("\n-- the same cue twice is said once --\n");
    {
        speech::Queue q;
        q.push("turn eight next", speech::PriorityCorner, 3000, 5);
        q.push("turn eight next", speech::PriorityCorner, 3000, 5);
        check(q.size() == 1, "an exact repeat is ignored while it waits");
    }

    printf("\n-- a backlog is bounded, and sheds the least useful first --\n");
    {
        speech::Queue q;
        for (int i = 0; i < 20; ++i)
            q.push("note " + std::to_string(i), speech::PriorityNote, 0, 5);
        check(q.size() <= 8, "the queue cannot grow without limit");
        check(q.dropped() > 0, "and the drops are counted");

        // A corner cue must still get in when the queue is full of chatter.
        q.push("turn eight next", speech::PriorityCorner, 3000, 5);
        speech::Item it;
        q.pop(&it);
        check(it.priority == speech::PriorityCorner,
              "a corner cue displaces chatter rather than being refused");
    }

    printf("\n-- a full queue of corner cues refuses a lower-priority note --\n");
    {
        speech::Queue q;
        for (int i = 0; i < 8; ++i)
            q.push("cue " + std::to_string(i), speech::PriorityCorner, 3000, 5);
        q.push("a reply", speech::PriorityNote, 0, 5);

        bool foundNote = false;
        speech::Item it;
        while (q.pop(&it))
            if (it.text == "a reply") foundNote = true;
        check(!foundNote, "nothing more important is sacrificed for something less");
    }

    printf("\n-- empty input is ignored --\n");
    {
        speech::Queue q;
        q.push("", speech::PriorityNote, 0, 5);
        check(q.empty(), "an empty note never reaches the voice");
    }

    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASSED" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
