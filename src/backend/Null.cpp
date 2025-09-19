#include <aquamarine/backend/Null.hpp>
#include <fcntl.h>
#include <ctime>
#include <sys/timerfd.h>
#include <cstring>
#include "Shared.hpp"

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
#define SP CSharedPointer

Aquamarine::CNullBackend::~CNullBackend() {
    ;
}

Aquamarine::CNullBackend::CNullBackend(SP<CBackend> backend_) : backend(backend_) {
    ;
}

eBackendType Aquamarine::CNullBackend::type() {
    return eBackendType::AQ_BACKEND_NULL;
}

bool Aquamarine::CNullBackend::start() {
    return true;
}

std::vector<SP<SPollFD>> Aquamarine::CNullBackend::pollFDs() {
    return {};
}

int Aquamarine::CNullBackend::drmFD() {
    return -1;
}

int Aquamarine::CNullBackend::drmRenderNodeFD() {
    return -1;
}

bool Aquamarine::CNullBackend::dispatchEvents() {
    return true;
}

uint32_t Aquamarine::CNullBackend::capabilities() {
    return 0;
}

bool Aquamarine::CNullBackend::setCursor(SP<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot) {
    return false;
}

void Aquamarine::CNullBackend::onReady() {
    ;
}

std::vector<SDRMFormat> Aquamarine::CNullBackend::getRenderFormats() {
    for (const auto& impl : backend->getImplementations()) {
        if (impl->type() != AQ_BACKEND_DRM || impl->getRenderableFormats().empty())
            continue;
        return impl->getRenderableFormats();
    }

    return m_formats;
}

void Aquamarine::CNullBackend::setFormats(const std::vector<SDRMFormat>& fmts) {
    m_formats = fmts;
}

std::vector<SDRMFormat> Aquamarine::CNullBackend::getCursorFormats() {
    return {}; // No cursor support
}

bool Aquamarine::CNullBackend::createOutput(const std::string& name) {
    return false;
}

SP<IAllocator> Aquamarine::CNullBackend::preferredAllocator() {
    return backend->primaryAllocator;
}

std::vector<SP<IAllocator>> Aquamarine::CNullBackend::getAllocators() {
    return {backend->primaryAllocator};
}

Hyprutils::Memory::CWeakPointer<IBackendImplementation> Aquamarine::CNullBackend::getPrimary() {
    return {};
}
