#include <aquamarine/misc/Attachment.hpp>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
#define SP CSharedPointer

void Aquamarine::CAttachmentManager::add(SP<IAttachment> attachment) {
    const IAttachment& att   = *attachment;
    attachments[typeid(att)] = attachment;
}

void Aquamarine::CAttachmentManager::remove(SP<IAttachment> attachment) {
    const IAttachment& att = *attachment;
    auto               it  = attachments.find(typeid(att));
    if (it != attachments.end() && it->second == attachment)
        attachments.erase(it);
}

void Aquamarine::CAttachmentManager::clear() {
    attachments.clear();
}
