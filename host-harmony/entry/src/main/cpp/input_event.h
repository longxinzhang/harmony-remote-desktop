#ifndef HARMONY_REMOTE_INPUT_EVENT_H
#define HARMONY_REMOTE_INPUT_EVENT_H

#include <string>

struct RemoteInputEvent {
    enum class Kind { Move, Button, Scroll, Key };
    Kind kind = Kind::Move;
    double x = 0;
    double y = 0;
    double dx = 0;
    double dy = 0;
    std::string button;
    std::string code;
    bool down = false;
};

#endif
