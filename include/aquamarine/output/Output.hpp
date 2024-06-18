#pragma once

#include <vector>
#include <hyprutils/signal/Signal.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/math/Region.hpp>

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
        COutputState(Hyprutils::Memory::CSharedPointer<IOutput> parent);

        Hyprutils::Math::CRegion                     damage;
        bool                                         enabled          = false;
        bool                                         adaptiveSync     = false;
        eOutputPresentationMode                      presentationMode = AQ_OUTPUT_PRESENTATION_VSYNC;
        std::vector<uint16_t>                        gammaLut;
        Hyprutils::Math::Vector2D                    lastModeSize;
        Hyprutils::Memory::CWeakPointer<SOutputMode> mode;
    };

    class IOutput {
      public:
        virtual ~IOutput() {
            ;
        }

        virtual void              commit() = 0;

        std::string               name, description, make, model, serial;
        Hyprutils::Math::Vector2D physicalSize;
        bool                      enabled      = false;
        bool                      adaptiveSync = false;
        bool                      nonDesktop   = false;

        //
        std::vector<Hyprutils::Memory::CSharedPointer<SOutputMode>> modes;
        Hyprutils::Memory::CSharedPointer<COutputState>             state;

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