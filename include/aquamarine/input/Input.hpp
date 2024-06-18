#pragma once

#include <hyprutils/signal/Signal.hpp>

struct libinput_device;

namespace Aquamarine {
    class IKeyboard {
      public:
        virtual libinput_device* getLibinputHandle();

        struct {
            Hyprutils::Signal::CSignal destroy;
            Hyprutils::Signal::CSignal key;
            Hyprutils::Signal::CSignal modifiers;
        } events;
    };

    class IPointer {
      public:
        virtual libinput_device* getLibinputHandle();

        struct {
            Hyprutils::Signal::CSignal destroy;
            Hyprutils::Signal::CSignal move;
            Hyprutils::Signal::CSignal warp;
            Hyprutils::Signal::CSignal button;
            Hyprutils::Signal::CSignal axis;
        } events;
    };
}