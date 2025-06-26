#pragma once

#include <hyprutils/signal/Signal.hpp>
#include <hyprutils/math/Vector2D.hpp>

struct libinput_device;

namespace Aquamarine {
    class ITabletTool;

    class IKeyboard {
      public:
        virtual ~IKeyboard() {
            events.destroy.emit();
        }
        virtual libinput_device*   getLibinputHandle();
        virtual const std::string& getName() = 0;
        virtual void               updateLEDs(uint32_t leds);

        struct SKeyEvent {
            uint32_t timeMs  = 0;
            uint32_t key     = 0;
            bool     pressed = false;
        };

        struct SModifiersEvent {
            uint32_t depressed = 0, latched = 0, locked = 0, group = 0;
        };

        struct {
            Hyprutils::Signal::CSignalT<>                destroy;
            Hyprutils::Signal::CSignalT<SKeyEvent>       key;
            Hyprutils::Signal::CSignalT<SModifiersEvent> modifiers;
        } events;
    };

    class IPointer {
      public:
        virtual ~IPointer() {
            events.destroy.emit();
        }

        virtual libinput_device*   getLibinputHandle();
        virtual const std::string& getName() = 0;

        enum ePointerAxis : uint32_t {
            AQ_POINTER_AXIS_VERTICAL = 0,
            AQ_POINTER_AXIS_HORIZONTAL,
        };

        enum ePointerAxisSource : uint32_t {
            AQ_POINTER_AXIS_SOURCE_WHEEL = 0,
            AQ_POINTER_AXIS_SOURCE_FINGER,
            AQ_POINTER_AXIS_SOURCE_CONTINUOUS,
            AQ_POINTER_AXIS_SOURCE_TILT,
        };

        enum ePointerAxisRelativeDirection : uint32_t {
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
            double                        delta = 0.0, discrete = 0.0;
        };

        struct SSwipeBeginEvent {
            uint32_t timeMs  = 0;
            uint32_t fingers = 0;
        };

        struct SSwipeUpdateEvent {
            uint32_t                  timeMs  = 0;
            uint32_t                  fingers = 0;
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
            uint32_t                  timeMs  = 0;
            uint32_t                  fingers = 0;
            Hyprutils::Math::Vector2D delta;
            double                    scale = 1.0, rotation = 0.0;
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
            Hyprutils::Signal::CSignalT<>                  destroy;
            Hyprutils::Signal::CSignalT<SMoveEvent>        move;
            Hyprutils::Signal::CSignalT<SWarpEvent>        warp;
            Hyprutils::Signal::CSignalT<SButtonEvent>      button;
            Hyprutils::Signal::CSignalT<SAxisEvent>        axis;
            Hyprutils::Signal::CSignalT<>                  frame;

            Hyprutils::Signal::CSignalT<SSwipeBeginEvent>  swipeBegin;
            Hyprutils::Signal::CSignalT<SSwipeUpdateEvent> swipeUpdate;
            Hyprutils::Signal::CSignalT<SSwipeEndEvent>    swipeEnd;

            Hyprutils::Signal::CSignalT<SPinchBeginEvent>  pinchBegin;
            Hyprutils::Signal::CSignalT<SPinchUpdateEvent> pinchUpdate;
            Hyprutils::Signal::CSignalT<SPinchEndEvent>    pinchEnd;

            Hyprutils::Signal::CSignalT<SHoldBeginEvent>   holdBegin;
            Hyprutils::Signal::CSignalT<SHoldEndEvent>     holdEnd;
        } events;
    };

    class ITouch {
      public:
        virtual ~ITouch() {
            events.destroy.emit();
        }

        virtual libinput_device*   getLibinputHandle();
        virtual const std::string& getName() = 0;

        Hyprutils::Math::Vector2D  physicalSize; // in mm, 0,0 if unknown

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
            Hyprutils::Signal::CSignalT<>             destroy;
            Hyprutils::Signal::CSignalT<SMotionEvent> move;
            Hyprutils::Signal::CSignalT<SDownEvent>   down;
            Hyprutils::Signal::CSignalT<SUpEvent>     up;
            Hyprutils::Signal::CSignalT<SCancelEvent> cancel;
            Hyprutils::Signal::CSignalT<>             frame;
        } events;
    };

    class ISwitch {
      public:
        virtual ~ISwitch() {
            events.destroy.emit();
        }

        virtual libinput_device*   getLibinputHandle();
        virtual const std::string& getName() = 0;

        enum eSwitchType : uint32_t {
            AQ_SWITCH_TYPE_UNKNOWN = 0,
            AQ_SWITCH_TYPE_LID,
            AQ_SWITCH_TYPE_TABLET_MODE,
        };

        struct SFireEvent {
            uint32_t    timeMs = 0;
            eSwitchType type   = AQ_SWITCH_TYPE_UNKNOWN;
            bool        enable = false;
        };

        struct {
            Hyprutils::Signal::CSignalT<>           destroy;
            Hyprutils::Signal::CSignalT<SFireEvent> fire;
        } events;
    };

    enum eTabletToolAxes : uint32_t {
        AQ_TABLET_TOOL_AXIS_X        = (1 << 0),
        AQ_TABLET_TOOL_AXIS_Y        = (1 << 1),
        AQ_TABLET_TOOL_AXIS_DISTANCE = (1 << 2),
        AQ_TABLET_TOOL_AXIS_PRESSURE = (1 << 3),
        AQ_TABLET_TOOL_AXIS_TILT_X   = (1 << 4),
        AQ_TABLET_TOOL_AXIS_TILT_Y   = (1 << 5),
        AQ_TABLET_TOOL_AXIS_ROTATION = (1 << 6),
        AQ_TABLET_TOOL_AXIS_SLIDER   = (1 << 7),
        AQ_TABLET_TOOL_AXIS_WHEEL    = (1 << 8),
    };

    class ITablet {
      public:
        virtual ~ITablet() {
            events.destroy.emit();
        }

        virtual libinput_device*   getLibinputHandle();
        virtual const std::string& getName() = 0;

        uint16_t                   usbVendorID = 0, usbProductID = 0;
        Hyprutils::Math::Vector2D  physicalSize; // mm
        std::vector<std::string>   paths;

        struct SAxisEvent {
            Hyprutils::Memory::CSharedPointer<ITabletTool> tool;

            uint32_t                                       timeMs = 0, updatedAxes = 0;
            Hyprutils::Math::Vector2D                      absolute;
            Hyprutils::Math::Vector2D                      delta;
            Hyprutils::Math::Vector2D                      tilt;
            double                                         pressure = 0.0, distance = 0.0, rotation = 0.0, slider = 0.0, wheelDelta = 0.0;
        };

        struct SProximityEvent {
            Hyprutils::Memory::CSharedPointer<ITabletTool> tool;

            uint32_t                                       timeMs = 0;
            Hyprutils::Math::Vector2D                      absolute;
            bool                                           in = false;
        };

        struct STipEvent {
            Hyprutils::Memory::CSharedPointer<ITabletTool> tool;

            uint32_t                                       timeMs = 0;
            Hyprutils::Math::Vector2D                      absolute;
            bool                                           down = false;
        };

        struct SButtonEvent {
            Hyprutils::Memory::CSharedPointer<ITabletTool> tool;

            uint32_t                                       timeMs = 0, button = 0;
            bool                                           down = false;
        };

        struct {
            Hyprutils::Signal::CSignalT<SAxisEvent>      axis;
            Hyprutils::Signal::CSignalT<SProximityEvent> proximity;
            Hyprutils::Signal::CSignalT<STipEvent>       tip;
            Hyprutils::Signal::CSignalT<SButtonEvent>    button;
            Hyprutils::Signal::CSignalT<>                destroy;
        } events;
    };

    class ITabletTool {
      public:
        virtual ~ITabletTool() {
            events.destroy.emit();
        }

        virtual libinput_device*   getLibinputHandle();
        virtual const std::string& getName() = 0;

        enum eTabletToolType : uint32_t {
            AQ_TABLET_TOOL_TYPE_INVALID = 0,
            AQ_TABLET_TOOL_TYPE_PEN,
            AQ_TABLET_TOOL_TYPE_ERASER,
            AQ_TABLET_TOOL_TYPE_BRUSH,
            AQ_TABLET_TOOL_TYPE_PENCIL,
            AQ_TABLET_TOOL_TYPE_AIRBRUSH,
            AQ_TABLET_TOOL_TYPE_MOUSE,
            AQ_TABLET_TOOL_TYPE_LENS,
            AQ_TABLET_TOOL_TYPE_TOTEM,
        };

        eTabletToolType type   = AQ_TABLET_TOOL_TYPE_INVALID;
        uint64_t        serial = 0, id = 0;

        enum eTabletToolCapabilities : uint32_t {
            AQ_TABLET_TOOL_CAPABILITY_TILT     = (1 << 0),
            AQ_TABLET_TOOL_CAPABILITY_PRESSURE = (1 << 1),
            AQ_TABLET_TOOL_CAPABILITY_DISTANCE = (1 << 2),
            AQ_TABLET_TOOL_CAPABILITY_ROTATION = (1 << 3),
            AQ_TABLET_TOOL_CAPABILITY_SLIDER   = (1 << 4),
            AQ_TABLET_TOOL_CAPABILITY_WHEEL    = (1 << 5),
        };

        uint32_t capabilities = 0; // enum eTabletToolCapabilities

        struct {
            Hyprutils::Signal::CSignalT<> destroy;
        } events;
    };

    class ITabletPad {
      public:
        virtual ~ITabletPad() {
            events.destroy.emit();
        }

        struct STabletPadGroup {
            std::vector<uint32_t> buttons, strips, rings;
            uint16_t              modes = 0;
        };

        virtual libinput_device*                                        getLibinputHandle();
        virtual const std::string&                                      getName() = 0;

        uint16_t                                                        buttons = 0, rings = 0, strips = 0;

        std::vector<std::string>                                        paths;
        std::vector<Hyprutils::Memory::CSharedPointer<STabletPadGroup>> groups;

        //

        struct SButtonEvent {
            uint32_t timeMs = 0, button = 0;
            bool     down = false;
            uint16_t mode = 0, group = 0;
        };

        enum eTabletPadRingSource : uint16_t {
            AQ_TABLET_PAD_RING_SOURCE_UNKNOWN = 0,
            AQ_TABLET_PAD_RING_SOURCE_FINGER,
        };

        enum eTabletPadStripSource : uint16_t {
            AQ_TABLET_PAD_STRIP_SOURCE_UNKNOWN = 0,
            AQ_TABLET_PAD_STRIP_SOURCE_FINGER,
        };

        struct SRingEvent {
            uint32_t             timeMs = 0;
            eTabletPadRingSource source = AQ_TABLET_PAD_RING_SOURCE_UNKNOWN;
            uint16_t             ring   = 0;
            double               pos    = 0.0;
            uint16_t             mode   = 0;
        };

        struct SStripEvent {
            uint32_t              timeMs = 0;
            eTabletPadStripSource source = AQ_TABLET_PAD_STRIP_SOURCE_UNKNOWN;
            uint16_t              strip  = 0;
            double                pos    = 0.0;
            uint16_t              mode   = 0;
        };

        struct {
            Hyprutils::Signal::CSignalT<>             destroy;
            Hyprutils::Signal::CSignalT<SButtonEvent> button;
            Hyprutils::Signal::CSignalT<SRingEvent>   ring;
            Hyprutils::Signal::CSignalT<SStripEvent>  strip;
            Hyprutils::Signal::CSignalT<>             attach;
        } events;
    };
}
