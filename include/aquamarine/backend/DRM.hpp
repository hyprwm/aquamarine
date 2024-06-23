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
    struct SDRMConnector;

    struct SDRMFB {
        uint32_t                                     id = 0;
        Hyprutils::Memory::CSharedPointer<IBuffer>   buffer;
        Hyprutils::Memory::CWeakPointer<CDRMBackend> backend;
    };

    struct SDRMLayer {
        Hyprutils::Memory::CSharedPointer<SDRMFB>    current /* displayed */, queued /* submitted */, pending /* to be submitted */;
        Hyprutils::Memory::CWeakPointer<CDRMBackend> backend;
    };

    struct SDRMPlane {
        bool                                         init(drmModePlane* plane);

        uint64_t                                     type      = 0;
        uint32_t                                     id        = 0;
        uint32_t                                     initialID = 0;

        Hyprutils::Memory::CSharedPointer<SDRMFB>    current /* displayed */, queued /* submitted */;
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

        Hyprutils::Memory::CWeakPointer<CDRMBackend>     backend;
        Hyprutils::Memory::CSharedPointer<SDRMConnector> connector;

        friend struct SDRMConnector;
    };

    struct SDRMConnector {
        ~SDRMConnector();

        bool                                           init(drmModeConnector* connector);
        void                                           connect(drmModeConnector* connector);
        void                                           disconnect();
        Hyprutils::Memory::CSharedPointer<SDRMCRTC>    getCurrentCRTC(const drmModeConnector* connector);
        drmModeModeInfo*                               getCurrentMode();
        void                                           parseEDID(std::vector<uint8_t> data);

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

        bool                                           cursorEnabled = false;
        Hyprutils::Math::Vector2D                      cursorPos, cursorSize, cursorHotspot;
        Hyprutils::Memory::CSharedPointer<SDRMFB>      pendingCursorFB;

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

      private:
        CDRMBackend(Hyprutils::Memory::CSharedPointer<CBackend> backend);

        static Hyprutils::Memory::CSharedPointer<CDRMBackend> attempt(Hyprutils::Memory::CSharedPointer<CBackend> backend);
        bool registerGPU(Hyprutils::Memory::CSharedPointer<CSessionDevice> gpu_, Hyprutils::Memory::CSharedPointer<CDRMBackend> primary_ = {});
        bool checkFeatures();
        bool initResources();
        bool grabFormats();
        void scanConnectors();

        Hyprutils::Memory::CSharedPointer<CSessionDevice>             gpu;
        Hyprutils::Memory::CWeakPointer<CDRMBackend>                  primary;

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

        friend class CBackend;
        friend struct SDRMFB;
        friend struct SDRMConnector;
        friend struct SDRMCRTC;
        friend struct SDRMPlane;
        friend struct CDRMOutput;
    };
};
