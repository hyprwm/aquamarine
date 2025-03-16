#pragma once

#include "./Backend.hpp"
#include "../allocator/Swapchain.hpp"
#include "../output/Output.hpp"
#include "../input/Input.hpp"
#include <hyprutils/memory/WeakPtr.hpp>
#include <wayland-client.h>
#include <xf86drmMode.h>
#include <optional>

namespace Aquamarine {
    class CDRMBackend;
    class CDRMFB;
    class CDRMOutput;
    struct SDRMConnector;
    class CDRMRenderer;
    class CDRMDumbAllocator;

    typedef std::function<void(void)> FIdleCallback;

    class CDRMBufferAttachment : public IAttachment {
      public:
        CDRMBufferAttachment(Hyprutils::Memory::CSharedPointer<CDRMFB> fb_);
        virtual ~CDRMBufferAttachment() {
            ;
        }

        Hyprutils::Memory::CSharedPointer<CDRMFB> fb;
    };

    class CDRMBufferUnimportable : public IAttachment {
      public:
        CDRMBufferUnimportable() {
            ;
        }
        virtual ~CDRMBufferUnimportable() {
            ;
        }
    };

    class CDRMLease {
      public:
        static Hyprutils::Memory::CSharedPointer<CDRMLease> create(std::vector<Hyprutils::Memory::CSharedPointer<IOutput>> outputs);
        ~CDRMLease();

        void                                                     terminate();

        int                                                      leaseFD  = -1;
        uint32_t                                                 lesseeID = 0;
        Hyprutils::Memory::CWeakPointer<CDRMBackend>             backend;
        std::vector<Hyprutils::Memory::CWeakPointer<CDRMOutput>> outputs;
        bool                                                     active = true;

        struct {
            Hyprutils::Signal::CSignal destroy;
        } events;

      private:
        CDRMLease() = default;

        void destroy();

        friend class CDRMBackend;
    };

    class CDRMFB {
      public:
        ~CDRMFB();

        static Hyprutils::Memory::CSharedPointer<CDRMFB> create(Hyprutils::Memory::CSharedPointer<IBuffer> buffer_, Hyprutils::Memory::CWeakPointer<CDRMBackend> backend_,
                                                                bool* isNew = nullptr);

        void                                             closeHandles();
        // drops the buffer from KMS
        void drop();

        // re-imports the buffer into KMS. Essentially drop and import.
        void                                         reimport();

        uint32_t                                     id = 0;
        Hyprutils::Memory::CWeakPointer<IBuffer>     buffer;
        Hyprutils::Memory::CWeakPointer<CDRMBackend> backend;
        std::array<uint32_t, 4>                      boHandles = {0, 0, 0, 0};

        // true if the original buffer is gone and this has been released.
        bool dead = false;

      private:
        CDRMFB(Hyprutils::Memory::CSharedPointer<IBuffer> buffer_, Hyprutils::Memory::CWeakPointer<CDRMBackend> backend_);
        uint32_t submitBuffer();
        void     import();

        bool     dropped = false, handlesClosed = false;

        struct {
            Hyprutils::Signal::CHyprSignalListener destroyBuffer;
        } listeners;
    };

    struct SDRMLayer {
        // we expect the consumers to use double-buffering, so we keep the 2 last FBs around. If any of these goes out of
        // scope, the DRM FB will be destroyed, but the IBuffer will stay, as long as it's ref'd somewhere.
        Hyprutils::Memory::CSharedPointer<CDRMFB>    front /* currently displaying */, back /* submitted */, last /* keep just in case */;
        Hyprutils::Memory::CWeakPointer<CDRMBackend> backend;
    };

    struct SDRMPlane {
        bool                                         init(drmModePlane* plane);

        uint64_t                                     type      = 0;
        uint32_t                                     id        = 0;
        uint32_t                                     initialID = 0;

        Hyprutils::Memory::CSharedPointer<CDRMFB>    front /* currently displaying */, back /* submitted */, last /* keep just in case */;
        Hyprutils::Memory::CWeakPointer<CDRMBackend> backend;
        Hyprutils::Memory::CWeakPointer<SDRMPlane>   self;
        std::vector<SDRMFormat>                      formats;

        union UDRMPlaneProps {
            struct {
                uint32_t type;
                uint32_t rotation;   // Not guaranteed to exist
                uint32_t in_formats; // Not guaranteed to exist

                // atomic-modesetting only

                uint32_t src_x;
                uint32_t src_y;
                uint32_t src_w;
                uint32_t src_h;
                uint32_t crtc_x;
                uint32_t crtc_y;
                uint32_t crtc_w;
                uint32_t crtc_h;
                uint32_t fb_id;
                uint32_t crtc_id;
                uint32_t fb_damage_clips;
                uint32_t hotspot_x;
                uint32_t hotspot_y;
                uint32_t in_fence_fd;
            };
            uint32_t props[17] = {0};
        };
        UDRMPlaneProps props;
    };

    struct SDRMCRTC {
        uint32_t               id = 0;
        std::vector<SDRMLayer> layers;
        int32_t                refresh = 0; // unused

        struct {
            int gammaSize = 0;
        } legacy;

        struct {
            bool     ownModeID = false;
            uint32_t modeID    = 0;
            uint32_t gammaLut  = 0;
            uint32_t ctm       = 0;
        } atomic;

        Hyprutils::Memory::CSharedPointer<SDRMPlane> primary;
        Hyprutils::Memory::CSharedPointer<SDRMPlane> cursor;
        Hyprutils::Memory::CWeakPointer<CDRMBackend> backend;
        Hyprutils::Memory::CSharedPointer<CDRMFB>    pendingCursor;

        union UDRMCRTCProps {
            struct {
                // None of these are guaranteed to exist
                uint32_t vrr_enabled;
                uint32_t gamma_lut;
                uint32_t gamma_lut_size;
                uint32_t ctm;
                uint32_t degamma_lut;
                uint32_t degamma_lut_size;

                // atomic-modesetting only

                uint32_t active;
                uint32_t mode_id;
                uint32_t out_fence_ptr;
            };
            uint32_t props[9] = {0};
        };
        UDRMCRTCProps props;
    };

    class CDRMOutput : public IOutput {
      public:
        virtual ~CDRMOutput();
        virtual bool                                                      commit();
        virtual bool                                                      test();
        virtual Hyprutils::Memory::CSharedPointer<IBackendImplementation> getBackend();
        virtual bool                                                      setCursor(Hyprutils::Memory::CSharedPointer<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot);
        virtual void                                                      moveCursor(const Hyprutils::Math::Vector2D& coord, bool skipSchedule = false);
        virtual void                                                      scheduleFrame(const scheduleFrameReason reason = AQ_SCHEDULE_UNKNOWN);
        virtual void                                                      setCursorVisible(bool visible);
        virtual Hyprutils::Math::Vector2D                                 cursorPlaneSize();
        virtual size_t                                                    getGammaSize();
        virtual size_t                                                    getDeGammaSize();
        virtual std::vector<SDRMFormat>                                   getRenderFormats();

        int                                                               getConnectorID();

        Hyprutils::Memory::CWeakPointer<CDRMOutput>                       self;
        Hyprutils::Memory::CWeakPointer<CDRMLease>                        lease;
        bool                                                              cursorVisible = true;
        Hyprutils::Math::Vector2D                                         cursorPos; // without hotspot
        Hyprutils::Math::Vector2D                                         cursorHotspot;

        bool enabledState = true; // actual enabled state. Should be synced with state->state().enabled after a new frame

      private:
        CDRMOutput(const std::string& name_, Hyprutils::Memory::CWeakPointer<CDRMBackend> backend_, Hyprutils::Memory::CSharedPointer<SDRMConnector> connector_);

        bool                                                         commitState(bool onlyTest = false);

        Hyprutils::Memory::CWeakPointer<CDRMBackend>                 backend;
        Hyprutils::Memory::CSharedPointer<SDRMConnector>             connector;
        Hyprutils::Memory::CSharedPointer<std::function<void(void)>> frameIdle;

        struct {
            Hyprutils::Memory::CSharedPointer<CSwapchain> swapchain;
            Hyprutils::Memory::CSharedPointer<CSwapchain> cursorSwapchain;
        } mgpu;

        bool lastCommitNoBuffer = true;

        friend struct SDRMConnector;
        friend class CDRMLease;
    };

    struct SDRMPageFlip {
        Hyprutils::Memory::CWeakPointer<SDRMConnector> connector;
    };

    struct SDRMConnectorCommitData {
        Hyprutils::Memory::CSharedPointer<CDRMFB> mainFB, cursorFB;
        bool                                      modeset  = false;
        bool                                      blocking = false;
        uint32_t                                  flags    = 0;
        bool                                      test     = false;
        drmModeModeInfo                           modeInfo;
        std::optional<Hyprutils::Math::Mat3x3>    ctm;
        std::optional<hdr_output_metadata>        hdrMetadata;

        struct {
            uint32_t gammaLut   = 0;
            uint32_t degammaLut = 0;
            uint32_t fbDamage   = 0;
            uint32_t modeBlob   = 0;
            uint32_t ctmBlob    = 0;
            uint32_t hdrBlob    = 0;
            bool     blobbed    = false;
            bool     gammad     = false;
            bool     degammad   = false;
            bool     ctmd       = false;
            bool     hdrd       = false; // true if hdr blob needs updating or clearing
        } atomic;

        void calculateMode(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector);
    };

    struct SDRMConnector {
        ~SDRMConnector();

        bool                                           init(drmModeConnector* connector);
        void                                           connect(drmModeConnector* connector);
        void                                           disconnect();
        Hyprutils::Memory::CSharedPointer<SDRMCRTC>    getCurrentCRTC(const drmModeConnector* connector);
        drmModeModeInfo*                               getCurrentMode();
        IOutput::SParsedEDID                           parseEDID(std::vector<uint8_t> data);
        bool                                           commitState(SDRMConnectorCommitData& data);
        void                                           applyCommit(const SDRMConnectorCommitData& data);
        void                                           rollbackCommit(const SDRMConnectorCommitData& data);
        void                                           onPresent();
        void                                           recheckCRTCProps();

        Hyprutils::Memory::CSharedPointer<CDRMOutput>  output;
        Hyprutils::Memory::CWeakPointer<CDRMBackend>   backend;
        Hyprutils::Memory::CWeakPointer<SDRMConnector> self;
        std::string                                    szName;
        drmModeConnection                              status       = DRM_MODE_DISCONNECTED;
        uint32_t                                       id           = 0;
        std::array<uint64_t, 2>                        maxBpcBounds = {0, 0};
        Hyprutils::Memory::CSharedPointer<SDRMCRTC>    crtc;
        int32_t                                        refresh       = 0;
        uint32_t                                       possibleCrtcs = 0;
        std::string                                    make, serial, model;
        bool                                           canDoVrr = false;

        bool                                           cursorEnabled = false;
        Hyprutils::Math::Vector2D                      cursorPos, cursorSize, cursorHotspot;
        Hyprutils::Memory::CSharedPointer<CDRMFB>      pendingCursorFB;

        bool                                           isPageFlipPending = false;
        SDRMPageFlip                                   pendingPageFlip;
        bool                                           frameEventScheduled = false;

        // the current state is invalid and won't commit, don't try to modeset.
        bool                                           commitTainted = false;

        Hyprutils::Memory::CSharedPointer<SOutputMode> fallbackMode;

        struct {
            bool vrrEnabled = false;
        } atomic;

        union UDRMConnectorProps {
            struct {
                uint32_t edid;
                uint32_t dpms;
                uint32_t link_status; // not guaranteed to exist
                uint32_t path;
                uint32_t vrr_capable;  // not guaranteed to exist
                uint32_t subconnector; // not guaranteed to exist
                uint32_t non_desktop;
                uint32_t panel_orientation;   // not guaranteed to exist
                uint32_t content_type;        // not guaranteed to exist
                uint32_t max_bpc;             // not guaranteed to exist
                uint32_t Colorspace;          // not guaranteed to exist
                uint32_t hdr_output_metadata; // not guaranteed to exist

                // atomic-modesetting only

                uint32_t crtc_id;
            };
            uint32_t props[13] = {0};
        };
        UDRMConnectorProps props;

        union UDRMConnectorColorspace {
            struct {
                uint32_t Default;
                uint32_t BT2020_RGB;
                uint32_t BT2020_YCC;
            };
            uint32_t props[3] = {0};
        };
        UDRMConnectorColorspace colorspace;
    };

    class IDRMImplementation {
      public:
        virtual ~IDRMImplementation()                                                                                  = default;
        virtual bool commit(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data) = 0;
        virtual bool reset()                                                                                           = 0;

        // moving a cursor IIRC is almost instant on most hardware so we don't have to wait for a commit.
        virtual bool moveCursor(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, bool skipSchedule = false) = 0;
    };

    class CDRMBackend : public IBackendImplementation {
      public:
        virtual ~CDRMBackend();
        virtual eBackendType                                               type();
        virtual bool                                                       start();
        virtual std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>>    pollFDs();
        virtual int                                                        drmFD();
        virtual bool                                                       dispatchEvents();
        virtual uint32_t                                                   capabilities();
        virtual bool                                                       setCursor(Hyprutils::Memory::CSharedPointer<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot);
        virtual void                                                       onReady();
        virtual std::vector<SDRMFormat>                                    getRenderFormats();
        virtual std::vector<SDRMFormat>                                    getCursorFormats();
        virtual bool                                                       createOutput(const std::string& name = "");
        virtual Hyprutils::Memory::CSharedPointer<IAllocator>              preferredAllocator();
        virtual std::vector<SDRMFormat>                                    getRenderableFormats();
        virtual std::vector<Hyprutils::Memory::CSharedPointer<IAllocator>> getAllocators();
        virtual Hyprutils::Memory::CWeakPointer<IBackendImplementation>    getPrimary();

        Hyprutils::Memory::CWeakPointer<CDRMBackend>                       self;

        void                                                               log(eBackendLogLevel, const std::string&);
        bool                                                               sessionActive();
        int                                                                getNonMasterFD();

        std::vector<FIdleCallback>                                         idleCallbacks;
        std::string                                                        gpuName;

      private:
        CDRMBackend(Hyprutils::Memory::CSharedPointer<CBackend> backend);

        static std::vector<Hyprutils::Memory::CSharedPointer<CDRMBackend>> attempt(Hyprutils::Memory::CSharedPointer<CBackend> backend);
        static Hyprutils::Memory::CSharedPointer<CDRMBackend>              fromGpu(std::string path, Hyprutils::Memory::CSharedPointer<CBackend> backend,
                                                                                   Hyprutils::Memory::CSharedPointer<CDRMBackend> primary);

        bool registerGPU(Hyprutils::Memory::CSharedPointer<CSessionDevice> gpu_, Hyprutils::Memory::CSharedPointer<CDRMBackend> primary_ = {});
        bool checkFeatures();
        bool initResources();
        bool initMgpu();
        bool grabFormats();
        bool shouldBlit();
        void scanConnectors();
        void scanLeases();
        void restoreAfterVT();
        void recheckOutputs();
        void recheckCRTCs();
        void buildGlFormats(const std::vector<SGLFormat>& fmts);

        Hyprutils::Memory::CSharedPointer<CSessionDevice>     gpu;
        Hyprutils::Memory::CSharedPointer<IDRMImplementation> impl;
        Hyprutils::Memory::CWeakPointer<CDRMBackend>          primary;

        struct {
            Hyprutils::Memory::CSharedPointer<IAllocator>   allocator;
            Hyprutils::Memory::CSharedPointer<CDRMRenderer> renderer; // may be null if creation fails
        } rendererState;

        Hyprutils::Memory::CWeakPointer<CBackend>                     backend;

        std::vector<Hyprutils::Memory::CSharedPointer<SDRMCRTC>>      crtcs;
        std::vector<Hyprutils::Memory::CSharedPointer<SDRMPlane>>     planes;
        std::vector<Hyprutils::Memory::CSharedPointer<SDRMConnector>> connectors;
        std::vector<SDRMFormat>                                       formats;
        std::vector<SDRMFormat>                                       glFormats;

        Hyprutils::Memory::CSharedPointer<CDRMDumbAllocator>          dumbAllocator;

        bool                                                          atomic = false;

        struct {
            Hyprutils::Math::Vector2D cursorSize;
            bool                      supportsAsyncCommit     = false;
            bool                      supportsAddFb2Modifiers = false;
            bool                      supportsTimelines       = false;
        } drmProps;

        struct {
            Hyprutils::Signal::CHyprSignalListener sessionActivate;
            Hyprutils::Signal::CHyprSignalListener gpuChange;
            Hyprutils::Signal::CHyprSignalListener gpuRemove;
        } listeners;

        friend class CBackend;
        friend class CDRMFB;
        friend class CDRMFBAttachment;
        friend struct SDRMConnector;
        friend struct SDRMCRTC;
        friend struct SDRMPlane;
        friend class CDRMOutput;
        friend struct SDRMPageFlip;
        friend class CDRMLegacyImpl;
        friend class CDRMAtomicImpl;
        friend class CDRMAtomicRequest;
        friend class CDRMLease;
        friend class CGBMBuffer;
    };
};
