#pragma once

#include "../DRM.hpp"

namespace Aquamarine {
    class CDRMLegacyImpl : public IDRMImplementation {
      public:
        CDRMLegacyImpl(Hyprutils::Memory::CSharedPointer<CDRMBackend> backend_);
        virtual bool commit(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, const SDRMConnectorCommitData& data);

      private:

        bool commitInternal(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, const SDRMConnectorCommitData& data);
        bool testInternal(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, const SDRMConnectorCommitData& data);

        Hyprutils::Memory::CWeakPointer<CDRMBackend> backend;
    };
};
