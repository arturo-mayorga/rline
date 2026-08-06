#include "voice-grammar.h"

#include <cstdio>
#include <set>
#include <sstream>

std::string voice::normalisePhrase(const std::string &raw)
{
    std::string out;
    out.reserve(raw.size());

    bool pendingSpace = false;
    for (size_t i = 0; i < raw.size(); ++i)
    {
        unsigned char c = (unsigned char)raw[i];

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            if (!out.empty())
                pendingSpace = true;
            continue;
        }

        // Everything except letters, digits and apostrophes is dropped rather
        // than kept: SAPI builds a word transition per token, and a token with
        // a comma stuck to it never matches anything the driver actually says.
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '\'';
        if (!keep)
        {
            // A hyphen or slash separates words rather than joining them.
            if ((c == '-' || c == '/') && !out.empty())
                pendingSpace = true;
            continue;
        }

        if (pendingSpace)
        {
            out.push_back(' ');
            pendingSpace = false;
        }

        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        out.push_back((char)c);
    }

    if (out.size() > kMaxPhraseLen)
        return std::string();

    return out;
}

std::vector<std::string> voice::parseGrammar(const std::string &contents)
{
    std::vector<std::string> phrases;
    std::set<std::string> seen;

    std::istringstream in(contents);
    std::string line;
    while (std::getline(in, line))
    {
        // Comments must be tested before normalising, which would strip the '#'.
        size_t a = 0;
        while (a < line.size() && (line[a] == ' ' || line[a] == '\t'))
            ++a;
        if (a < line.size() && line[a] == '#')
            continue;

        const std::string p = normalisePhrase(line);
        if (p.empty())
            continue;

        if (seen.insert(p).second)
            phrases.push_back(p);

        if (phrases.size() >= kMaxPhrases)
            break;
    }

    return phrases;
}

std::vector<std::string> voice::loadGrammar(const std::string &path, std::string *error)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
    {
        if (error)
            *error = "cannot open " + path;
        return std::vector<std::string>();
    }

    std::string contents;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        contents.append(buf, n);
    fclose(f);

    std::vector<std::string> phrases = parseGrammar(contents);
    if (phrases.empty() && error)
        *error = path + " contained no usable phrases";
    return phrases;
}
