#pragma once

#include "../DRM.hpp"

namespace Aquamarine {
    class CDRMLegacyImpl : public IDRMImplementation {
      public:
        CDRMLegacyImpl(Hyprutils::Memory::CSharedPointer<CDRMBackend> backend_);
        virtual bool commit(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data);
        virtual bool reset();
        virtual bool moveCursor(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, bool skipShedule = false);

      private:
        bool                                         commitInternal(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data);
        bool                                         testInternal(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data);

        Hyprutils::Memory::CWeakPointer<CDRMBackend> backend;
    };
};
