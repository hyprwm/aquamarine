#pragma once

#include <hyprutils/signal/Signal.hpp>
#include <hyprutils/math/Vector2D.hpp>

struct libinput_device;

namespace Aquamarine {
    class IKeyboard {
      public:
        virtual ~IKeyboard() {
            events.destroy.emit();
        }
        virtual libinput_device*   getLibinputHandle();
        virtual const std::string& getName() = 0;

        struct SKeyEvent {
            uint32_t timeMs  = 0;
            uint32_t key     = 0;
            bool     pressed = false;
        };

        struct SModifiersEvent {
            uint32_t depressed = 0, latched = 0, locked = 0, group = 0;
        };

        struct {
            Hyprutils::Signal::CSignal destroy;
            Hyprutils::Signal::CSignal key;
            Hyprutils::Signal::CSignal modifiers;
        } events;
    };

    class IPointer {
      public:
        virtual ~IPointer() {
            events.destroy.emit();
        }

        virtual libinput_device*   getLibinputHandle();
        virtual const std::string& getName() = 0;

        enum ePointerAxis {
            AQ_POINTER_AXIS_VERTICAL = 0,
            AQ_POINTER_AXIS_HORIZONTAL,
        };

        struct SMoveEvent {
            Hyprutils::Math::Vector2D delta;
        };

        struct SWarpEvent {
            Hyprutils::Math::Vector2D absolute;
        };

        struct SButtonEvent {
            uint32_t timeMs  = 0;
            uint32_t button  = 0;
            bool     pressed = false;
        };

        struct SAxisEvent {
            uint32_t     timeMs = 0;
            ePointerAxis axis   = AQ_POINTER_AXIS_VERTICAL;
            double       value  = 0.0;
        };

        struct {
            Hyprutils::Signal::CSignal destroy;
            Hyprutils::Signal::CSignal move;
            Hyprutils::Signal::CSignal warp;
            Hyprutils::Signal::CSignal button;
            Hyprutils::Signal::CSignal axis;
            Hyprutils::Signal::CSignal frame;
        } events;
    };
}