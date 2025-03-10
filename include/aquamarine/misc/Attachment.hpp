#pragma once

#include <vector>
#include <hyprutils/memory/SharedPtr.hpp>
#include <unordered_map>
#include <typeindex>

namespace Aquamarine {
    class IAttachment {
      public:
        virtual ~IAttachment() {
            ;
        }
    };

    template <typename T>
    concept AttachmentConcept = std::is_base_of_v<IAttachment, T>;

    // CAttachmentManager is a registry for arbitrary attachment types.
    // Any type implementing IAttachment can be added, retrieved, and removed from the registry.
    // However, only one attachment of a given type is permitted.
    class CAttachmentManager {
      public:
        template <AttachmentConcept T>
        bool has() const {
            return attachments.contains(typeid(T));
        }
        template <AttachmentConcept T>
        Hyprutils::Memory::CSharedPointer<T> get() const {
            auto it = attachments.find(typeid(T));
            if (it == attachments.end())
                return nullptr;
            // Reinterpret SP<IAttachment> into SP<T>.
            // This is safe because we looked up this attachment by typeid(T),
            // so it must be an SP<T>.
            return Hyprutils::Memory::reinterpretPointerCast<T>(it->second);
        }
        // Also removes the previous attachment of the same type if one exists
        void add(Hyprutils::Memory::CSharedPointer<IAttachment> attachment);
        void remove(Hyprutils::Memory::CSharedPointer<IAttachment> attachment);
        template <AttachmentConcept T>
        void removeByType() {
            attachments.erase(typeid(T));
        }
        void clear();

      private:
        std::unordered_map<std::type_index, Hyprutils::Memory::CSharedPointer<IAttachment>> attachments;
    };
};
