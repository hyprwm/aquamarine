#pragma once

#include <vector>
#include <optional>
#include <hyprutils/signal/Signal.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/math/Region.hpp>
#include <drm_fourcc.h>
#include <xf86drmMode.h>
#include "../allocator/Swapchain.hpp"
#include "../buffer/Buffer.hpp"

namespace Aquamarine {

    class IBackendImplementation;

    struct SOutputMode {
        Hyprutils::Math::Vector2D      pixelSize;
        unsigned int                   refreshRate = 0 /* in mHz */;
        bool                           preferred   = false;
        std::optional<drmModeModeInfo> modeInfo; // if this is a drm mode, this will be populated.
    };

    enum eOutputPresentationMode : uint32_t {
        AQ_OUTPUT_PRESENTATION_VSYNC = 0,
        AQ_OUTPUT_PRESENTATION_IMMEDIATE, // likely tearing
    };

    enum eSubpixelMode : uint32_t {
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
        enum eOutputStateProperties : uint32_t {
            AQ_OUTPUT_STATE_DAMAGE            = (1 << 0),
            AQ_OUTPUT_STATE_ENABLED           = (1 << 1),
            AQ_OUTPUT_STATE_ADAPTIVE_SYNC     = (1 << 2),
            AQ_OUTPUT_STATE_PRESENTATION_MODE = (1 << 3),
            AQ_OUTPUT_STATE_GAMMA_LUT         = (1 << 4),
            AQ_OUTPUT_STATE_MODE              = (1 << 5),
            AQ_OUTPUT_STATE_FORMAT            = (1 << 6),
            AQ_OUTPUT_STATE_BUFFER            = (1 << 7),
        };

        struct SInternalState {
            uint32_t                                       committed = 0; // enum eOutputStateProperties

            Hyprutils::Math::CRegion                       damage;
            bool                                           enabled          = false;
            bool                                           adaptiveSync     = false;
            eOutputPresentationMode                        presentationMode = AQ_OUTPUT_PRESENTATION_VSYNC;
            std::vector<uint16_t>                          gammaLut; // Gamma lut in the format [r,g,b]+
            Hyprutils::Math::Vector2D                      lastModeSize;
            Hyprutils::Memory::CWeakPointer<SOutputMode>   mode;
            Hyprutils::Memory::CSharedPointer<SOutputMode> customMode;
            uint32_t                                       drmFormat = DRM_FORMAT_INVALID;
            Hyprutils::Memory::CSharedPointer<IBuffer>     buffer;
        };

        const SInternalState& state();

        void                  addDamage(const Hyprutils::Math::CRegion& region);
        void                  clearDamage();
        void                  setEnabled(bool enabled);
        void                  setAdaptiveSync(bool enabled);
        void                  setPresentationMode(eOutputPresentationMode mode);
        void                  setGammaLut(const std::vector<uint16_t>& lut);
        void                  setMode(Hyprutils::Memory::CSharedPointer<SOutputMode> mode);
        void                  setCustomMode(Hyprutils::Memory::CSharedPointer<SOutputMode> mode);
        void                  setFormat(uint32_t drmFormat);
        void                  setBuffer(Hyprutils::Memory::CSharedPointer<IBuffer> buffer);

      private:
        SInternalState internalState;

        void           onCommit(); // clears a few props like damage and committed.

        friend class IOutput;
        friend class CWaylandOutput;
        friend class CDRMOutput;
        friend class CHeadlessOutput;
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
        virtual void                                                      setCursorVisible(bool visible); // moving the cursor will make it visible again without this util
        virtual Hyprutils::Math::Vector2D                                 cursorPlaneSize();              // -1, -1 means no set size, 0, 0 means error
        virtual void                                                      scheduleFrame();
        virtual size_t                                                    getGammaSize();
        virtual bool                                                      destroy(); // not all backends allow this!!!

        std::string                                                       name, description, make, model, serial;
        Hyprutils::Math::Vector2D                                         physicalSize;
        bool                                                              enabled    = false;
        bool                                                              nonDesktop = false;
        eSubpixelMode                                                     subpixel   = AQ_SUBPIXEL_NONE;
        bool                                                              vrrCapable = false, vrrActive = false;
        bool                                                              needsFrame = false;

        //
        std::vector<Hyprutils::Memory::CSharedPointer<SOutputMode>> modes;
        Hyprutils::Memory::CSharedPointer<COutputState>             state = Hyprutils::Memory::makeShared<COutputState>();
        Hyprutils::Memory::CSharedPointer<CSwapchain>               swapchain;

        //

        enum eOutputPresentFlags : uint32_t {
            AQ_OUTPUT_PRESENT_VSYNC         = (1 << 0),
            AQ_OUTPUT_PRESENT_HW_CLOCK      = (1 << 1),
            AQ_OUTPUT_PRESENT_HW_COMPLETION = (1 << 2),
            AQ_OUTPUT_PRESENT_ZEROCOPY      = (1 << 3),
        };
        struct SStateEvent {
            Hyprutils::Math::Vector2D size;
        };

        struct SPresentEvent {
            bool         presented = true;
            timespec*    when      = nullptr;
            unsigned int seq       = 0;
            int          refresh   = 0;
            uint32_t     flags     = 0;
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