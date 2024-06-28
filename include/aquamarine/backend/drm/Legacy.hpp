#pragma once

#include "../DRM.hpp"

namespace Aquamarine {
    class CDRMLegacyImpl : public IDRMImplementation {
      public:
        CDRMLegacyImpl(Hyprutils::Memory::CSharedPointer<CDRMBackend> backend_);
        virtual bool commit(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data);
        virtual bool reset(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector);
        virtual bool moveCursor(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector);

      private:

        bool commitInternal(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data);
        bool testInternal(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data);

        Hyprutils::Memory::CWeakPointer<CDRMBackend> backend;
    };
};
