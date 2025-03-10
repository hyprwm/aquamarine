#include <aquamarine/misc/Attachment.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/memory/WeakPtr.hpp>
#include "shared.hpp"

class CFooAttachment : public Aquamarine::IAttachment {
  public:
    int counter = 0;
};

class CBarAttachment : public Aquamarine::IAttachment {
  public:
    int counter = 0;
};

int main() {
    Aquamarine::CAttachmentManager attachments;
    int                            ret = 0;
    EXPECT(attachments.has<CFooAttachment>(), false);
    EXPECT(attachments.get<CFooAttachment>(), nullptr);
    EXPECT(attachments.has<CBarAttachment>(), false);
    EXPECT(attachments.get<CBarAttachment>(), nullptr);

    auto foo = Hyprutils::Memory::makeShared<CFooAttachment>();
    attachments.add(foo);
    EXPECT(attachments.has<CFooAttachment>(), true);
    EXPECT(attachments.has<CBarAttachment>(), false);
    foo->counter++;
    EXPECT(attachments.get<CFooAttachment>(), foo);
    EXPECT(attachments.get<CFooAttachment>()->counter, 1);

    attachments.add(Hyprutils::Memory::makeShared<CBarAttachment>());
    EXPECT(attachments.get<CFooAttachment>()->counter, 1);
    EXPECT(attachments.get<CBarAttachment>()->counter, 0);

    Hyprutils::Memory::CWeakPointer<CBarAttachment> bar = attachments.get<CBarAttachment>();
    EXPECT(bar.valid(), true);
    bar->counter = 5;

    // test overriding an attachment
    attachments.add(Hyprutils::Memory::makeShared<CBarAttachment>());
    Hyprutils::Memory::CWeakPointer<CBarAttachment> newBar = attachments.get<CBarAttachment>();
    EXPECT(bar == newBar, false);
    EXPECT(attachments.get<CBarAttachment>()->counter, 0);

    // should be a noop as this is a different attachment
    attachments.remove(Hyprutils::Memory::makeShared<CFooAttachment>());
    EXPECT(attachments.has<CFooAttachment>(), true);
    EXPECT(attachments.has<CBarAttachment>(), true);

    attachments.remove(foo);
    EXPECT(attachments.has<CFooAttachment>(), false);
    EXPECT(attachments.has<CBarAttachment>(), true);

    attachments.removeByType<CBarAttachment>();
    EXPECT(attachments.has<CFooAttachment>(), false);
    EXPECT(attachments.has<CBarAttachment>(), false);

    EXPECT(foo.strongRef(), 1);
    EXPECT(bar.valid(), false);
    EXPECT(newBar.valid(), false);

    return ret;
}
