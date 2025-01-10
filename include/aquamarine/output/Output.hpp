#pragma once

#include <vector>
#include <optional>
#include <hyprutils/signal/Signal.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/math/Region.hpp>
#include <hyprutils/math/Mat3x3.hpp>
#include <drm_fourcc.h>
#include <xf86drmMode.h>
#include "../allocator/Swapchain.hpp"
#include "../buffer/Buffer.hpp"
#include "../backend/Misc.hpp"

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
            AQ_OUTPUT_STATE_DAMAGE             = (1 << 0),
            AQ_OUTPUT_STATE_ENABLED            = (1 << 1),
            AQ_OUTPUT_STATE_ADAPTIVE_SYNC      = (1 << 2),
            AQ_OUTPUT_STATE_PRESENTATION_MODE  = (1 << 3),
            AQ_OUTPUT_STATE_GAMMA_LUT          = (1 << 4),
            AQ_OUTPUT_STATE_MODE               = (1 << 5),
            AQ_OUTPUT_STATE_FORMAT             = (1 << 6),
            AQ_OUTPUT_STATE_BUFFER             = (1 << 7),
            AQ_OUTPUT_STATE_EXPLICIT_IN_FENCE  = (1 << 8),
            AQ_OUTPUT_STATE_EXPLICIT_OUT_FENCE = (1 << 9),
            AQ_OUTPUT_STATE_CTM                = (1 << 10),
            AQ_OUTPUT_STATE_HDR                = (1 << 11),
            AQ_OUTPUT_STATE_DEGAMMA_LUT        = (1 << 12),
            AQ_OUTPUT_STATE_WCG                = (1 << 13),
            AQ_OUTPUT_STATE_CURSOR_SHAPE       = (1 << 14),
            AQ_OUTPUT_STATE_CURSOR_POS         = (1 << 15),
        };

        struct SInternalState {
            uint32_t                                       committed = 0; // enum eOutputStateProperties

            Hyprutils::Math::CRegion                       damage;
            bool                                           enabled          = false;
            bool                                           adaptiveSync     = false;
            eOutputPresentationMode                        presentationMode = AQ_OUTPUT_PRESENTATION_VSYNC;
            std::vector<uint16_t>                          gammaLut;   // Gamma lut in the format [r,g,b]+
            std::vector<uint16_t>                          degammaLut; // Gamma lut in the format [r,g,b]+
            Hyprutils::Math::Vector2D                      lastModeSize;
            Hyprutils::Memory::CWeakPointer<SOutputMode>   mode;
            Hyprutils::Memory::CSharedPointer<SOutputMode> customMode;
            uint32_t                                       drmFormat = DRM_FORMAT_INVALID;
            Hyprutils::Memory::CSharedPointer<IBuffer>     buffer;
            int32_t                                        explicitInFence = -1, explicitOutFence = -1;
            Hyprutils::Math::Mat3x3                        ctm;
            bool                                           wideColorGamut = false;
            hdr_output_metadata                            hdrMetadata;
        };

        const SInternalState& state();

        void                  addDamage(const Hyprutils::Math::CRegion& region);
        void                  clearDamage();
        void                  setEnabled(bool enabled);
        void                  setAdaptiveSync(bool enabled);
        void                  setPresentationMode(eOutputPresentationMode mode);
        void                  setGammaLut(const std::vector<uint16_t>& lut);
        void                  setDeGammaLut(const std::vector<uint16_t>& lut);
        void                  setMode(Hyprutils::Memory::CSharedPointer<SOutputMode> mode);
        void                  setCustomMode(Hyprutils::Memory::CSharedPointer<SOutputMode> mode);
        void                  setFormat(uint32_t drmFormat);
        void                  setBuffer(Hyprutils::Memory::CSharedPointer<IBuffer> buffer);
        void                  setExplicitInFence(int32_t fenceFD); // -1 removes
        void                  enableExplicitOutFenceForNextCommit();
        void                  resetExplicitFences();
        void                  setCTM(const Hyprutils::Math::Mat3x3& ctm);
        void                  setWideColorGamut(bool wcg);
        void                  setHDRMetadata(const hdr_output_metadata& metadata);

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
        virtual ~IOutput();

        enum scheduleFrameReason : uint32_t {
            AQ_SCHEDULE_UNKNOWN = 0,
            AQ_SCHEDULE_NEW_CONNECTOR,
            AQ_SCHEDULE_CURSOR_VISIBLE,
            AQ_SCHEDULE_CURSOR_SHAPE,
            AQ_SCHEDULE_CURSOR_MOVE,
            AQ_SCHEDULE_CLIENT_UNKNOWN,
            AQ_SCHEDULE_DAMAGE,
            AQ_SCHEDULE_NEW_MONITOR,
            AQ_SCHEDULE_RENDER_MONITOR,
            AQ_SCHEDULE_NEEDS_FRAME,
            AQ_SCHEDULE_ANIMATION,
            AQ_SCHEDULE_ANIMATION_DAMAGE,
        };

        struct SHDRMetadata {
            float desiredContentMaxLuminance      = 0;
            float desiredMaxFrameAverageLuminance = 0;
            float desiredContentMinLuminance      = 0;
            bool  supportsPQ                      = false;
        };

        struct xy {
            double x = 0;
            double y = 0;
        };

        struct SChromaticityCoords {
            xy red;
            xy green;
            xy blue;
            xy white;
        };

        struct SParsedEDID {
            std::string                        make, serial, model;
            std::optional<SHDRMetadata>        hdrMetadata;
            std::optional<SChromaticityCoords> chromaticityCoords;
            bool                               supportsBT2020 = false;
        };

        virtual bool                                                      commit()           = 0;
        virtual bool                                                      test()             = 0;
        virtual Hyprutils::Memory::CSharedPointer<IBackendImplementation> getBackend()       = 0;
        virtual std::vector<SDRMFormat>                                   getRenderFormats() = 0;
        virtual Hyprutils::Memory::CSharedPointer<SOutputMode>            preferredMode();
        virtual bool                                                      setCursor(Hyprutils::Memory::CSharedPointer<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot);
        virtual void                                                      moveCursor(const Hyprutils::Math::Vector2D& coord, bool skipSchedule = false); // includes the hotspot
        virtual void                                                      setCursorVisible(bool visible); // moving the cursor will make it visible again without this util
        virtual Hyprutils::Math::Vector2D                                 cursorPlaneSize();              // -1, -1 means no set size, 0, 0 means error
        virtual void                                                      scheduleFrame(const scheduleFrameReason reason = AQ_SCHEDULE_UNKNOWN);
        virtual size_t                                                    getGammaSize();
        virtual size_t                                                    getDeGammaSize();
        virtual bool                                                      destroy(); // not all backends allow this!!!

        std::string                                                       name, description, make, model, serial;
        SParsedEDID                                                       parsedEDID;
        Hyprutils::Math::Vector2D                                         physicalSize;
        bool                                                              enabled    = false;
        bool                                                              nonDesktop = false;
        eSubpixelMode                                                     subpixel   = AQ_SUBPIXEL_NONE;
        bool                                                              vrrCapable = false, vrrActive = false;
        bool                                                              needsFrame       = false;
        bool                                                              supportsExplicit = false;

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
            Hyprutils::Math::Vector2D size; // if {0,0}, means it needs a reconfigure.
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
