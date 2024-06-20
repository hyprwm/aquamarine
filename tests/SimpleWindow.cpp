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

    output->state->buffer = buf;
    output->commit();
}

void onState(const Aquamarine::IOutput::SStateEvent& event) {
    std::cout << "[Client] onState with size " << std::format("{}", event.size) << "\n";

    output->state->enabled = true;
    output->state->customMode = makeShared<Aquamarine::SOutputMode>(Aquamarine::SOutputMode{.pixelSize = event.size});
    output->state->drmFormat = DRM_FORMAT_XRGB8888;

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

    newOutputListener = aqBackend->events.newOutput.registerListener([](std::any data) {
        output = std::any_cast<SP<Aquamarine::IOutput>>(data);

        std::cout << "[Client] Got a new output named " << output->name << "\n";

        outputFrameListener = output->events.frame.registerListener([](std::any data) { onFrame(); });
        outputStateListener = output->events.state.registerListener([](std::any data) { onState(std::any_cast<Aquamarine::IOutput::SStateEvent>(data)); });
    });

    newMouseListener = aqBackend->events.newPointer.registerListener([] (std::any pointer) {
        auto p = std::any_cast<SP<Aquamarine::IPointer>>(pointer);
        mouseMotionListener = p->events.warp.registerListener([] (std::any data) {
            auto e = std::any_cast<Aquamarine::IPointer::SWarpEvent>(data);
            std::cout << "[Client] Mouse warped to " << std::format("{}", e.absolute) << "\n";
        });
    });

    newKeyboardListener = aqBackend->events.newKeyboard.registerListener([] (std::any keeb) {
        auto k = std::any_cast<SP<Aquamarine::IKeyboard>>(keeb);
        keyboardKeyListener = k->events.key.registerListener([] (std::any data) {
            auto e = std::any_cast<Aquamarine::IKeyboard::SKeyEvent>(data);
            std::cout << "[Client] Key " << std::format("{}", e.key) << " state: " << e.pressed << " \n";
        });
    });

    if (!aqBackend || !aqBackend->start()) {
        std::cout << "Failed to start the aq backend\n";
        return 1;
    }

    aqBackend->enterLoop();

    return 0;
}