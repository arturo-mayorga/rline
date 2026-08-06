#ifndef talk_button_h_
#define talk_button_h_

#include <string>

// Which wheel button means "I want to say something".
//
// Read straight off the joystick rather than through iRacing. The obvious
// route - binding the button to iRacing's push-to-talk and watching the
// PushToTalk telemetry channel - costs no code at all, but that button opens
// voice chat to everyone in the session. Talking to your coach is not something
// to broadcast to the people you are racing.
//
// A button is identified as <device>:<button>, both zero-based, matching the
// legacy joystick API's numbering. The driver never has to work these out:
// `rline --bind-talk` waits for a press and writes the answer to
// rline-talk.txt beside the exe.
namespace talk
{
    struct Spec
    {
        int device = -1;
        int button = -1;

        bool valid() const { return device >= 0 && button >= 0; }
    };

    // "0:7" -> {0, 7}. Rejects anything else, including negative indices and
    // trailing junk, so a typo in the config file is reported rather than
    // silently binding button 0.
    bool parseSpec(const std::string &text, Spec *out);

    std::string formatSpec(const Spec &s);
}

#endif
