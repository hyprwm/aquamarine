#pragma once

#include <vector>
#include <optional>
#include <hyprutils/signal/Signal.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/math/Region.hpp>
#include <drm_fourcc.h>
#include "../allocator/Swapchain.hpp"
#include "../buffer/Buffer.hpp"

namespace Aquamarine {

    class IBackendImplementation;

    struct SOutputMode {
        Hyprutils::Math::Vector2D pixelSize;
        unsigned int              refreshRate = 0 /* in mHz */;
        bool                      preferred   = false;
    };

    enum eOutputPresentationMode {
        AQ_OUTPUT_PRESENTATION_VSYNC = 0,
        AQ_OUTPUT_PRESENTATION_IMMEDIATE, // likely tearing
    };

    enum eSubpixelMode {
        AQ_SUBPIXEL_UNKNOWN = 0,
        AQ_SUBPIXEL_NONE,
        AQ_SUBPIXEL_HORIZONTAL_RGB,
        AQ_SUBPIXEL_HORIZONTAL_BGR,
        AQ_SUBPIXEL_VERTICAL_RGB,
        AQ_SUBPIXEL_VERTICAL_BGR,
    };

    class IOutput;

    class COutputState {
      public:
        // TODO: make this state private, this sucks
        Hyprutils::Math::CRegion                       damage;
        bool                                           enabled          = false;
        bool                                           adaptiveSync     = false;
        eOutputPresentationMode                        presentationMode = AQ_OUTPUT_PRESENTATION_VSYNC;
        std::vector<uint16_t>                          gammaLut;
        Hyprutils::Math::Vector2D                      lastModeSize;
        Hyprutils::Memory::CWeakPointer<SOutputMode>   mode;
        Hyprutils::Memory::CSharedPointer<SOutputMode> customMode;
        uint32_t                                       drmFormat = DRM_FORMAT_INVALID;
        Hyprutils::Memory::CSharedPointer<IBuffer>     buffer;
    };

    class IOutput {
      public:
        virtual ~IOutput() {
            ;
        }

        virtual bool                                                      commit()     = 0;
        virtual bool                                                      test()       = 0;
        virtual Hyprutils::Memory::CSharedPointer<IBackendImplementation> getBackend() = 0;
        virtual Hyprutils::Memory::CSharedPointer<SOutputMode>            preferredMode();
        virtual bool                                                      setCursor(Hyprutils::Memory::CSharedPointer<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot);
        virtual void                                                      moveCursor(const Hyprutils::Math::Vector2D& coord); // includes the hotspot
        virtual Hyprutils::Math::Vector2D                                 maxCursorSize();                                    // -1, -1 means no limit, 0, 0 means error
        virtual void                                                      scheduleFrame();

        std::string                                                       name, description, make, model, serial;
        Hyprutils::Math::Vector2D                                         physicalSize;
        bool                                                              enabled      = false;
        bool                                                              adaptiveSync = false;
        bool                                                              nonDesktop   = false;
        eSubpixelMode                                                     subpixel     = AQ_SUBPIXEL_NONE;

        //
        std::vector<Hyprutils::Memory::CSharedPointer<SOutputMode>> modes;
        Hyprutils::Memory::CSharedPointer<COutputState>             state = Hyprutils::Memory::makeShared<COutputState>();
        Hyprutils::Memory::CSharedPointer<CSwapchain>               swapchain;

        //
        struct SStateEvent {
            Hyprutils::Math::Vector2D size;
        };

        struct {
            Hyprutils::Signal::CSignal destroy;
            Hyprutils::Signal::CSignal frame;
            Hyprutils::Signal::CSignal needsFrame;
            Hyprutils::Signal::CSignal present;
            Hyprutils::Signal::CSignal commit;
            Hyprutils::Signal::CSignal state;
        } events;
    };
}