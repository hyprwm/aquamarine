#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/output/Output.hpp>
#include <aquamarine/input/Input.hpp>
#include <iostream>
#include <wayland-server.h>

using namespace Hyprutils::Signal;
using namespace Hyprutils::Memory;
#define SP CSharedPointer

static const char* aqLevelToString(Aquamarine::eBackendLogLevel level) {
    switch (level) {
        case Aquamarine::eBackendLogLevel::AQ_LOG_TRACE: return "TRACE";
        case Aquamarine::eBackendLogLevel::AQ_LOG_DEBUG: return "DEBUG";
        case Aquamarine::eBackendLogLevel::AQ_LOG_ERROR: return "ERROR";
        case Aquamarine::eBackendLogLevel::AQ_LOG_WARNING: return "WARNING";
        case Aquamarine::eBackendLogLevel::AQ_LOG_CRITICAL: return "CRITICAL";
        default: break;
    }

    return "UNKNOWN";
}

void aqLog(Aquamarine::eBackendLogLevel level, std::string msg) {
    std::cout << "[AQ] [" << aqLevelToString(level) << "] " << msg << "\n";
}

CHyprSignalListener     newOutputListener, outputFrameListener, outputStateListener, mouseMotionListener, keyboardKeyListener, newMouseListener, newKeyboardListener;
SP<Aquamarine::IOutput> output;

//
void onFrame() {
    std::cout << "[Client] onFrame\n";

    auto buf = output->swapchain->next(nullptr);

    output->state->setBuffer(buf);
    output->commit();
}

void onState(const Aquamarine::IOutput::SStateEvent& event) {
    std::cout << "[Client] onState with size " << std::format("{}", event.size) << "\n";

    output->state->setEnabled(true);
    output->state->setCustomMode(makeShared<Aquamarine::SOutputMode>(Aquamarine::SOutputMode{.pixelSize = event.size}));
    output->state->setFormat(DRM_FORMAT_XRGB8888);

    output->commit();
}

int main(int argc, char** argv, char** envp) {
    Aquamarine::SBackendOptions options;
    options.logFunction = aqLog;

    std::vector<Aquamarine::SBackendImplementationOptions> implementations;
    Aquamarine::SBackendImplementationOptions              waylandOptions;
    waylandOptions.backendType        = Aquamarine::eBackendType::AQ_BACKEND_WAYLAND;
    waylandOptions.backendRequestMode = Aquamarine::eBackendRequestMode::AQ_BACKEND_REQUEST_IF_AVAILABLE;
    implementations.emplace_back(waylandOptions);

    auto aqBackend = Aquamarine::CBackend::create(implementations, options);

    newOutputListener = aqBackend->events.newOutput.listen([](const SP<Aquamarine::IOutput> newOutput) {
        output = newOutput;

        std::cout << "[Client] Got a new output named " << output->name << "\n";

        outputFrameListener = output->events.frame.listen([] { onFrame(); });
        outputStateListener = output->events.state.listen([](const Aquamarine::IOutput::SStateEvent& event) { onState(event); });
    });

    newMouseListener = aqBackend->events.newPointer.listen([](const SP<Aquamarine::IPointer>& pointer) {
        mouseMotionListener = pointer->events.warp.listen([](const Aquamarine::IPointer::SWarpEvent& event) {
            std::cout << "[Client] Mouse warped to " << std::format("{}", event.absolute) << "\n";
        });
    });

    newKeyboardListener = aqBackend->events.newKeyboard.listen([](const SP<Aquamarine::IKeyboard>& keyboard) {
        keyboardKeyListener = keyboard->events.key.listen([](const Aquamarine::IKeyboard::SKeyEvent& event) {
            std::cout << "[Client] Key " << std::format("{}", event.key) << " state: " << event.pressed << " \n";
        });
    });

    if (!aqBackend || !aqBackend->start()) {
        std::cout << "Failed to start the aq backend\n";
        return 1;
    }

    // FIXME: write an event loop.
    // aqBackend->enterLoop();

    return 0;
}
