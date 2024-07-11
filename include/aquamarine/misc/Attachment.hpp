#pragma once

#include <vector>
#include <hyprutils/memory/SharedPtr.hpp>

namespace Aquamarine {
    enum eAttachmentType : uint32_t {
        AQ_ATTACHMENT_DRM_BUFFER = 0,
        AQ_ATTACHMENT_DRM_KMS_UNIMPORTABLE,
        AQ_ATTACHMENT_DRM_RENDERER_DATA,
    };

    class IAttachment {
      public:
        virtual ~IAttachment() {
            ;
        }

        virtual eAttachmentType type() = 0;
    };

    class CAttachmentManager {
      public:
        bool                                           has(eAttachmentType type);
        Hyprutils::Memory::CSharedPointer<IAttachment> get(eAttachmentType type);
        void                                           add(Hyprutils::Memory::CSharedPointer<IAttachment> attachment);
        void                                           remove(Hyprutils::Memory::CSharedPointer<IAttachment> attachment);
        void                                           removeByType(eAttachmentType type);
        void                                           clear();

      private:
        std::vector<Hyprutils::Memory::CSharedPointer<IAttachment>> attachments;
    };
};
