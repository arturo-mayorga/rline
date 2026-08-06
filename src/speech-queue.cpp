#include "speech-queue.h"

void speech::Queue::push(const std::string &text, int priority, float ttlMs,
                         float displaySecs)
{
    if (text.empty())
        return;

    // The corner coach can produce the same cue on consecutive ticks; saying it
    // twice is worse than saying it once.
    for (const Item &i : _items)
        if (i.text == text)
            return;

    if (_items.size() >= kMax)
    {
        // Full. Sacrifice the least useful thing waiting - lowest priority,
        // and the oldest of those, since it is closest to going stale anyway.
        size_t worst = 0;
        for (size_t k = 1; k < _items.size(); ++k)
        {
            const bool lowerPriority = _items[k].priority < _items[worst].priority;
            const bool sameButOlder = _items[k].priority == _items[worst].priority &&
                                      _items[k].ageMs > _items[worst].ageMs;
            if (lowerPriority || sameButOlder)
                worst = k;
        }

        // Never drop something more important than what is arriving.
        if (_items[worst].priority > priority)
        {
            ++_dropped;
            return;
        }
        _items.erase(_items.begin() + worst);
        ++_dropped;
    }

    Item it;
    it.text = text;
    it.priority = priority;
    it.ttlMs = ttlMs;
    it.displaySecs = displaySecs;
    it.ageMs = 0;
    _items.push_back(it);
}

void speech::Queue::age(float deltaMs)
{
    for (size_t k = _items.size(); k-- > 0;)
    {
        _items[k].ageMs += deltaMs;
        if (_items[k].ttlMs > 0 && _items[k].ageMs >= _items[k].ttlMs)
        {
            _items.erase(_items.begin() + k);
            ++_expired;
        }
    }
}

bool speech::Queue::pop(Item *out)
{
    if (_items.empty())
        return false;

    size_t best = 0;
    for (size_t k = 1; k < _items.size(); ++k)
    {
        const bool higher = _items[k].priority > _items[best].priority;
        const bool sameButOlder = _items[k].priority == _items[best].priority &&
                                  _items[k].ageMs > _items[best].ageMs;
        if (higher || sameButOlder)
            best = k;
    }

    if (out)
        *out = _items[best];
    _items.erase(_items.begin() + best);
    return true;
}
