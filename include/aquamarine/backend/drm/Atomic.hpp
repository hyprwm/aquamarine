#pragma once

#include "../DRM.hpp"

namespace Aquamarine {
    class CDRMAtomicRequest;

    class CDRMAtomicImpl : public IDRMImplementation {
      public:
        CDRMAtomicImpl(Hyprutils::Memory::CSharedPointer<CDRMBackend> backend_);
        virtual bool                                         commit(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data);
        virtual bool                                         reset();
        virtual bool                                         moveCursor(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, bool skipSchedule = false);

        Hyprutils::Memory::CUniquePointer<CDRMAtomicRequest> prepareAsync(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data,
                                                                          uint32_t& flags);
        void finalizeAsync(CDRMAtomicRequest& request, Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data, bool success);

      private:
        bool                                         prepareConnector(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data);

        Hyprutils::Memory::CWeakPointer<CDRMBackend> backend;

        friend class CDRMAtomicRequest;
    };

    class CDRMAtomicRequest {
      public:
        struct SAsyncResult {
            bool submitted = false;
            int  error     = 0;
        };

        CDRMAtomicRequest(Hyprutils::Memory::CWeakPointer<CDRMBackend> backend);
        ~CDRMAtomicRequest();

        void         setConnector(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector);
        void         addConnector(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data);
        bool         restateConnectors(Hyprutils::Memory::CSharedPointer<SDRMConnector> self);
        void         addConnectorModeset(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data);
        void         addConnectorCursor(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data);
        bool         commit(uint32_t flagssss);
        void         add(uint32_t id, uint32_t prop, uint64_t val);
        bool         addRaw(uint32_t id, uint32_t prop, uint64_t val) noexcept;
        SAsyncResult submitAsync(int drmFD, uint32_t flags, uint64_t commitID) noexcept;
        void         planeProps(Hyprutils::Memory::CSharedPointer<SDRMPlane> plane, Hyprutils::Memory::CSharedPointer<CDRMFB> fb, uint32_t crtc, Hyprutils::Math::Vector2D pos,
                                eOutputColorRange colorRange = AQ_OUTPUT_COLOR_RANGE_AUTO);
        void         planePropsPos(Hyprutils::Memory::CSharedPointer<SDRMPlane> plane, Hyprutils::Math::Vector2D pos);

        void         resetProps(uint32_t id, const std::vector<uint32_t>& props);
        void         resetUnknownProps(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector);
        void         resetUnknownProps(Hyprutils::Memory::CSharedPointer<SDRMCRTC> crtc);
        void         resetUnknownProps(Hyprutils::Memory::CSharedPointer<SDRMPlane> plane);

        void         rollback(SDRMConnectorCommitData& data);
        void         apply(SDRMConnectorCommitData& data);

        bool         failed = false;

      private:
        void                                             destroyBlob(uint32_t id);
        void                                             commitBlob(uint32_t* current, uint32_t next);
        void                                             rollbackBlob(uint32_t* current, uint32_t next);

        Hyprutils::Memory::CWeakPointer<CDRMBackend>     backend;
        drmModeAtomicReq*                                req = nullptr;
        Hyprutils::Memory::CSharedPointer<SDRMConnector> conn;

        // mode blobs minted by restateConnectors, owned by this request
        std::vector<uint32_t> borrowedModeBlobs;
    };
};
