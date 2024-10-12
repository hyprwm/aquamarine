#include <aquamarine/output/Output.hpp>
#include "Shared.hpp"

#include <bit>
#include <cerrno>
#include <fcntl.h>

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

uint32_t Aquamarine::IOutput::commitCapabilities() const {
    return 0;
}

Aquamarine::IOutput::SCommitSubmission Aquamarine::IOutput::commitAsync(const SCommitOptions& options) {
    return {
        .error = ENOTSUP,
    };
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

bool Aquamarine::IOutput::hasCursorPlane() const {
    return false;
}

void Aquamarine::IOutput::scheduleFrame(const scheduleFrameReason reason) {
    ;
}

Hyprutils::Math::Vector2D Aquamarine::IOutput::cursorPlaneSize() {
    return {}; // error
}

std::optional<std::chrono::steady_clock::time_point> Aquamarine::IOutput::nextVBlank() const {
    return std::nullopt;
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

Aquamarine::COutputState::CSnapshot::CSnapshot(const COutputState* owner, const SInternalState& state, const std::array<uint64_t, 16>& generations) :
    m_owner(owner), m_state(state), m_generations(generations) {
    if (!(m_state.committed & AQ_OUTPUT_STATE_EXPLICIT_IN_FENCE) || m_state.explicitInFence < 0)
        return;

    const int DUPLICATED_FD = fcntl(m_state.explicitInFence, F_DUPFD_CLOEXEC, 0);
    if (DUPLICATED_FD < 0) {
        m_error                 = errno;
        m_state.explicitInFence = -1;
        return;
    }

    m_explicitInFence       = Hyprutils::OS::CFileDescriptor{DUPLICATED_FD};
    m_state.explicitInFence = m_explicitInFence.get();
}

const Aquamarine::COutputState::SInternalState& Aquamarine::COutputState::CSnapshot::state() const {
    return m_state;
}

bool Aquamarine::COutputState::CSnapshot::needsReconfig() const {
    return m_state.committed & (AQ_OUTPUT_STATE_ENABLED | AQ_OUTPUT_STATE_FORMAT | AQ_OUTPUT_STATE_MODE | AQ_OUTPUT_STATE_HDR | AQ_OUTPUT_STATE_WCG);
}

int Aquamarine::COutputState::CSnapshot::error() const {
    return m_error;
}

std::vector<Aquamarine::IOutput::SPlaneData> Aquamarine::IOutput::getPlanes() {
    return {{.renderFormats = {}, .type = AQ_PLANE_PRIMARY}};
}

const Aquamarine::COutputState::SInternalState& Aquamarine::COutputState::state() {
    return internalState;
}

const Aquamarine::COutputState::SInternalState& Aquamarine::COutputState::state() const {
    return internalState;
}

Aquamarine::COutputState::CSnapshot Aquamarine::COutputState::snapshot() const {
    return CSnapshot{this, internalState, propertyGenerations};
}

void Aquamarine::COutputState::consume(const CSnapshot& snapshot) {
    ASSERT(snapshot.m_owner == this);
    if (snapshot.m_owner != this)
        return;

    uint32_t properties = snapshot.m_state.committed;

    while (properties) {
        const uint32_t property = 1U << std::countr_zero(properties);
        const size_t   index    = std::countr_zero(property);
        properties &= ~property;

        if (!(internalState.committed & property) || propertyGenerations.at(index) != snapshot.m_generations.at(index))
            continue;

        internalState.committed &= ~property;
        if (property == AQ_OUTPUT_STATE_DAMAGE)
            internalState.damage.clear();
    }
}

void Aquamarine::COutputState::rearm(const CSnapshot& snapshot) {
    ASSERT(snapshot.m_owner == this);
    if (snapshot.m_owner != this)
        return;

    uint32_t properties = snapshot.m_state.committed & ~(AQ_OUTPUT_STATE_EXPLICIT_IN_FENCE | AQ_OUTPUT_STATE_EXPLICIT_OUT_FENCE);
    while (properties) {
        const uint32_t property = 1U << std::countr_zero(properties);
        const size_t   index    = std::countr_zero(property);
        properties &= ~property;

        if (propertyGenerations.at(index) != snapshot.m_generations.at(index))
            continue;

        internalState.committed |= property;
        if (property == AQ_OUTPUT_STATE_DAMAGE)
            internalState.damage.add(snapshot.m_state.damage);
    }
}

bool Aquamarine::COutputState::needsReconfig() const {
    return internalState.committed & (AQ_OUTPUT_STATE_ENABLED | AQ_OUTPUT_STATE_FORMAT | AQ_OUTPUT_STATE_MODE | AQ_OUTPUT_STATE_HDR | AQ_OUTPUT_STATE_WCG);
}

void Aquamarine::COutputState::addDamage(const Hyprutils::Math::CRegion& region) {
    internalState.damage.add(region);
    markCommitted(AQ_OUTPUT_STATE_DAMAGE);
}

void Aquamarine::COutputState::clearDamage() {
    internalState.damage.clear();
    markCommitted(AQ_OUTPUT_STATE_DAMAGE);
}

void Aquamarine::COutputState::setEnabled(bool enabled) {
    internalState.enabled = enabled;
    markCommitted(AQ_OUTPUT_STATE_ENABLED);
}

void Aquamarine::COutputState::setAdaptiveSync(bool enabled) {
    internalState.adaptiveSync = enabled;
    markCommitted(AQ_OUTPUT_STATE_ADAPTIVE_SYNC);
}

void Aquamarine::COutputState::setPresentationMode(eOutputPresentationMode mode) {
    internalState.presentationMode = mode;
    markCommitted(AQ_OUTPUT_STATE_PRESENTATION_MODE);
}

void Aquamarine::COutputState::setGammaLut(const std::vector<uint16_t>& lut) {
    internalState.gammaLut = lut;
    markCommitted(AQ_OUTPUT_STATE_GAMMA_LUT);
}

void Aquamarine::COutputState::setDeGammaLut(const std::vector<uint16_t>& lut) {
    internalState.degammaLut = lut;
    markCommitted(AQ_OUTPUT_STATE_DEGAMMA_LUT);
}

void Aquamarine::COutputState::setMode(Hyprutils::Memory::CSharedPointer<SOutputMode> mode) {
    internalState.mode       = mode;
    internalState.customMode = nullptr;
    markCommitted(AQ_OUTPUT_STATE_MODE);
}

void Aquamarine::COutputState::setCustomMode(Hyprutils::Memory::CSharedPointer<SOutputMode> mode) {
    internalState.mode.reset();
    internalState.customMode = mode;
    markCommitted(AQ_OUTPUT_STATE_MODE);
}

void Aquamarine::COutputState::setFormat(uint32_t drmFormat) {
    internalState.drmFormat = drmFormat;
    markCommitted(AQ_OUTPUT_STATE_FORMAT);
}

void Aquamarine::COutputState::setBuffer(Hyprutils::Memory::CSharedPointer<IBuffer> buffer) {
    internalState.buffer = buffer;
    markCommitted(AQ_OUTPUT_STATE_BUFFER);
}

void Aquamarine::COutputState::setExplicitInFence(int32_t fenceFD) {
    internalState.explicitInFence = fenceFD;
    markCommitted(AQ_OUTPUT_STATE_EXPLICIT_IN_FENCE);
}

void Aquamarine::COutputState::enableExplicitOutFenceForNextCommit() {
    markCommitted(AQ_OUTPUT_STATE_EXPLICIT_OUT_FENCE);
}

void Aquamarine::COutputState::resetExplicitFences() {
    // fences are now used, let's reset them to not confuse ourselves later.
    internalState.explicitInFence  = -1;
    internalState.explicitOutFence = -1;
}

void Aquamarine::COutputState::setCTM(const Hyprutils::Math::Mat3x3& ctm) {
    internalState.ctm = ctm;
    markCommitted(AQ_OUTPUT_STATE_CTM);
}

void Aquamarine::COutputState::setWideColorGamut(bool wcg) {
    internalState.wideColorGamut = wcg;
    markCommitted(AQ_OUTPUT_STATE_WCG);
}

void Aquamarine::COutputState::setHDRMetadata(const hdr_output_metadata& metadata) {
    internalState.hdrMetadata = metadata;
    markCommitted(AQ_OUTPUT_STATE_HDR);
}

void Aquamarine::COutputState::setContentType(const uint16_t drmContentType) {
    internalState.contentType = drmContentType;
}

void Aquamarine::COutputState::setColorRange(eOutputColorRange range) {
    internalState.colorRange = range;
}

void Aquamarine::COutputState::markCommitted(uint32_t properties) {
    internalState.committed |= properties;

    while (properties) {
        const uint32_t property = 1U << std::countr_zero(properties);
        properties &= ~property;
        propertyGenerations.at(std::countr_zero(property)) = ++nextGeneration;
    }
}

void Aquamarine::COutputState::setPlaneEnabled(uint32_t planeIdx, bool enabled) {
    if (planeIdx >= internalState.planeStates.size())
        return;

    internalState.planeStates.at(planeIdx).enabled = enabled;
    internalState.planeStates.at(planeIdx).updated = true;

    internalState.committed |= AQ_OUTPUT_STATE_PLANE_STATE;
}

void Aquamarine::COutputState::setPlaneBuffer(uint32_t planeIdx, Hyprutils::Memory::CSharedPointer<IBuffer> buffer) {
    if (planeIdx >= internalState.planeStates.size())
        return;

    internalState.planeStates.at(planeIdx).buffer  = buffer;
    internalState.planeStates.at(planeIdx).updated = true;

    internalState.committed |= AQ_OUTPUT_STATE_PLANE_STATE;
}

void Aquamarine::COutputState::setPlaneGeometry(uint32_t planeIdx, const Hyprutils::Math::CBox& box) {
    if (planeIdx >= internalState.planeStates.size())
        return;

    internalState.planeStates.at(planeIdx).geometry = box;
    internalState.planeStates.at(planeIdx).updated  = true;

    internalState.committed |= AQ_OUTPUT_STATE_PLANE_STATE;
}

void Aquamarine::COutputState::addPlaneDamage(uint32_t planeIdx, const Hyprutils::Math::CRegion& region) {
    if (planeIdx >= internalState.planeStates.size())
        return;

    internalState.planeStates.at(planeIdx).damage.add(region);
    internalState.planeStates.at(planeIdx).updated = true;

    internalState.committed |= AQ_OUTPUT_STATE_PLANE_STATE;
}

void Aquamarine::COutputState::onCommit() {
    internalState.committed = 0;
    internalState.damage.clear();

    for (auto& p : internalState.planeStates) {
        p.damage.clear();
        p.updated = false;
    }
}
