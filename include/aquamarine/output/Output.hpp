#pragma once

#include <vector>
#include <hyprutils/signal/Signal.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/math/Region.hpp>
#include <drm_fourcc.h>
#include "../allocator/Swapchain.hpp"
#include "../buffer/Buffer.hpp"

namespace Aquamarine {
    struct SOutputMode {
        Hyprutils::Math::Vector2D pixelSize;
        unsigned int              refreshRate = 0 /* in mHz */;
        bool                      preferred   = false;
    };

    enum eOutputPresentationMode {
        AQ_OUTPUT_PRESENTATION_VSYNC = 0,
        AQ_OUTPUT_PRESENTATION_IMMEDIATE, // likely tearing
    };

    class IOutput;

    class COutputState {
      public:
        Hyprutils::Math::CRegion                     damage;
        bool                                         enabled          = false;
        bool                                         adaptiveSync     = false;
        eOutputPresentationMode                      presentationMode = AQ_OUTPUT_PRESENTATION_VSYNC;
        std::vector<uint16_t>                        gammaLut;
        Hyprutils::Math::Vector2D                    lastModeSize;
        Hyprutils::Memory::CWeakPointer<SOutputMode> mode;
        std::optional<SOutputMode>                   customMode;
        uint32_t                                     drmFormat = DRM_FORMAT_INVALID;
        Hyprutils::Memory::CSharedPointer<IBuffer>   buffer;
    };

    class IOutput {
      public:
        virtual ~IOutput() {
            ;
        }

        virtual bool              commit() = 0;

        std::string               name, description, make, model, serial;
        Hyprutils::Math::Vector2D physicalSize;
        bool                      enabled      = false;
        bool                      adaptiveSync = false;
        bool                      nonDesktop   = false;

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
            Hyprutils::Signal::CSignal state;
        } events;
    };
}