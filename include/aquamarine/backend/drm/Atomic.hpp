#pragma once

#include "../DRM.hpp"

namespace Aquamarine {
    class CDRMAtomicImpl : public IDRMImplementation {
      public:
        CDRMAtomicImpl(Hyprutils::Memory::CSharedPointer<CDRMBackend> backend_);
        virtual bool commit(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, const SDRMConnectorCommitData& data);
        virtual bool reset(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector);
        virtual bool moveCursor(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector);

      private:
        bool                                         prepareConnector(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, const SDRMConnectorCommitData& data);

        Hyprutils::Memory::CWeakPointer<CDRMBackend> backend;

        friend class CDRMAtomicRequest;
    };

    class CDRMAtomicRequest {
      public:
        CDRMAtomicRequest(Hyprutils::Memory::CWeakPointer<CDRMBackend> backend);
        ~CDRMAtomicRequest();

        void addConnector(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, const SDRMConnectorCommitData& data);
        bool commit(uint32_t flagssss);
        void add(uint32_t id, uint32_t prop, uint64_t val);
        void planeProps(Hyprutils::Memory::CSharedPointer<SDRMPlane> plane, Hyprutils::Memory::CSharedPointer<CDRMFB> fb, uint32_t crtc, Hyprutils::Math::Vector2D pos);

        void rollback();
        void apply();

        bool failed = false;

      private:
        void                                             destroyBlob(uint32_t id);
        void                                             commitBlob(uint32_t* current, uint32_t next);
        void                                             rollbackBlob(uint32_t* current, uint32_t next);

        Hyprutils::Memory::CWeakPointer<CDRMBackend>     backend;
        drmModeAtomicReq*                                req = nullptr;
        Hyprutils::Memory::CSharedPointer<SDRMConnector> conn;
    };
};
