#include "session-policy.h"

#include <cctype>

namespace
{
    std::string lower(const std::string &s)
    {
        std::string o = s;
        for (char &c : o)
            c = (char)tolower((unsigned char)c);
        return o;
    }

    bool has(const std::string &hay, const char *needle)
    {
        return hay.find(needle) != std::string::npos;
    }

    std::string field(const std::string &line, const std::string &key)
    {
        const std::string needle = "|" + key + "=";
        size_t p = line.find(needle);
        if (p == std::string::npos)
            return std::string();
        p += needle.size();
        const size_t end = line.find('|', p);
        return line.substr(p, end == std::string::npos ? std::string::npos : end - p);
    }
}

namespace sessionpolicy
{
    const char *name(Mode m)
    {
        switch (m)
        {
        case kFull:
            return "full";
        case kConfirm:
            return "confirm";
        case kSilent:
            return "silent";
        }
        return "confirm";
    }

    bool isYellow(unsigned flags)
    {
        return (flags & kYellowFlagMask) != 0;
    }

    bool parseCommand(const std::string &line, Mode *out, bool *isAuto)
    {
        if (line.compare(0, 6, "COACH|") != 0)
            return false;

        const std::string v = lower(field(line, "mode"));
        if (v.empty())
            return false;

        bool automatic = false;
        Mode m = kConfirm;

        if (v == "auto")
            automatic = true;
        else if (v == "full" || v == "on")
            m = kFull;
        else if (v == "confirm")
            m = kConfirm;
        else if (v == "silent" || v == "off")
            m = kSilent;
        else
            return false; // an unrecognised mode must not silently mean anything

        if (out)
            *out = m;
        if (isAuto)
            *isAuto = automatic;
        return true;
    }

    std::string typeFromYaml(const std::string &yaml, int sessionNum)
    {
        const std::string numKey = "SessionNum:";
        const std::string typeKey = "SessionType:";

        size_t at = 0;
        while ((at = yaml.find(numKey, at)) != std::string::npos)
        {
            size_t p = at + numKey.size();
            while (p < yaml.size() && (yaml[p] == ' ' || yaml[p] == '\t'))
                ++p;

            // Read the number without atoi so a trailing "SessionNumber:"-style
            // key cannot be mistaken for a value.
            int n = 0;
            size_t digits = 0;
            while (p + digits < yaml.size() && yaml[p + digits] >= '0' && yaml[p + digits] <= '9')
            {
                n = n * 10 + (yaml[p + digits] - '0');
                ++digits;
            }
            at += numKey.size();
            if (digits == 0 || n != sessionNum)
                continue;

            // The type belongs to this session only if it appears before the
            // next SessionNum - otherwise a session with no SessionType would
            // silently borrow the following session's, which at a race weekend
            // means reporting "Race" during practice.
            const size_t nextNum = yaml.find(numKey, p);
            const size_t typeAt = yaml.find(typeKey, p);
            if (typeAt == std::string::npos)
                return std::string();
            if (nextNum != std::string::npos && typeAt > nextNum)
                return std::string();

            size_t v = typeAt + typeKey.size();
            while (v < yaml.size() && (yaml[v] == ' ' || yaml[v] == '\t'))
                ++v;
            size_t e = v;
            while (e < yaml.size() && yaml[e] != '\n' && yaml[e] != '\r')
                ++e;
            while (e > v && (yaml[e - 1] == ' ' || yaml[e - 1] == '\t'))
                --e;
            return yaml.substr(v, e - v);
        }
        return std::string();
    }

    Mode fromSessionType(const std::string &sessionType)
    {
        const std::string s = lower(sessionType);

        // Checked before "practice": iRacing calls the pre-race session
        // "Warmup", and on a race day that is the ten minutes in which nothing
        // new may be introduced.
        if (has(s, "warmup") || has(s, "warm up"))
            return kConfirm;

        if (has(s, "race"))
            return kSilent;

        // Qualifying is a race session in every way that matters here: one lap
        // to get right, no lap left to correct anything in.
        if (has(s, "qualify") || has(s, "qualifying"))
            return kSilent;

        if (has(s, "practice") || has(s, "testing") || has(s, "test"))
            return kFull;

        // Unknown. A new session name is more likely to be a race variant than
        // a practice one, and the cost of guessing wrong is asymmetric, so this
        // never opens up - it closes down one step.
        return kConfirm;
    }

    Mode decide(const std::string &sessionType, bool onPitRoad, bool outLap,
                unsigned flags)
    {
        // Nothing about technique belongs in the pit lane or under caution,
        // whatever session it is.
        if (onPitRoad || isYellow(flags))
            return kSilent;

        Mode m = fromSessionType(sessionType);

        // An out-lap carries no fresh evidence: whatever fault was remembered
        // came from before the stop, and the tyres are not the tyres it was
        // measured on. Never more than confirmation.
        if (outLap && m == kFull)
            m = kConfirm;

        return m;
    }
}
