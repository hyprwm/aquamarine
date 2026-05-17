#pragma once

#include <chrono>
#include <array>
#include <cstdint>
#include <vector>
#include <optional>
#include <hyprutils/signal/Signal.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/math/Region.hpp>
#include <hyprutils/math/Mat3x3.hpp>
#include <hyprutils/os/FileDescriptor.hpp>
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

    enum eOutputColorRange : uint32_t {
        AQ_OUTPUT_COLOR_RANGE_AUTO = 0, // leave the driver default
        AQ_OUTPUT_COLOR_RANGE_FULL,     // force full range
        AQ_OUTPUT_COLOR_RANGE_LIMITED,  // force limited (16-235) range
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
            AQ_OUTPUT_STATE_PLANE_STATE        = (1 << 16),
        };

        struct SPlaneState {
            bool                                       updated = false;

            bool                                       enabled = false;
            Hyprutils::Math::CRegion                   damage;
            Hyprutils::Math::CBox                      geometry;
            Hyprutils::Memory::CSharedPointer<IBuffer> buffer;
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
            Hyprutils::Math::Mat3x3                        ctm            = Hyprutils::Math::Mat3x3::identity();
            bool                                           wideColorGamut = false;
            hdr_output_metadata                            hdrMetadata    = {};
            uint16_t                                       contentType    = DRM_MODE_CONTENT_TYPE_GRAPHICS;
            eOutputColorRange                              colorRange     = AQ_OUTPUT_COLOR_RANGE_AUTO;
            std::vector<SPlaneState>                       planeStates;
        };

        class CSnapshot {
          public:
            CSnapshot(CSnapshot&&) = default;

            const SInternalState& state() const;
            bool                  needsReconfig() const;
            int                   error() const;

          private:
            CSnapshot(const COutputState* owner, const SInternalState& state, const std::array<uint64_t, 16>& generations);

            const COutputState*            m_owner = nullptr;
            SInternalState                 m_state;
            std::array<uint64_t, 16>       m_generations = {};
            Hyprutils::OS::CFileDescriptor m_explicitInFence;
            int                            m_error = 0;

            friend class COutputState;
        };

        const SInternalState& state();
        const SInternalState& state() const;
        CSnapshot             snapshot() const;
        void                  consume(const CSnapshot& snapshot);
        void                  rearm(const CSnapshot& snapshot);

        bool                  needsReconfig() const;
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
        void                  setContentType(const uint16_t drmContentType);
        void                  setColorRange(eOutputColorRange range);

        void                  setPlaneEnabled(uint32_t planeIdx, bool enabled);
        void                  setPlaneBuffer(uint32_t planeIdx, Hyprutils::Memory::CSharedPointer<IBuffer> buffer);
        void                  setPlaneGeometry(uint32_t planeIdx, const Hyprutils::Math::CBox& box);
        void                  addPlaneDamage(uint32_t planeIdx, const Hyprutils::Math::CRegion& region);

      private:
        SInternalState           internalState;
        std::array<uint64_t, 16> propertyGenerations = {};
        uint64_t                 nextGeneration      = 0;

        void                     markCommitted(uint32_t properties);

        friend class IOutput;
        friend class CWaylandOutput;
        friend class CDRMOutput;
        friend class CDRMBackend;
        friend class CHeadlessOutput;
        friend struct SDRMConnector;
    };

    class IOutput {
      public:
        virtual ~IOutput();

        enum eOutputCommitCapabilities : uint32_t {
            AQ_OUTPUT_COMMIT_CAPABILITY_QUEUED      = (1 << 0), // Supports commitAsync.
            AQ_OUTPUT_COMMIT_CAPABILITY_TIMED       = (1 << 1), // Supports targetPresentation.
            AQ_OUTPUT_COMMIT_CAPABILITY_VRR         = (1 << 2), // Supports queued commits while adaptive sync is active.
            AQ_OUTPUT_COMMIT_CAPABILITY_TEARING     = (1 << 3), // Supports queued immediate-presentation commits.
            AQ_OUTPUT_COMMIT_CAPABILITY_LATE_CURSOR = (1 << 4), // Supports lateCursor.
        };

        enum eOutputCommitStatus : uint32_t {
            AQ_OUTPUT_COMMIT_SUBMITTED = 0,
            AQ_OUTPUT_COMMIT_FAILED,
            AQ_OUTPUT_COMMIT_CANCELLED,
        };

        // commitAsync snapshots pending state before returning. Requesting an option without its corresponding capability is rejected with ENOTSUP.
        struct SCommitOptions {
            // requires TIMED when set. The value is the desired presentation time; nullopt requests immediate submission.
            std::optional<std::chrono::steady_clock::time_point> targetPresentation;
            // requires LATE_CURSOR when set. The cursor position is the only state allowed to change after commitAsync returns.
            bool lateCursor = false;
        };

        // rejection leaves pending output state untouched. Pending VRR or tearing state requires the corresponding capability.
        struct SCommitSubmission {
            uint64_t id    = 0; // 0 means rejected.
            int      error = 0; // Positive errno, 0 on acceptance.
        };

        // every accepted id receives exactly one result after commitAsync returns, on the backend's dispatch thread. A submitted result precedes its presentation.
        // cancellation results are emitted before destroy.
        struct SCommitResult {
            uint64_t            id           = 0;
            eOutputCommitStatus status       = AQ_OUTPUT_COMMIT_FAILED;
            int                 error        = 0;     // positive errno, 0 when submitted.
            bool                missedTarget = false; // backend submission completed after targetPresentation. Valid only for timed, submitted commits.
        };

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

        enum ePlaneType : uint32_t {
            AQ_PLANE_UNKNOWN = 0,
            AQ_PLANE_PRIMARY,
            AQ_PLANE_GENERIC,
            AQ_PLANE_CURSOR,
        };

        struct SPlaneData {
            std::vector<SDRMFormat>                       renderFormats; // empty if unknown / not specified -> use getRenderFormats()
            ePlaneType                                    type  = AQ_PLANE_UNKNOWN;
            uint32_t                                      id    = 0;
            uint32_t                                      index = 0;
            Hyprutils::Memory::CSharedPointer<CSwapchain> swapchain;
        };

        virtual bool                                                      commit()           = 0;
        virtual bool                                                      test()             = 0;
        virtual Hyprutils::Memory::CSharedPointer<IBackendImplementation> getBackend()       = 0;
        virtual std::vector<SDRMFormat>                                   getRenderFormats() = 0;
        virtual Hyprutils::Memory::CSharedPointer<SOutputMode>            preferredMode();
        virtual bool                                                      setCursor(Hyprutils::Memory::CSharedPointer<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot);
        virtual void                                                      moveCursor(const Hyprutils::Math::Vector2D& coord, bool skipSchedule = false); // includes the hotspot
        virtual void                                                      setCursorVisible(bool visible); // moving the cursor will make it visible again without this util
        virtual bool                                                      hasCursorPlane() const;
        virtual Hyprutils::Math::Vector2D                                 cursorPlaneSize(); // -1, -1 means no set size, 0, 0 means error
        virtual std::optional<std::chrono::steady_clock::time_point>      nextVBlank() const;
        virtual void                                                      scheduleFrame(const scheduleFrameReason reason = AQ_SCHEDULE_UNKNOWN);
        virtual size_t                                                    getGammaSize();
        virtual size_t                                                    getDeGammaSize();
        virtual bool                                                      destroy(); // not all backends allow this!!!
        virtual bool                                                      pendingPageFlip()  = 0;
        virtual bool                                                      pendingIdleFrame() = 0;
        virtual uint32_t                                                  commitCapabilities() const;
        virtual SCommitSubmission                                         commitAsync(const SCommitOptions& options);
        virtual std::vector<SPlaneData>                                   getPlanes();
        virtual std::optional<SPlaneData>                                 getOverlayPlane();

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
        Hyprutils::Memory::CSharedPointer<CSwapchain>               overlaySwapchain;

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
            uint64_t     commitID  = 0; // 0 means the presentation is not associated with a queued commit.
        };

        struct {
            Hyprutils::Signal::CSignalT<>              destroy;
            Hyprutils::Signal::CSignalT<>              frame;
            Hyprutils::Signal::CSignalT<>              needsFrame;
            Hyprutils::Signal::CSignalT<SPresentEvent> present;
            Hyprutils::Signal::CSignalT<>              commit;
            Hyprutils::Signal::CSignalT<SStateEvent>   state;
            Hyprutils::Signal::CSignalT<SCommitResult> commitResult;
        } events;
    };
}
