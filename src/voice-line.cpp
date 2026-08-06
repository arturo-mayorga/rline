#include "voice-line.h"

#include <cstdio>
#include <cstdlib>

namespace
{
    bool isSpace(unsigned char c)
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
    }

    // True for the second and subsequent bytes of a UTF-8 sequence. Used so a
    // truncation never leaves half a character on the wire.
    bool isContinuation(unsigned char c)
    {
        return (c & 0xC0) == 0x80;
    }
}

std::string voice::sanitize(const std::string &raw)
{
    std::string out;
    out.reserve(raw.size());

    bool pendingSpace = false;
    for (size_t i = 0; i < raw.size(); ++i)
    {
        const unsigned char c = (unsigned char)raw[i];

        // '|' would forge a field boundary and let a misrecognition rewrite the
        // rest of the line. There is no legitimate way for dictation to produce
        // one, so drop it rather than escaping it.
        if (c == '|')
            continue;

        if (isSpace(c) || c < 0x20 || c == 0x7F)
        {
            // Collapse to a single space, and never lead with one.
            if (!out.empty())
                pendingSpace = true;
            continue;
        }

        if (pendingSpace)
        {
            out.push_back(' ');
            pendingSpace = false;
        }
        out.push_back((char)c);
    }

    if (out.size() > kMaxTextLen)
    {
        size_t cut = kMaxTextLen;
        // Back off out of the middle of a UTF-8 sequence...
        while (cut > 0 && isContinuation((unsigned char)out[cut]))
            --cut;
        // ...and then, if there is one nearby, to a word boundary.
        const size_t space = out.rfind(' ', cut);
        if (space != std::string::npos && space + 40 >= cut)
            cut = space;
        out.resize(cut);
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
    }

    return out;
}

std::string voice::buildLine(const std::string &text, float confidence, bool fromGrammar)
{
    const std::string clean = sanitize(text);
    if (clean.empty())
        return std::string();

    float c = confidence;
    if (!(c >= 0.0f)) // also catches NaN
        c = 0.0f;
    if (c > 1.0f)
        c = 1.0f;

    char head[48];
    snprintf(head, sizeof(head), "HEAR|conf=%.2f|via=%s|text=", c,
             fromGrammar ? "cmd" : "free");
    return std::string(head) + clean;
}

bool voice::parseLine(const std::string &line, std::string *text, float *confidence,
                      std::string *via)
{
    if (line.rfind("HEAR|", 0) != 0)
        return false;

    // text= is always last and its value runs to end of line, so it needs no
    // escaping and may contain '=' freely.
    const size_t textPos = line.find("|text=");
    if (textPos == std::string::npos)
        return false;

    const std::string value = line.substr(textPos + 6);
    if (value.empty())
        return false;

    if (via)
        *via = "free";

    if (confidence || via)
    {
        if (confidence)
            *confidence = 0.0f;
        const std::string head = line.substr(5, textPos - 5);
        size_t pos = 0;
        while (pos <= head.size())
        {
            const size_t bar = head.find('|', pos);
            const std::string field =
                head.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
            const size_t eq = field.find('=');
            if (eq != std::string::npos)
            {
                const std::string k = field.substr(0, eq);
                if (k == "conf" && confidence)
                    *confidence = (float)atof(field.c_str() + eq + 1);
                else if (k == "via" && via)
                    *via = field.substr(eq + 1);
            }
            if (bar == std::string::npos)
                break;
            pos = bar + 1;
        }
    }

    if (text)
        *text = value;
    return true;
}
