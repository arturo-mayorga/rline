// Which button the driver has to press to talk. Reading the wheel directly
// rather than through iRacing's push-to-talk, because that one broadcasts to
// everyone in the session.

#include "../src/talk-button.h"

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
    printf("-- parsing a binding --\n");
    {
        talk::Spec s;
        check(talk::parseSpec("0:7", &s) && s.device == 0 && s.button == 7,
              "device and button are read out");
        check(talk::parseSpec("1:23", &s) && s.device == 1 && s.button == 23,
              "two-digit button indices work");
        check(talk::parseSpec("  2:5\r\n", &s) && s.device == 2 && s.button == 5,
              "whitespace and CRLF from a hand-edited file are tolerated");
    }

    printf("\n-- a typo is refused, not silently bound to button zero --\n");
    {
        // The failure that matters: a bad config binding button 0 would look
        // exactly like broken hardware, because button 0 is usually a paddle.
        talk::Spec s;
        check(!talk::parseSpec("", &s), "empty");
        check(!talk::parseSpec("7", &s), "no colon");
        check(!talk::parseSpec("0:", &s), "no button");
        check(!talk::parseSpec(":7", &s), "no device");
        check(!talk::parseSpec("a:7", &s), "non-numeric device");
        check(!talk::parseSpec("0:b", &s), "non-numeric button");
        check(!talk::parseSpec("-1:7", &s), "negative device");
        check(!talk::parseSpec("0:-1", &s), "negative button");
        check(!talk::parseSpec("0x0:7", &s), "hex is not accepted");
        check(!talk::parseSpec("0:7:2", &s), "trailing junk");
        check(!talk::parseSpec("0 : 7", &s), "inner spaces are not a valid spec");
    }

    printf("\n-- an unset binding is not a valid one --\n");
    {
        talk::Spec none;
        check(!none.valid(), "a default Spec is invalid, so nothing is bound by accident");
        check(talk::formatSpec(none).empty(), "and formats to nothing");
    }

    printf("\n-- round trip --\n");
    {
        talk::Spec s;
        talk::parseSpec("3:14", &s);
        check(talk::formatSpec(s) == "3:14", "formats back to what was parsed");

        talk::Spec again;
        check(talk::parseSpec(talk::formatSpec(s), &again) &&
                  again.device == s.device && again.button == s.button,
              "survives a save and reload");
    }

    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASSED" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
