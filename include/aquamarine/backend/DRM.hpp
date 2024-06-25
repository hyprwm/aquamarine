#pragma once

#include "./Backend.hpp"
#include "../allocator/Swapchain.hpp"
#include "../output/Output.hpp"
#include "../input/Input.hpp"
#include <hyprutils/memory/WeakPtr.hpp>
#include <wayland-client.h>
#include <xf86drmMode.h>

namespace Aquamarine {
    class CDRMBackend;
    class CDRMFB;
    struct SDRMConnector;

    typedef std::function<void(void)> FIdleCallback;

    class CDRMBufferUnimportable : public IAttachment {
      public:
        CDRMBufferUnimportable() {
            ;
        }
        virtual ~CDRMBufferUnimportable() {
            ;
        }
        virtual eAttachmentType type() {
            return AQ_ATTACHMENT_DRM_KMS_UNIMPORTABLE;
        }
    };

    class CDRMFB {
      public:
        ~CDRMFB();

        static Hyprutils::Memory::CSharedPointer<CDRMFB> create(Hyprutils::Memory::CSharedPointer<IBuffer> buffer_, Hyprutils::Memory::CWeakPointer<CDRMBackend> backend_);

        void                                             closeHandles();
        // drops the buffer from KMS
        void                                         drop();

        uint32_t                                     id = 0;
        Hyprutils::Memory::CSharedPointer<IBuffer>   buffer;
        Hyprutils::Memory::CWeakPointer<CDRMBackend> backend;

      private:
        CDRMFB(Hyprutils::Memory::CSharedPointer<IBuffer> buffer_, Hyprutils::Memory::CWeakPointer<CDRMBackend> backend_);
        uint32_t                submitBuffer();

        bool                    dropped = false, handlesClosed = false;

        std::array<uint32_t, 4> boHandles = {0};
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
            };
            uint32_t props[16] = {0};
        };
        UDRMPlaneProps props;
    };

    struct SDRMCRTC {
        uint32_t               id = 0;
        std::vector<SDRMLayer> layers;
        int32_t                refresh = 0;

        struct {
            int gammaSize = 0;
        } legacy;

        Hyprutils::Memory::CSharedPointer<SDRMPlane> primary;
        Hyprutils::Memory::CSharedPointer<SDRMPlane> cursor;
        Hyprutils::Memory::CWeakPointer<CDRMBackend> backend;

        union UDRMCRTCProps {
            struct {
                // None of these are guaranteed to exist
                uint32_t vrr_enabled;
                uint32_t gamma_lut;
                uint32_t gamma_lut_size;

                // atomic-modesetting only

                uint32_t active;
                uint32_t mode_id;
            };
            uint32_t props[6] = {0};
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
        virtual void                                                      moveCursor(const Hyprutils::Math::Vector2D& coord);
        virtual void                                                      scheduleFrame();
        virtual Hyprutils::Math::Vector2D                                 maxCursorSize();

        Hyprutils::Memory::CWeakPointer<CDRMOutput>                       self;

      private:
        CDRMOutput(const std::string& name_, Hyprutils::Memory::CWeakPointer<CDRMBackend> backend_, Hyprutils::Memory::CSharedPointer<SDRMConnector> connector_);

        bool                                             commitState(bool onlyTest = false);

        Hyprutils::Memory::CWeakPointer<CDRMBackend>     backend;
        Hyprutils::Memory::CSharedPointer<SDRMConnector> connector;

        friend struct SDRMConnector;
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

        void                                      calculateMode(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector);
    };

    struct SDRMConnector {
        ~SDRMConnector();

        bool                                           init(drmModeConnector* connector);
        void                                           connect(drmModeConnector* connector);
        void                                           disconnect();
        Hyprutils::Memory::CSharedPointer<SDRMCRTC>    getCurrentCRTC(const drmModeConnector* connector);
        drmModeModeInfo*                               getCurrentMode();
        void                                           parseEDID(std::vector<uint8_t> data);
        bool                                           commitState(const SDRMConnectorCommitData& data);
        void                                           applyCommit(const SDRMConnectorCommitData& data);
        void                                           rollbackCommit(const SDRMConnectorCommitData& data);
        void                                           onPresent();

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

        drmModeModeInfo                                fallbackModeInfo;

        union UDRMConnectorProps {
            struct {
                uint32_t edid;
                uint32_t dpms;
                uint32_t link_status; // not guaranteed to exist
                uint32_t path;
                uint32_t vrr_capable;  // not guaranteed to exist
                uint32_t subconnector; // not guaranteed to exist
                uint32_t non_desktop;
                uint32_t panel_orientation; // not guaranteed to exist
                uint32_t content_type;      // not guaranteed to exist
                uint32_t max_bpc;           // not guaranteed to exist

                // atomic-modesetting only

                uint32_t crtc_id;
            };
            uint32_t props[4] = {0};
        };
        UDRMConnectorProps props;
    };

    class IDRMImplementation {
      public:
        virtual bool commit(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, const SDRMConnectorCommitData& data) = 0;
        virtual bool reset(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector)                                       = 0;
    };

    class CDRMBackend : public IBackendImplementation {
      public:
        virtual ~CDRMBackend();
        virtual eBackendType                         type();
        virtual bool                                 start();
        virtual int                                  pollFD();
        virtual int                                  drmFD();
        virtual bool                                 dispatchEvents();
        virtual uint32_t                             capabilities();
        virtual bool                                 setCursor(Hyprutils::Memory::CSharedPointer<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot);
        virtual void                                 onReady();
        virtual std::vector<SDRMFormat>              getRenderFormats();
        virtual std::vector<SDRMFormat>              getCursorFormats();

        Hyprutils::Memory::CWeakPointer<CDRMBackend> self;

        void                                         log(eBackendLogLevel, const std::string&);
        bool                                         sessionActive();

        std::vector<FIdleCallback>                   idleCallbacks;

      private:
        CDRMBackend(Hyprutils::Memory::CSharedPointer<CBackend> backend);

        static Hyprutils::Memory::CSharedPointer<CDRMBackend> attempt(Hyprutils::Memory::CSharedPointer<CBackend> backend);
        bool registerGPU(Hyprutils::Memory::CSharedPointer<CSessionDevice> gpu_, Hyprutils::Memory::CSharedPointer<CDRMBackend> primary_ = {});
        bool checkFeatures();
        bool initResources();
        bool grabFormats();
        void scanConnectors();
        void restoreAfterVT();

        Hyprutils::Memory::CSharedPointer<CSessionDevice>             gpu;
        Hyprutils::Memory::CSharedPointer<IDRMImplementation>         impl;
        Hyprutils::Memory::CWeakPointer<CDRMBackend>                  primary;
        std::string                                                   gpuName;

        Hyprutils::Memory::CWeakPointer<CBackend>                     backend;

        std::vector<Hyprutils::Memory::CSharedPointer<SDRMCRTC>>      crtcs;
        std::vector<Hyprutils::Memory::CSharedPointer<SDRMPlane>>     planes;
        std::vector<Hyprutils::Memory::CSharedPointer<SDRMConnector>> connectors;
        std::vector<SDRMFormat>                                       formats;

        struct {
            Hyprutils::Math::Vector2D cursorSize;
            bool                      supportsAsyncCommit     = false;
            bool                      supportsAddFb2Modifiers = false;
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
    };
};
