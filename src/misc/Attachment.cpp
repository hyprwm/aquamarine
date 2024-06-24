#include <aquamarine/misc/Attachment.hpp>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
#define SP CSharedPointer

bool Aquamarine::CAttachmentManager::has(eAttachmentType type) {
    for (auto& a : attachments) {
        if (a->type() == type)
            return true;
    }
    return false;
}

SP<IAttachment> Aquamarine::CAttachmentManager::get(eAttachmentType type) {
    for (auto& a : attachments) {
        if (a->type() == type)
            return a;
    }
    return nullptr;
}

void Aquamarine::CAttachmentManager::add(SP<IAttachment> attachment) {
    attachments.emplace_back(attachment);
}

void Aquamarine::CAttachmentManager::remove(SP<IAttachment> attachment) {
    std::erase(attachments, attachment);
}

void Aquamarine::CAttachmentManager::removeByType(eAttachmentType type) {
    std::erase_if(attachments, [type](const auto& e) { return e->type() == type; });
}
