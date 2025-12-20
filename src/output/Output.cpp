#include <aquamarine/output/Output.hpp>

using namespace Aquamarine;

Aquamarine::IOutput::~IOutput() {
    events.destroy.emit();
}

Hyprutils::Memory::CSharedPointer<SOutputMode> Aquamarine::IOutput::preferredMode() {
    for (auto const& m : modes) {
        if (m->preferred)
            return m;
    }

    return nullptr;
}

void Aquamarine::IOutput::moveCursor(const Hyprutils::Math::Vector2D& coord, bool skipSchedule) {
    ;
}

bool Aquamarine::IOutput::setCursor(Hyprutils::Memory::CSharedPointer<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot) {
    return false;
}

void Aquamarine::IOutput::setCursorVisible(bool visible) {
    ;
}

void Aquamarine::IOutput::scheduleFrame(const scheduleFrameReason reason) {
    ;
}

Hyprutils::Math::Vector2D Aquamarine::IOutput::cursorPlaneSize() {
    return {}; // error
}

size_t Aquamarine::IOutput::getGammaSize() {
    return 0;
}

size_t Aquamarine::IOutput::getDeGammaSize() {
    return 0;
}

bool Aquamarine::IOutput::destroy() {
    return false;
}

const Aquamarine::COutputState::SInternalState& Aquamarine::COutputState::state() {
    return internalState;
}

void Aquamarine::COutputState::addDamage(const Hyprutils::Math::CRegion& region) {
    if (region.empty())
        return;

    internalState.damage.add(region);
    internalState.committed |= AQ_OUTPUT_STATE_DAMAGE;
}

void Aquamarine::COutputState::clearDamage() {
    if (internalState.damage.empty())
        return;

    internalState.damage.clear();
    internalState.committed |= AQ_OUTPUT_STATE_DAMAGE;
}

void Aquamarine::COutputState::setEnabled(bool enabled) {
    if (internalState.enabled == enabled)
        return;

    internalState.enabled = enabled;
    internalState.committed |= AQ_OUTPUT_STATE_ENABLED;
}

void Aquamarine::COutputState::setAdaptiveSync(bool enabled) {
    if (internalState.adaptiveSync == enabled)
        return;

    internalState.adaptiveSync = enabled;
    internalState.committed |= AQ_OUTPUT_STATE_ADAPTIVE_SYNC;
}

void Aquamarine::COutputState::setPresentationMode(eOutputPresentationMode mode) {
    if (internalState.presentationMode == mode)
        return;

    internalState.presentationMode = mode;
    internalState.committed |= AQ_OUTPUT_STATE_PRESENTATION_MODE;
}

void Aquamarine::COutputState::setGammaLut(const std::vector<uint16_t>& lut) {
    if (internalState.gammaLut == lut)
        return;

    internalState.gammaLut = lut;
    internalState.committed |= AQ_OUTPUT_STATE_GAMMA_LUT;
}

void Aquamarine::COutputState::setDeGammaLut(const std::vector<uint16_t>& lut) {
    if (internalState.degammaLut == lut)
        return;

    internalState.degammaLut = lut;
    internalState.committed |= AQ_OUTPUT_STATE_DEGAMMA_LUT;
}

void Aquamarine::COutputState::setMode(Hyprutils::Memory::CSharedPointer<SOutputMode> mode) {
    if (internalState.mode == mode && !internalState.customMode)
        return;

    internalState.mode       = mode;
    internalState.customMode = nullptr;
    internalState.committed |= AQ_OUTPUT_STATE_MODE;
}

void Aquamarine::COutputState::setCustomMode(Hyprutils::Memory::CSharedPointer<SOutputMode> mode) {
    if (internalState.customMode == mode && !internalState.mode)
        return;

    internalState.mode.reset();
    internalState.customMode = mode;
    internalState.committed |= AQ_OUTPUT_STATE_MODE;
}

void Aquamarine::COutputState::setFormat(uint32_t drmFormat) {
    if (internalState.drmFormat == drmFormat)
        return;

    internalState.drmFormat = drmFormat;
    internalState.committed |= AQ_OUTPUT_STATE_FORMAT;
}

void Aquamarine::COutputState::setBuffer(Hyprutils::Memory::CSharedPointer<IBuffer> buffer) {
    if (internalState.buffer == buffer)
        return;

    internalState.buffer = buffer;
    internalState.committed |= AQ_OUTPUT_STATE_BUFFER;
}

void Aquamarine::COutputState::setExplicitInFence(int32_t fenceFD) {
    if (fenceFD < 0)
        return;

    internalState.explicitInFence = fenceFD;
    internalState.committed |= AQ_OUTPUT_STATE_EXPLICIT_IN_FENCE;
}

void Aquamarine::COutputState::enableExplicitOutFenceForNextCommit() {
    internalState.committed |= AQ_OUTPUT_STATE_EXPLICIT_OUT_FENCE;
}

void Aquamarine::COutputState::resetExplicitFences() {
    // fences are now used, let's reset them to not confuse ourselves later.
    internalState.explicitInFence  = -1;
    internalState.explicitOutFence = -1;
}

void Aquamarine::COutputState::setCTM(const Hyprutils::Math::Mat3x3& ctm) {
    if (internalState.ctm == ctm)
        return;

    internalState.ctm = ctm;
    internalState.committed |= AQ_OUTPUT_STATE_CTM;
}

void Aquamarine::COutputState::setWideColorGamut(bool wcg) {
    if (internalState.wideColorGamut == wcg)
        return;

    internalState.wideColorGamut = wcg;
    internalState.committed |= AQ_OUTPUT_STATE_WCG;
}

void Aquamarine::COutputState::setHDRMetadata(const hdr_output_metadata& metadata) {
    internalState.hdrMetadata = metadata;
    internalState.committed |= AQ_OUTPUT_STATE_HDR;
}

void Aquamarine::COutputState::setContentType(const uint16_t drmContentType) {
    if (internalState.contentType == drmContentType)
        return;

    internalState.contentType = drmContentType;
    internalState.committed |= AQ_OUTPUT_CONTENT_TYPE;
}

void Aquamarine::COutputState::onCommit() {
    internalState.committed = 0;
    internalState.damage.clear();
}
