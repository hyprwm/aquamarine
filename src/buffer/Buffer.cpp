#include <aquamarine/buffer/Buffer.hpp>
#include "Shared.hpp"

using namespace Aquamarine;

SDMABUFAttrs Aquamarine::IBuffer::dmabuf() {
    return SDMABUFAttrs{};
}

SSHMAttrs Aquamarine::IBuffer::shm() {
    return SSHMAttrs{};
}

std::tuple<uint8_t*, uint32_t, size_t> Aquamarine::IBuffer::beginDataPtr(uint32_t flags) {
    return {nullptr, 0, 0};
}

void Aquamarine::IBuffer::endDataPtr() {
    ; // empty
}

bool Aquamarine::IBuffer::getOpaque() {
    return opaque;
}

Hyprutils::Math::Vector2D& Aquamarine::IBuffer::getSize() {
    return size;
}

CAttachmentManager& Aquamarine::IBuffer::getAttachments() {
    return attachments;
}

Hyprutils::Signal::CSignal& Aquamarine::IBuffer::getDestroyEvent() {
    return events.destroy;
}