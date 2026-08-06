#include "talk-button.h"

#include <cstdio>

namespace
{
    // Strict, deliberately: strtol would accept " 0x7 " and a stray sign, and a
    // config file typo that silently binds the wrong button would look like
    // broken hardware.
    bool parseIndex(const std::string &s, int *out)
    {
        if (s.empty() || s.size() > 3)
            return false;
        int v = 0;
        for (char c : s)
        {
            if (c < '0' || c > '9')
                return false;
            v = v * 10 + (c - '0');
        }
        *out = v;
        return true;
    }
}

bool talk::parseSpec(const std::string &text, Spec *out)
{
    // Trim: the value usually arrives from a text file the driver may have
    // edited by hand.
    size_t a = 0, b = text.size();
    while (a < b && (text[a] == ' ' || text[a] == '\t'))
        ++a;
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' ||
                     text[b - 1] == '\r' || text[b - 1] == '\n'))
        --b;
    const std::string t = text.substr(a, b - a);

    const size_t colon = t.find(':');
    if (colon == std::string::npos)
        return false;

    Spec s;
    if (!parseIndex(t.substr(0, colon), &s.device))
        return false;
    if (!parseIndex(t.substr(colon + 1), &s.button))
        return false;

    if (out)
        *out = s;
    return true;
}

std::string talk::formatSpec(const Spec &s)
{
    if (!s.valid())
        return std::string();
    char buf[32];
    snprintf(buf, sizeof(buf), "%d:%d", s.device, s.button);
    return std::string(buf);
}
