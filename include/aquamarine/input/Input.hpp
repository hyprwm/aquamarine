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
        virtual void updateLEDs(uint32_t leds);

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

        enum ePointerAxisSource {
            AQ_POINTER_AXIS_SOURCE_WHEEL = 0,
            AQ_POINTER_AXIS_SOURCE_FINGER,
            AQ_POINTER_AXIS_SOURCE_CONTINUOUS,
            AQ_POINTER_AXIS_SOURCE_TILT,
        };

        enum ePointerAxisRelativeDirection {
            AQ_POINTER_AXIS_RELATIVE_IDENTICAL = 0,
            AQ_POINTER_AXIS_RELATIVE_INVERTED,
        };

        struct SMoveEvent {
            uint32_t                  timeMs = 0;
            Hyprutils::Math::Vector2D delta, unaccel;
        };

        struct SWarpEvent {
            uint32_t                  timeMs = 0;
            Hyprutils::Math::Vector2D absolute;
        };

        struct SButtonEvent {
            uint32_t timeMs  = 0;
            uint32_t button  = 0;
            bool     pressed = false;
        };

        struct SAxisEvent {
            uint32_t                      timeMs    = 0;
            ePointerAxis                  axis      = AQ_POINTER_AXIS_VERTICAL;
            ePointerAxisSource            source    = AQ_POINTER_AXIS_SOURCE_WHEEL;
            ePointerAxisRelativeDirection direction = AQ_POINTER_AXIS_RELATIVE_IDENTICAL;
            double                        delta     = 0.0, discrete = 0.0;
        };

        struct SSwipeBeginEvent {
            uint32_t timeMs  = 0;
            uint32_t fingers = 0;
        };

        struct SSwipeUpdateEvent {
            uint32_t timeMs  = 0;
            uint32_t fingers = 0;
            Hyprutils::Math::Vector2D delta;
        };

        struct SSwipeEndEvent {
            uint32_t timeMs    = 0;
            bool     cancelled = false;
        };

        struct SPinchBeginEvent {
            uint32_t timeMs  = 0;
            uint32_t fingers = 0;
        };

        struct SPinchUpdateEvent {
            uint32_t timeMs  = 0;
            uint32_t fingers = 0;
            Hyprutils::Math::Vector2D delta;
            double   scale = 1.0, rotation = 0.0;
        };

        struct SPinchEndEvent {
            uint32_t timeMs    = 0;
            bool     cancelled = false;
        };

        struct SHoldBeginEvent {
            uint32_t timeMs  = 0;
            uint32_t fingers = 0;
        };

        struct SHoldEndEvent {
            uint32_t timeMs    = 0;
            bool     cancelled = false;
        };

        struct {
            Hyprutils::Signal::CSignal destroy;
            Hyprutils::Signal::CSignal move;
            Hyprutils::Signal::CSignal warp;
            Hyprutils::Signal::CSignal button;
            Hyprutils::Signal::CSignal axis;
            Hyprutils::Signal::CSignal frame;

            Hyprutils::Signal::CSignal swipeBegin;
            Hyprutils::Signal::CSignal swipeUpdate;
            Hyprutils::Signal::CSignal swipeEnd;

            Hyprutils::Signal::CSignal pinchBegin;
            Hyprutils::Signal::CSignal pinchUpdate;
            Hyprutils::Signal::CSignal pinchEnd;

            Hyprutils::Signal::CSignal holdBegin;
            Hyprutils::Signal::CSignal holdEnd;
        } events;
    };

    class ITouch {
      public:
        virtual ~ITouch() {
            events.destroy.emit();
        }

        virtual libinput_device*   getLibinputHandle();
        virtual const std::string& getName() = 0;

        Hyprutils::Math::Vector2D physicalSize; // in mm, 0,0 if unknown

        struct SDownEvent {
            uint32_t                  timeMs  = 0;
            int32_t                   touchID = 0;
            Hyprutils::Math::Vector2D pos;
        };

        struct SUpEvent {
            uint32_t timeMs  = 0;
            int32_t  touchID = 0;
        };

        struct SMotionEvent {
            uint32_t                  timeMs  = 0;
            int32_t                   touchID = 0;
            Hyprutils::Math::Vector2D pos;
        };

        struct SCancelEvent {
            uint32_t timeMs  = 0;
            int32_t  touchID = 0;
        };

        struct {
            Hyprutils::Signal::CSignal destroy;
            Hyprutils::Signal::CSignal move;
            Hyprutils::Signal::CSignal down;
            Hyprutils::Signal::CSignal up;
            Hyprutils::Signal::CSignal cancel;
            Hyprutils::Signal::CSignal frame;
        } events;
    };

    class ITablet {
      public:
        // FIXME:

        struct {
            Hyprutils::Signal::CSignal destroy;
        } events;
    };

    class ITabletTool {
      public:
        // FIXME:

        struct {
            Hyprutils::Signal::CSignal destroy;
        } events;
    };

    class ITabletPad {
      public:
        // FIXME:

        struct {
            Hyprutils::Signal::CSignal destroy;
        } events;
    };
}