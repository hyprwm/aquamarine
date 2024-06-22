#include <aquamarine/output/Output.hpp>

using namespace Aquamarine;

Hyprutils::Memory::CSharedPointer<SOutputMode> Aquamarine::IOutput::preferredMode() {
    for (auto& m : modes) {
        if (m->preferred)
            return m;
    }

    return nullptr;
}

void Aquamarine::IOutput::moveCursor(const Hyprutils::Math::Vector2D& coord) {
    ;
}

bool Aquamarine::IOutput::setCursor(Hyprutils::Memory::CSharedPointer<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot) {
    return false;
}

void Aquamarine::IOutput::scheduleFrame() {
    ;
}

Hyprutils::Math::Vector2D Aquamarine::IOutput::maxCursorSize() {
    return {}; // error
}
