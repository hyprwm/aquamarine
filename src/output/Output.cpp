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

void Aquamarine::IOutput::setCursorVisible(bool visible) {
    ;
}

void Aquamarine::IOutput::scheduleFrame() {
    ;
}

Hyprutils::Math::Vector2D Aquamarine::IOutput::cursorPlaneSize() {
    return {}; // error
}

size_t Aquamarine::IOutput::getGammaSize() {
    return 0;
}

bool Aquamarine::IOutput::destroy() {
    return false;
}

const Aquamarine::COutputState::SInternalState& Aquamarine::COutputState::state() {
    return internalState;
}

void Aquamarine::COutputState::addDamage(const Hyprutils::Math::CRegion& region) {
    internalState.damage.add(region);
    internalState.committed |= AQ_OUTPUT_STATE_DAMAGE;
}

void Aquamarine::COutputState::clearDamage() {
    internalState.damage.clear();
    internalState.committed |= AQ_OUTPUT_STATE_DAMAGE;
}

void Aquamarine::COutputState::setEnabled(bool enabled) {
    internalState.enabled = enabled;
    internalState.committed |= AQ_OUTPUT_STATE_ENABLED;
}

void Aquamarine::COutputState::setAdaptiveSync(bool enabled) {
    internalState.adaptiveSync = enabled;
    internalState.committed |= AQ_OUTPUT_STATE_ADAPTIVE_SYNC;
}

void Aquamarine::COutputState::setPresentationMode(eOutputPresentationMode mode) {
    internalState.presentationMode = mode;
    internalState.committed |= AQ_OUTPUT_STATE_PRESENTATION_MODE;
}

void Aquamarine::COutputState::setGammaLut(const std::vector<uint16_t>& lut) {
    internalState.gammaLut = lut;
    internalState.committed |= AQ_OUTPUT_STATE_GAMMA_LUT;
}

void Aquamarine::COutputState::setMode(Hyprutils::Memory::CSharedPointer<SOutputMode> mode) {
    internalState.mode       = mode;
    internalState.customMode = nullptr;
    internalState.committed |= AQ_OUTPUT_STATE_MODE;
}

void Aquamarine::COutputState::setCustomMode(Hyprutils::Memory::CSharedPointer<SOutputMode> mode) {
    internalState.mode.reset();
    internalState.customMode = mode;
    internalState.committed |= AQ_OUTPUT_STATE_MODE;
}

void Aquamarine::COutputState::setFormat(uint32_t drmFormat) {
    internalState.drmFormat = drmFormat;
    internalState.committed |= AQ_OUTPUT_STATE_FORMAT;
}

void Aquamarine::COutputState::setBuffer(Hyprutils::Memory::CSharedPointer<IBuffer> buffer) {
    internalState.buffer = buffer;
    internalState.committed |= AQ_OUTPUT_STATE_BUFFER;
}

void Aquamarine::COutputState::setExplicitInFence(int64_t fenceFD) {
    internalState.explicitInFence = fenceFD;
    internalState.committed |= AQ_OUTPUT_STATE_EXPLICIT_IN_FENCE;
}

void Aquamarine::COutputState::setExplicitOutFence(int64_t fenceFD) {
    internalState.explicitOutFence = fenceFD;
    internalState.committed |= AQ_OUTPUT_STATE_EXPLICIT_OUT_FENCE;
}

void Aquamarine::COutputState::onCommit() {
    internalState.committed = 0;
    internalState.damage.clear();

    // fences are now used, let's reset them to not confuse ourselves later.
    internalState.explicitInFence  = -1;
    internalState.explicitOutFence = -1;
}
