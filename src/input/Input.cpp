#include <aquamarine/input/Input.hpp>

libinput_device* Aquamarine::IPointer::getLibinputHandle() {
    return nullptr;
}

libinput_device* Aquamarine::IKeyboard::getLibinputHandle() {
    return nullptr;
}

libinput_device* Aquamarine::ITouch::getLibinputHandle() {
    return nullptr;
}

libinput_device* Aquamarine::ISwitch::getLibinputHandle() {
    return nullptr;
}

libinput_device* Aquamarine::ITabletTool::getLibinputHandle() {
    return nullptr;
}

libinput_device* Aquamarine::ITablet::getLibinputHandle() {
    return nullptr;
}

libinput_device* Aquamarine::ITabletPad::getLibinputHandle() {
    return nullptr;
}

void Aquamarine::IKeyboard::updateLEDs(uint32_t leds) {
    ;
}
