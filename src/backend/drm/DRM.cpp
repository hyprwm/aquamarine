#include "aquamarine/output/Output.hpp"
#include <algorithm>
#include <aquamarine/backend/DRM.hpp>
#include <aquamarine/backend/drm/Legacy.hpp>
#include <aquamarine/backend/drm/Atomic.hpp>
#include <aquamarine/allocator/GBM.hpp>
#include <aquamarine/allocator/DRMDumb.hpp>
#include <cstdint>
#include <format>
#include <hyprutils/math/Mat3x3.hpp>
#include <hyprutils/memory/Atomic.hpp>
#include <hyprutils/string/VarList.hpp>
#include <chrono>
#include <thread>
#include <deque>
#include <unordered_map>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
extern "C" {
#include <libseat.h>
#include <libudev.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libdisplay-info/cta.h>
#include <libdisplay-info/cvt.h>
#include <libdisplay-info/info.h>
#include <libdisplay-info/edid.h>
}

#include "Props.hpp"
#include "FormatUtils.hpp"
#include "Shared.hpp"
#include "hwdata.hpp"
#include "Renderer.hpp"
#include "OutputTiming.hpp"
#include "AsyncCommit.hpp"

#include <hyprutils/utils/ScopeGuard.hpp>
using Hyprutils::Utils::CScopeGuard;

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
#define SP CSharedPointer

static bool shouldSubmitCTM(SP<SDRMConnector> connector, const COutputState::SInternalState& state, bool modeset) {
    if (state.committed & COutputState::AQ_OUTPUT_STATE_CTM)
        return true;

    if (!modeset)
        return false;

    if (!(state.ctm == Mat3x3::identity()))
        return true;

    return connector->crtc->props.values.ctm && !connector->crtc->atomic.ctmStateKnown;
}

Aquamarine::CDRMBackend::CDRMBackend(SP<CBackend> backend_) : backend(backend_) {
    listeners.sessionActivate = backend->session->events.changeActive.listen([this] {
        if (backend->session->active) {
            // session got activated, we need to restore
            restoreAfterVT();
        } else
            for (const auto& connector : connectors) {
                if (connector->output)
                    cancelAsyncOutput(connector->output.get(), true);
                connector->invalidateFrame();
            }
    });
}

static udev_enumerate* enumDRMCards(udev* udev) {
    auto enumerate = udev_enumerate_new(udev);
    if (!enumerate)
        return nullptr;

    udev_enumerate_add_match_subsystem(enumerate, "drm");
#ifdef __linux__
    // https://github.com/wulf7/libudev-devd/issues/11
    udev_enumerate_add_match_property(enumerate, "DEVTYPE", "drm_minor");
#endif
    udev_enumerate_add_match_sysname(enumerate, DRM_PRIMARY_MINOR_NAME "[0-9]*");

    if (udev_enumerate_scan_devices(enumerate)) {
        udev_enumerate_unref(enumerate);
        return nullptr;
    }

    return enumerate;
}

static int gpuNumBuiltinPanels(const SP<CSessionDevice> gpu) {
    auto resources = drmModeGetResources(gpu->fd);
    if (!resources)
        return 0;

    int num = 0;
    for (int i = 0; i < resources->count_connectors; ++i) {
        auto drmConn = drmModeGetConnector(gpu->fd, resources->connectors[i]);
        if (!drmConn)
            continue;

        if (drmConn->connection == DRM_MODE_CONNECTED &&
            (drmConn->connector_type == DRM_MODE_CONNECTOR_LVDS || drmConn->connector_type == DRM_MODE_CONNECTOR_eDP || drmConn->connector_type == DRM_MODE_CONNECTOR_DSI))
            num++;

        drmModeFreeConnector(drmConn);
    }

    drmModeFreeResources(resources);

    return num;
}

static std::vector<SP<CSessionDevice>> scanGPUs(SP<CBackend> backend) {
    auto enumerate = enumDRMCards(backend->session->udevHandle);

    if (!enumerate) {
        backend->log(AQ_LOG_ERROR, "drm: couldn't enumerate gpus with udev");
        return {};
    }

    if (!udev_enumerate_get_list_entry(enumerate)) {
        backend->log(AQ_LOG_ERROR, "drm: No gpus in scanGPUs.");
        udev_enumerate_unref(enumerate);
        return {};
    }

    udev_list_entry*               entry = nullptr;
    std::deque<SP<CSessionDevice>> devices;

    int                            maxBuiltinPanels = 0;
    SP<CSessionDevice>             maxBuiltinPanelsGPU;

    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerate)) {
        auto path   = udev_list_entry_get_name(entry);
        auto device = udev_device_new_from_syspath(backend->session->udevHandle, path);
        if (!device) {
            backend->log(AQ_LOG_WARNING, std::format("drm: Skipping device {}", path ? path : "unknown"));
            continue;
        }

        backend->log(AQ_LOG_DEBUG, std::format("drm: Enumerated device {}", path ? path : "unknown"));

        auto seat = udev_device_get_property_value(device, "ID_SEAT");
        if (!seat)
            seat = "seat0";

        if (!backend->session->seatName.empty() && backend->session->seatName != seat) {
            backend->log(AQ_LOG_WARNING, std::format("drm: Skipping device {} because seat {} doesn't match our {}", path ? path : "unknown", seat, backend->session->seatName));
            udev_device_unref(device);
            continue;
        }

        auto pciDevice = udev_device_get_parent_with_subsystem_devtype(device, "pci", nullptr);
        bool isBootVGA = false;
        if (pciDevice) {
            auto id   = udev_device_get_sysattr_value(pciDevice, "boot_vga");
            isBootVGA = id && id == std::string{"1"};
        }

        if (!udev_device_get_devnode(device)) {
            backend->log(AQ_LOG_ERROR, std::format("drm: Skipping device {}, no devnode", path ? path : "unknown"));
            udev_device_unref(device);
            continue;
        }

        auto sessionDevice = CSessionDevice::openIfKMS(backend->session, udev_device_get_devnode(device));
        if (!sessionDevice) {
            backend->log(AQ_LOG_ERROR, std::format("drm: Skipping device {}, not a KMS device", path ? path : "unknown"));
            udev_device_unref(device);
            continue;
        }

        sessionDevice->resolveMatchingRenderNode(device);

        udev_device_unref(device);

        if (isBootVGA)
            devices.push_front(sessionDevice);
        else
            devices.push_back(sessionDevice);

        int numBuiltinPanels = gpuNumBuiltinPanels(sessionDevice);
        backend->log(AQ_LOG_TRACE, std::format("drm: Device {} has {} builtin {}", sessionDevice->path, numBuiltinPanels, numBuiltinPanels == 1 ? "panel" : "panels"));
        if (numBuiltinPanels > maxBuiltinPanels) {
            maxBuiltinPanelsGPU = sessionDevice;
            maxBuiltinPanels    = numBuiltinPanels;
        }
    }

    udev_enumerate_unref(enumerate);

    std::vector<SP<CSessionDevice>> vecDevices;

    auto                            explicitGpus = getenv("AQ_DRM_DEVICES");
    if (explicitGpus) {
        backend->log(AQ_LOG_DEBUG, std::format("drm: Explicit device list {}", explicitGpus));
        Hyprutils::String::CVarList explicitDevices(explicitGpus, 0, ':', true);

        // Iterate over GPUs and canonicalize the paths
        for (auto& d : explicitDevices) {
            std::error_code ec;

            auto            canonicalFilePath = std::filesystem::canonical(d, ec);

            // If there is an error, log and continue.
            // TODO: Verify that the path is a valid DRM device. (https://gitlab.freedesktop.org/wlroots/wlroots/-/blob/master/backend/session/session.c?ref_type=heads#L369-387)
            if (ec) {
                backend->log(AQ_LOG_ERROR, std::format("drm: Failed to canonicalize path {}", d));
                continue;
            }

            d = canonicalFilePath.string();
        }

        for (auto const& d : explicitDevices) {
            bool found = false;
            for (auto const& vd : devices) {
                std::error_code ec;
                auto            canonicalFilePath = std::filesystem::canonical(vd->path, ec);
                if (ec) {
                    backend->log(AQ_LOG_ERROR, std::format("drm: Failed to canonicalize path {}", d));
                    canonicalFilePath = vd->path;
                }

                if (canonicalFilePath == d) {
                    vecDevices.emplace_back(vd);
                    found = true;
                    break;
                }
            }

            if (found)
                backend->log(AQ_LOG_DEBUG, std::format("drm: Explicit device {} found", d));
            else
                backend->log(AQ_LOG_ERROR, std::format("drm: Explicit device {} not found", d));
        }
    } else {
        if (maxBuiltinPanelsGPU && devices.front() != maxBuiltinPanelsGPU) {
            std::erase(devices, maxBuiltinPanelsGPU);
            devices.push_front(maxBuiltinPanelsGPU);
        }
        for (auto const& d : devices) {
            vecDevices.push_back(d);
        }
    }

    return vecDevices;
}

SP<CDRMBackend> Aquamarine::CDRMBackend::fromGpu(std::string path, SP<CBackend> backend, SP<CDRMBackend> primary) {
    auto gpu = CSessionDevice::openIfKMS(backend->session, path);
    if (!gpu) {
        return nullptr;
    }

    auto drmBackend  = SP<CDRMBackend>(new CDRMBackend(backend));
    drmBackend->self = drmBackend;

    if (!drmBackend->registerGPU(gpu, primary)) {
        backend->log(AQ_LOG_ERROR, std::format("drm: Failed to register gpu {}", gpu->path));
        return nullptr;
    } else
        backend->log(AQ_LOG_DEBUG, std::format("drm: Registered gpu {}", gpu->path));

    if (!drmBackend->checkFeatures()) {
        backend->log(AQ_LOG_ERROR, "drm: Failed checking features");
        return nullptr;
    }

    if (!drmBackend->initResources()) {
        backend->log(AQ_LOG_ERROR, "drm: Failed initializing resources");
        return nullptr;
    }

    backend->log(AQ_LOG_DEBUG, std::format("drm: Basic init pass for gpu {}", gpu->path));

    drmBackend->grabFormats();

    drmBackend->dumbAllocator = CDRMDumbAllocator::create(gpu->fd, backend);

    // so that session can handle udev change/remove events for this gpu
    backend->session->sessionDevices.push_back(gpu);

    return drmBackend;
}

std::vector<SP<CDRMBackend>> Aquamarine::CDRMBackend::attempt(SP<CBackend> backend) {
    if (!backend->session)
        backend->session = CSession::attempt(backend);

    if (!backend->session) {
        backend->log(AQ_LOG_ERROR, "Failed to open a session");
        return {};
    }

    if (!backend->session->active) {
        backend->log(AQ_LOG_DEBUG, "Session is not active, waiting for 5s");

        auto started = std::chrono::system_clock::now();

        while (!backend->session->active) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            backend->session->dispatchPendingEventsAsync();

            if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - started).count() >= 5000) {
                backend->log(AQ_LOG_DEBUG, "Session timeout reached");
                break;
            }
        }

        if (!backend->session->active) {
            backend->log(AQ_LOG_DEBUG, "Session could not be activated in time");
            return {};
        }
    }

    auto gpus = scanGPUs(backend);

    if (gpus.empty()) {
        backend->log(AQ_LOG_ERROR, "drm: Found no gpus to use, cannot continue");
        return {};
    }

    backend->log(AQ_LOG_DEBUG, std::format("drm: Found {} GPUs", gpus.size()));

    std::vector<SP<CDRMBackend>> backends;
    SP<CDRMBackend>              newPrimary;

    for (auto const& gpu : gpus) {
        auto drmBackend  = SP<CDRMBackend>(new CDRMBackend(backend));
        drmBackend->self = drmBackend;

        if (!drmBackend->registerGPU(gpu, newPrimary)) {
            backend->log(AQ_LOG_ERROR, std::format("drm: Failed to register gpu {}", gpu->path));
            continue;
        } else
            backend->log(AQ_LOG_DEBUG, std::format("drm: Registered gpu {}", gpu->path));

        // TODO: consider listening for new devices
        // But if you expect me to handle gpu hotswaps you are probably insane LOL

        if (!drmBackend->checkFeatures()) {
            backend->log(AQ_LOG_ERROR, "drm: Failed checking features");
            continue;
        }

        if (!drmBackend->initResources()) {
            backend->log(AQ_LOG_ERROR, "drm: Failed initializing resources");
            continue;
        }

        backend->log(AQ_LOG_DEBUG, std::format("drm: Basic init pass for gpu {}", gpu->path));

        drmBackend->grabFormats();

        drmBackend->recheckOutputs();

        if (!newPrimary) {
            backend->log(AQ_LOG_DEBUG, std::format("drm: gpu {} becomes primary drm", gpu->path));
            newPrimary = drmBackend;
        }

        drmBackend->dumbAllocator = CDRMDumbAllocator::create(gpu->fd, backend);

        backends.emplace_back(drmBackend);

        // so that session can handle udev change/remove events for this gpu
        backend->session->sessionDevices.push_back(gpu);
    }

    return backends;
}

Aquamarine::CDRMBackend::~CDRMBackend() {
    stopCommitThread();

    for (auto& conn : connectors) {
        conn->disconnect();
        conn.reset();
    }

    if (rendererState.allocator)
        rendererState.allocator->destroyBuffers();

    rendererState.renderer.reset();
    rendererState.allocator.reset();
}

bool Aquamarine::CDRMBackend::initCommitThread() {
    if (commitThread)
        return true;

    commitThread = CDRMCommitThread::create(makeDRMCommitSubmitter(gpu->fd));
    return !!commitThread;
}

void Aquamarine::CDRMBackend::stopCommitThread() {
    if (!commitThread)
        return;

    flushAsyncCommitEvents();
    auto RESULTS = commitThread->stopAndDrain();
    for (auto& result : RESULTS) {
        auto* DATA        = dynamic_cast<CDRMAsyncCommitData*>(result.request->data.get());
        auto* ATOMIC_IMPL = dynamic_cast<CDRMAtomicImpl*>(impl.get());
        if (!DATA || !DATA->request || !ATOMIC_IMPL)
            continue;

        const bool SUBMITTED = result.status == CDRMCommitThread::AQ_DRM_COMMIT_THREAD_SUBMITTED;
        if (SUBMITTED)
            DATA->mgpuAcquired = false;
        ATOMIC_IMPL->finalizeAsync(*DATA->request, DATA->connector, DATA->commitData, SUBMITTED);

        if (SUBMITTED && DATA->connector && DATA->output && DATA->connector->output == DATA->output) {
            DATA->output->state->internalState.drmFormat = DATA->commitData.outputState.drmFormat;
            DATA->connector->applyCommit(DATA->commitData);
            for (const auto& fb : DATA->commitData.retiredFBs)
                DATA->connector->releaseFBBuffer(fb);
        }

        if (!SUBMITTED)
            DATA->rollbackMgpu();
        DATA->releasePins();
        if (!SUBMITTED && DATA->output && DATA->snapshot)
            DATA->output->state->rearm(*DATA->snapshot);
        if (DATA->output && DATA->output->pendingAsyncCommit == result.request->id) {
            if (!SUBMITTED)
                DATA->output->pendingAsyncCommit = 0;
            DATA->output->events.commitResult.emit(IOutput::SCommitResult{
                .id           = result.request->id,
                .status       = SUBMITTED ? IOutput::AQ_OUTPUT_COMMIT_SUBMITTED :
                                            (result.status == CDRMCommitThread::AQ_DRM_COMMIT_THREAD_CANCELLED ? IOutput::AQ_OUTPUT_COMMIT_CANCELLED : IOutput::AQ_OUTPUT_COMMIT_FAILED),
                .error        = result.error,
                .missedTarget = result.missedTarget,
            });
        }
    }

    commitThread.reset();
}

uint64_t Aquamarine::CDRMBackend::nextAsyncOwnerID() {
    if (++m_lastAsyncOwnerID == 0)
        ++m_lastAsyncOwnerID;
    return m_lastAsyncOwnerID;
}

bool Aquamarine::CDRMBackend::pauseCommitQueue(uint64_t queueKey) {
    if (!commitThread)
        return true;

    if (!commitThread->pauseQueue(queueKey))
        return false;

    dispatchCommitResults();
    return true;
}

void Aquamarine::CDRMBackend::resumeCommitQueue(uint64_t queueKey) {
    if (commitThread)
        commitThread->resumeQueue(queueKey);
}

void Aquamarine::CDRMBackend::cancelAsyncOutput(CDRMOutput* output, bool renewOwner) {
    if (!output || !output->asyncOwnerID)
        return;

    flushAsyncCommitEvents();
    if (commitThread)
        commitThread->cancelOwner(output->asyncOwnerID);
    dispatchCommitResults();

    if (renewOwner)
        output->asyncOwnerID = nextAsyncOwnerID();
}

void Aquamarine::CDRMBackend::emitAsyncCommitEvent(SP<CDRMOutput> output) {
    if (!output || !output->asyncCommitEventPending)
        return;

    output->asyncCommitEventPending = false;
    if (m_pendingAsyncCommitEvents > 0)
        --m_pendingAsyncCommitEvents;
    output->events.commit.emit();

    if (m_pendingAsyncCommitEvents == 0)
        dispatchCommitResults();
}

void Aquamarine::CDRMBackend::flushAsyncCommitEvents() {
    for (const auto& connector : connectors) {
        if (connector->output && connector->output->asyncCommitEventPending)
            emitAsyncCommitEvent(connector->output);
    }
}

void Aquamarine::CDRMBackend::dispatchCommitResults() {
    if (!commitThread || m_pendingAsyncCommitEvents > 0)
        return;

    auto* atomicImpl = dynamic_cast<CDRMAtomicImpl*>(impl.get());
    auto  results    = commitThread->drainResults();
    for (auto& result : results) {
        auto* data = dynamic_cast<CDRMAsyncCommitData*>(result.request->data.get());
        if (!data || !data->request || !atomicImpl)
            continue;

        const bool SUBMITTED = result.status == CDRMCommitThread::AQ_DRM_COMMIT_THREAD_SUBMITTED;
        if (SUBMITTED)
            data->mgpuAcquired = false;
        atomicImpl->finalizeAsync(*data->request, data->connector, data->commitData, SUBMITTED);

        const auto OUTPUT    = data->output;
        const auto CONNECTOR = data->connector;
        const auto CRTC      = CONNECTOR ? CONNECTOR->crtc : nullptr;

        if (SUBMITTED && CONNECTOR && OUTPUT && CONNECTOR->output == OUTPUT) {
            OUTPUT->state->internalState.drmFormat = data->commitData.outputState.drmFormat;
            if (data->commitData.committed & COutputState::AQ_OUTPUT_STATE_EXPLICIT_OUT_FENCE)
                OUTPUT->state->internalState.explicitOutFence = data->commitData.outputState.explicitOutFence;
            CONNECTOR->applyCommit(data->commitData);
            for (const auto& fb : data->commitData.retiredFBs)
                CONNECTOR->releaseFBBuffer(fb);
        }

        if (!SUBMITTED)
            data->rollbackMgpu();
        data->releasePins();

        const bool CURRENT = OUTPUT && result.request->ownerID == OUTPUT->asyncOwnerID && OUTPUT->pendingAsyncCommit == result.request->id;
        if (!CURRENT) {
            if (!SUBMITTED && CRTC) {
                commitThread->releaseQueue(CRTC->id, result.request->id);
                if (CRTC->pendingFlip.commitID == result.request->id)
                    CRTC->disarmPageFlip();
            }
            continue;
        }

        IOutput::SCommitResult commitResult{
            .id           = result.request->id,
            .status       = SUBMITTED ? IOutput::AQ_OUTPUT_COMMIT_SUBMITTED :
                                        (result.status == CDRMCommitThread::AQ_DRM_COMMIT_THREAD_CANCELLED ? IOutput::AQ_OUTPUT_COMMIT_CANCELLED : IOutput::AQ_OUTPUT_COMMIT_FAILED),
            .error        = result.error,
            .missedTarget = result.missedTarget,
        };

        if (!SUBMITTED) {
            OUTPUT->pendingAsyncCommit = 0;
            if (data->snapshot)
                OUTPUT->state->rearm(*data->snapshot);
            if (CRTC) {
                commitThread->releaseQueue(CRTC->id, result.request->id);
                if (CRTC->pendingFlip.commitID == result.request->id)
                    CRTC->disarmPageFlip();
            }
            if (CONNECTOR)
                CONNECTOR->sched.onFrameComplete();
            OUTPUT->events.commitResult.emit(commitResult);
            OUTPUT->scheduleFrame(IOutput::AQ_SCHEDULE_NEEDS_FRAME);
            continue;
        }

        if (!CRTC || CRTC->pendingFlip.commitID != result.request->id) {
            OUTPUT->pendingAsyncCommit = 0;
            OUTPUT->events.commitResult.emit(commitResult);
            continue;
        }

        CRTC->pendingFlip.resultReady = true;
        const auto EARLY              = CRTC->pendingFlip.early;
        const auto FLIP_ID            = *CRTC->pendingFlip.id;
        OUTPUT->events.commitResult.emit(commitResult);

        if (EARLY.valid)
            handlePageFlip(FLIP_ID, EARLY.seq, EARLY.sec, EARLY.usec, CRTC->id);
    }
}

void Aquamarine::CDRMBackend::log(eBackendLogLevel l, const std::string& s) {
    backend->log(l, s);
}

bool Aquamarine::CDRMBackend::sessionActive() {
    return backend->session->active;
}

void Aquamarine::CDRMBackend::restoreAfterVT() {
    backend->log(AQ_LOG_DEBUG, "drm: Restoring after VT switch");

    // Clear stale page-flip bookkeeping for all connectors.
    // During S3 suspend the display hardware powers off, so any pending
    // page-flip completion events are lost. The handlePF() callback that
    // normally clears these flags will never fire. Without this reset,
    // commitState() rejects every frame with "Cannot commit when a
    // page-flip is awaiting" and scheduleFrame() returns early, leaving
    // outputs permanently black after resume.
    //
    // For VT switch this is also safe: pending events from the old session
    // are still queued in the fd buffer and will fire handlePF() after
    // restore, but isPageFlipPending is already false so the = false
    // assignment is a harmless no-op.
    for (auto const& c : connectors) {
        if (c->sched.frameInFlight() || c->sched.frameRunning()) {
            backend->log(AQ_LOG_DEBUG, std::format("drm: Clearing stale page-flip state for {}", c->szName));
            c->invalidateFrame();
        }
    }

    recheckOutputs();

    backend->log(AQ_LOG_DEBUG, "drm: Rescanned connectors");

    std::vector<SP<SDRMConnector>> noMode;

    for (auto const& c : connectors) {
        if (!c->crtc || !c->output)
            continue;

        auto&                   STATE = c->output->state->state();

        SDRMConnectorCommitData data = {
            .mainFB        = nullptr,
            .outputState   = STATE,
            .cursorPos     = c->output->cursorPos,
            .cursorHotspot = c->output->cursorHotspot,
            .cursorVisible = c->output->cursorVisible,
            .modeset       = true,
            .blocking      = true,
            .flags         = 0,
            .test          = false,
            .enabled       = STATE.enabled,
            .committed     = STATE.committed,
            .hdrMetadata   = STATE.hdrMetadata,
        };

        auto& MODE = STATE.customMode ? STATE.customMode : STATE.mode;

        if (!MODE) {
            backend->log(AQ_LOG_WARNING, "drm: Connector {} has output but state has no mode, will send a reset state event later.");
            noMode.emplace_back(c);
            continue;
        }

        c->crtc->atomic.ctmStateKnown = false;

        if (MODE->modeInfo.has_value())
            data.modeInfo = *MODE->modeInfo;
        else
            data.calculateMode(c);

        if (shouldSubmitCTM(c, STATE, data.modeset))
            data.ctm = STATE.ctm;

        if (STATE.buffer) {
            SP<CDRMFB> drmFB;

            // If this backend requires blit (multi-GPU), the STATE.buffer was
            // rendered on a different GPU and cannot be directly imported here.
            // Skip the import and request a re-render through the proper blit pipeline.
            if (!shouldBlit()) {
                auto buf   = STATE.buffer;
                bool isNew = false;
                drmFB      = CDRMFB::create(buf, self, &isNew);
            }

            if (!drmFB) {
                backend->log(AQ_LOG_DEBUG,
                             std::format("drm: Buffer unavailable for crtc {} restore ({}), requesting re-render", c->crtc->id, shouldBlit() ? "multi-gpu" : "import failed"));
                noMode.emplace_back(c);
                continue;
            }

            data.mainFB = drmFB;
        }

        if (c->crtc->pendingCursor)
            data.cursorFB = c->crtc->pendingCursor;
        else if (c->crtc->cursor)
            data.cursorFB = c->crtc->cursor->front; // a consumed pending cursor lives on the plane

        if (data.cursorFB && (data.cursorFB->dead || data.cursorFB->buffer->dmabuf().modifier == DRM_FORMAT_MOD_INVALID))
            data.cursorFB = nullptr;

        backend->log(AQ_LOG_DEBUG,
                     std::format("drm: Restoring crtc {} with clock {} hdisplay {} vdisplay {} vrefresh {}", c->crtc->id, data.modeInfo.clock, data.modeInfo.hdisplay,
                                 data.modeInfo.vdisplay, data.modeInfo.vrefresh));

        if (!impl->commit(c, data))
            backend->log(AQ_LOG_ERROR, std::format("drm: crtc {} failed restore", c->crtc->id));
    }

    for (auto const& c : noMode) {
        if (!c->output)
            continue;

        // tell the consumer to re-set a state because we had no mode
        c->output->events.state.emit(IOutput::SStateEvent{});
    }
}

bool Aquamarine::CDRMBackend::checkFeatures() {
    uint64_t curW = 0, curH = 0;
    if (drmGetCap(gpu->fd, DRM_CAP_CURSOR_WIDTH, &curW))
        curW = 64;
    if (drmGetCap(gpu->fd, DRM_CAP_CURSOR_HEIGHT, &curH))
        curH = 64;

    drmProps.cursorSize = Hyprutils::Math::Vector2D{(double)curW, (double)curH};

    uint64_t cap = 0;
    if (drmGetCap(gpu->fd, DRM_CAP_PRIME, &cap) || !(cap & DRM_PRIME_CAP_IMPORT)) {
        backend->log(AQ_LOG_ERROR, std::format("drm: DRM_PRIME_CAP_IMPORT unsupported"));
        return false;
    }

    if (drmGetCap(gpu->fd, DRM_CAP_CRTC_IN_VBLANK_EVENT, &cap) || !cap) {
        backend->log(AQ_LOG_ERROR, std::format("drm: DRM_CAP_CRTC_IN_VBLANK_EVENT unsupported"));
        return false;
    }

    if (drmGetCap(gpu->fd, DRM_CAP_TIMESTAMP_MONOTONIC, &cap) || !cap) {
        backend->log(AQ_LOG_ERROR, std::format("drm: DRM_PRIME_CAP_IMPORT unsupported"));
        return false;
    }

    if (drmSetClientCap(gpu->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) {
        backend->log(AQ_LOG_ERROR, std::format("drm: DRM_CLIENT_CAP_UNIVERSAL_PLANES unsupported"));
        return false;
    }

    drmProps.supportsAsyncCommit = drmGetCap(gpu->fd, DRM_CAP_ASYNC_PAGE_FLIP, &cap) == 0 && cap == 1;
    drmProps.supportsTimelines   = drmGetCap(gpu->fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap) == 0 && cap == 1;

    if (envEnabled("AQ_NO_MODIFIERS")) {
        backend->log(AQ_LOG_WARNING, "drm: AQ_NO_MODIFIERS enabled, disabling modifiers for DRM buffers.");
        drmProps.supportsAddFb2Modifiers = false;
    } else
        drmProps.supportsAddFb2Modifiers = drmGetCap(gpu->fd, DRM_CAP_ADDFB2_MODIFIERS, &cap) == 0 && cap == 1;

    if (envEnabled("AQ_NO_ATOMIC")) {
        backend->log(AQ_LOG_WARNING, "drm: AQ_NO_ATOMIC enabled, using the legacy drm iface");
        impl = makeShared<CDRMLegacyImpl>(self.lock());
    } else if (drmSetClientCap(gpu->fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
        backend->log(AQ_LOG_WARNING, "drm: failed to set DRM_CLIENT_CAP_ATOMIC, falling back to legacy");
        impl = makeShared<CDRMLegacyImpl>(self.lock());
    } else {
        backend->log(AQ_LOG_DEBUG, "drm: Atomic supported, using atomic for modesetting");
        impl                         = makeShared<CDRMAtomicImpl>(self.lock());
        drmProps.supportsAsyncCommit = drmGetCap(gpu->fd, DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP, &cap) == 0 && cap == 1;
        atomic                       = true;
        if (!initCommitThread())
            backend->log(AQ_LOG_WARNING, "drm: Failed to create the asynchronous commit worker");
    }

    backend->log(AQ_LOG_DEBUG, std::format("drm: drmProps.supportsAsyncCommit: {}", drmProps.supportsAsyncCommit));
    backend->log(AQ_LOG_DEBUG, std::format("drm: drmProps.supportsAddFb2Modifiers: {}", drmProps.supportsAddFb2Modifiers));
    backend->log(AQ_LOG_DEBUG, std::format("drm: drmProps.supportsTimelines: {}", drmProps.supportsTimelines));

    // TODO: allow no-modifiers?

    return true;
}

bool Aquamarine::CDRMBackend::initResources() {
    auto resources = drmModeGetResources(gpu->fd);
    if (!resources) {
        backend->log(AQ_LOG_ERROR, "drm: drmModeGetResources failed");
        return false;
    }

    CScopeGuard resourcesGuard([resources] { drmModeFreeResources(resources); });

    bool        success = false;

    CScopeGuard rollbackGuard([&] {
        if (!success) {
            crtcs.clear();
            planes.clear();
        }
    });

    backend->log(AQ_LOG_DEBUG, std::format("drm: found {} CRTCs", resources->count_crtcs));

    for (int i = 0; i < resources->count_crtcs; ++i) {
        auto CRTC     = makeShared<SDRMCRTC>();
        CRTC->id      = resources->crtcs[i];
        CRTC->backend = self;

        auto drmCRTC = drmModeGetCrtc(gpu->fd, CRTC->id);
        if (!drmCRTC) {
            backend->log(AQ_LOG_ERROR, std::format("drm: drmModeGetCrtc for crtc {} failed", CRTC->id));
            return false;
        }

        CScopeGuard crtcGuard([drmCRTC] { drmModeFreeCrtc(drmCRTC); });

        CRTC->legacy.gammaSize = drmCRTC->gamma_size;

        if (!getDRMCRTCProps(gpu->fd, CRTC->id, &CRTC->props)) {
            backend->log(AQ_LOG_ERROR, std::format("drm: getDRMCRTCProps for crtc {} failed", CRTC->id));
            return false;
        }

        crtcs.emplace_back(CRTC);
    }

    if (crtcs.size() > 32) {
        backend->log(AQ_LOG_CRITICAL, "drm: Cannot support more than 32 CRTCs");
        return false;
    }

    auto planeResources = drmModeGetPlaneResources(gpu->fd);
    if (!planeResources) {
        backend->log(AQ_LOG_ERROR, "drm: drmModeGetPlaneResources failed");
        return false;
    }

    CScopeGuard planeResourcesGuard([planeResources] { drmModeFreePlaneResources(planeResources); });

    for (uint32_t i = 0; i < planeResources->count_planes; ++i) {
        const auto id = planeResources->planes[i];

        auto       plane = drmModeGetPlane(gpu->fd, id);
        if (!plane) {
            backend->log(AQ_LOG_ERROR, std::format("drm: drmModeGetPlane for plane {} failed", id));
            return false;
        }

        CScopeGuard planeGuard([plane] { drmModeFreePlane(plane); });

        auto        aqPlane = makeShared<SDRMPlane>();
        aqPlane->backend    = self;
        aqPlane->self       = aqPlane;

        if (!aqPlane->init(plane)) {
            backend->log(AQ_LOG_ERROR, std::format("drm: aqPlane->init for plane {} failed", id));
            return false;
        }

        planes.emplace_back(aqPlane);
    }

    success = true;
    return true;
}

bool Aquamarine::CDRMBackend::shouldBlit() {
    return !!primary;
}

bool Aquamarine::CDRMBackend::initMgpu() {
    if (!rendererRequired)
        return true;

    SP<CGBMAllocator> newAllocator;
    if (primary || backend->primaryAllocator->type() != AQ_ALLOCATOR_TYPE_GBM) {
        newAllocator            = CGBMAllocator::create(backend->reopenDRMNode(gpu->fd), backend);
        rendererState.allocator = newAllocator;
    } else {
        newAllocator            = ((CGBMAllocator*)backend->primaryAllocator.get())->self.lock();
        rendererState.allocator = newAllocator;
    }

    if (!rendererState.allocator) {
        backend->log(AQ_LOG_ERROR, "drm: initMgpu: no allocator");
        return false;
    }

    rendererState.renderer = CDRMRenderer::attempt(backend.lock(), gpu->renderNodeFd >= 0 ? gpu->renderNodeFd : gpu->fd);

    if (!rendererState.renderer) {
        backend->log(AQ_LOG_ERROR, "drm: initMgpu: no renderer");
        return false;
    }

    rendererState.renderer->self = rendererState.renderer;

    buildGlFormats(rendererState.renderer->formats);

    return true;
}

bool Aquamarine::CDRMBackend::updateSecondaryRendererState(DRMFBList* deferred) {
    if (!backend->ready)
        return true;

    if (!primary) {
        if (rendererState.renderer && rendererState.allocator)
            return true;

        return initMgpu();
    }

    const bool hasEnabledOutputs = std::ranges::any_of(connectors, [](const auto& c) { return c->status == DRM_MODE_CONNECTED && c->output && c->output->enabledState; });

    if (hasEnabledOutputs) {
        if (rendererState.renderer && rendererState.allocator)
            return true;

        backend->log(AQ_LOG_DEBUG, std::format("drm: Initializing secondary renderer on {}, has enabled outputs", gpu->path));
        return initMgpu();
    }

    if (!rendererState.renderer && !rendererState.allocator)
        return true;

    backend->log(AQ_LOG_DEBUG, std::format("drm: Deinitializing secondary renderer on {}, no enabled outputs", gpu->path));

    DRMFBList retired;
    for (auto const& c : connectors) {
        c->releaseFBReferences(&retired);

        if (c->output)
            c->output->releaseMgpuResources();
    }

    if (rendererState.allocator)
        rendererState.allocator->destroyBuffers();

    rendererState.renderer.reset();
    rendererState.allocator.reset();

    if (deferred) {
        for (auto& fb : retired)
            deferred->emplace_back(std::move(fb));
    } else {
        for (const auto& fb : retired) {
            if (auto buffer = fb->buffer.lock())
                buffer->backendUnpin();
        }
    }

    return true;
}

void Aquamarine::CDRMBackend::buildGlFormats(const std::vector<SGLFormat>& fmts) {
    std::vector<SDRMFormat> result;

    for (auto const& fmt : fmts) {
        if (fmt.external && fmt.modifier != DRM_FORMAT_MOD_INVALID)
            continue;

        if (auto it = std::ranges::find_if(result, [fmt](const auto& e) { return fmt.drmFormat == e.drmFormat; }); it != result.end()) {
            it->modifiers.emplace_back(fmt.modifier);
            continue;
        }

        result.emplace_back(SDRMFormat{
            .drmFormat = fmt.drmFormat,
            .modifiers = {fmt.modifier},
        });
    }

    glFormats = result;
}

void Aquamarine::CDRMBackend::recheckCRTCs() {
    if (connectors.empty() || crtcs.empty())
        return;

    backend->log(AQ_LOG_DEBUG, "drm: Rechecking CRTCs");

    std::vector<SP<SDRMConnector>> recheck, changed;
    for (auto const& c : connectors) {
        if (c->tilingRedundant) {
            backend->log(AQ_LOG_DEBUG, std::format("drm: Skipping tiling-redundant connector {} from CRTC assignment", c->szName));
            continue;
        }

        // disabled outputs release their CRTCs so active outputs get priority
        if (c->crtc && c->status == DRM_MODE_CONNECTED && c->output && !c->output->enabledState) {
            backend->log(AQ_LOG_DEBUG, std::format("drm: {} is disabled, releasing crtc {}", c->szName, c->crtc->id));
            c->setCRTC(nullptr);
        }

        if (c->crtc && c->status == DRM_MODE_CONNECTED) {
            backend->log(AQ_LOG_DEBUG, std::format("drm: Skipping connector {}, has crtc {} and is connected", c->szName, c->crtc->id));
            continue;
        }

        recheck.emplace_back(c);
        backend->log(AQ_LOG_DEBUG, std::format("drm: connector {}, has crtc {}, will be rechecked", c->szName, c->crtc ? (int)c->crtc->id : -1));
    }

    for (size_t i = 0; i < crtcs.size(); ++i) {
        const auto& crtc  = crtcs.at(i);
        bool        taken = false;
        for (auto const& c : connectors) {
            if (c->crtc != crtc)
                continue;

            if (c->status != DRM_MODE_CONNECTED && !c->tilingRedundant)
                continue;

            backend->log(AQ_LOG_DEBUG, std::format("drm: slot {} crtc {} taken by {}, skipping", i, c->crtc->id, c->szName));
            taken = true;
            break;
        }

        if (taken)
            continue;

        bool assigned = false;

        // try to use a connected, enabled connector
        for (auto const& c : recheck) {
            if (!(c->possibleCrtcs & (1 << i)))
                continue;

            if (c->status != DRM_MODE_CONNECTED)
                continue;

            // Pass 1 only assigns to enabled connectors
            if (c->output && !c->output->enabledState)
                continue;

            // deactivate old output
            if (c->output && c->output->enabledState) {
                c->output->state->setEnabled(false);
                if (!c->output->commit()) {
                    backend->log(AQ_LOG_ERROR, std::format("drm: Failed to disable {} before reassigning its CRTC", c->szName));
                    continue;
                }
            }

            backend->log(AQ_LOG_DEBUG,
                         std::format("drm: connected slot {} crtc {} assigned to {}{}", i, crtc->id, c->szName, c->crtc ? std::format(" (old {})", c->crtc->id) : ""));
            c->setCRTC(crtc);
            assigned = true;
            changed.emplace_back(c);
            std::erase(recheck, c);
            break;
        }

        if (!assigned)
            backend->log(AQ_LOG_DEBUG, std::format("drm: slot {} crtc {} unassigned", i, crtc->id));
    }

    // Pass 2: assign remaining CRTCs to disabled connectors as backup slots
    for (size_t i = 0; i < crtcs.size(); ++i) {
        bool taken = false;
        for (auto const& c : connectors) {
            if (c->crtc == crtcs.at(i)) {
                taken = true;
                break;
            }
        }
        if (taken)
            continue;

        for (auto const& c : recheck) {
            if (!(c->possibleCrtcs & (1 << i)))
                continue;
            if (c->status != DRM_MODE_CONNECTED)
                continue;
            if (c->output && c->output->enabledState)
                continue;

            backend->log(AQ_LOG_DEBUG, std::format("drm: backup slot {} crtc {} assigned to disabled {}", i, crtcs.at(i)->id, c->szName));
            c->setCRTC(crtcs.at(i));
            std::erase(recheck, c);
            break;
        }
    }

    for (auto const& c : connectors) {
        if (c->status == DRM_MODE_CONNECTED || c->tilingRedundant)
            continue;

        if (c->crtc)
            backend->log(AQ_LOG_DEBUG, std::format("drm: {} is not connected, clearing stale crtc {}", c->szName, c->crtc->id));
        c->setCRTC(nullptr);
    }

    // tell the user to re-assign a valid mode etc, if needed
    for (auto const& conn : changed) {
        if (conn->status != DRM_MODE_CONNECTED && conn->output)
            conn->output->events.state.emit(IOutput::SStateEvent{});
    }
}

bool Aquamarine::CDRMBackend::grabFormats() {
    // FIXME: do this properly maybe?
    return true;
}

bool Aquamarine::CDRMBackend::registerGPU(SP<CSessionDevice> gpu_, SP<CDRMBackend> primary_) {
    gpu     = gpu_;
    primary = primary_;

    auto driverFromName = [](const std::string_view name) {
        if (name == "i915" || name == "xe")
            return AQ_BACKEND_GPU_DRIVER_INTEL;
        if (name == "amdgpu" || name == "radeon")
            return AQ_BACKEND_GPU_DRIVER_AMD;
        if (name == "nvidia-drm")
            return AQ_BACKEND_GPU_DRIVER_NVIDIA;
        if (name == "nouveau")
            return AQ_BACKEND_GPU_DRIVER_NOUVEAU;
        if (name == "evdi")
            return AQ_BACKEND_GPU_DRIVER_EVDI;

        return AQ_BACKEND_GPU_DRIVER_UNKNOWN;
    };

    auto drmName = drmGetDeviceNameFromFd2(gpu->fd);
    auto drmVer  = drmGetVersion(gpu->fd);

    gpuName = drmName ? drmName : "unknown";

    auto drmVerName = drmVer && drmVer->name ? drmVer->name : "unknown";
    driver          = driverFromName(drmVerName);
    if (driver == AQ_BACKEND_GPU_DRIVER_EVDI) {
        // DisplayLink/evdi exposes KMS without a usable EGL renderer.
        primary          = {};
        rendererRequired = false;
    }

    backend->log(AQ_LOG_DEBUG,
                 std::format("drm: Starting backend for {}, with driver {}{}", drmName ? drmName : "unknown", drmVerName,
                             (primary ? std::format(" with primary {}", primary->gpu->path) : "")));

    free(drmName);

    if (drmVer)
        drmFreeVersion(drmVer);
    listeners.gpuChange = gpu->events.change.listen([this](const CSessionDevice::SChangeEvent& E) {
        if (E.type == CSessionDevice::AQ_SESSION_EVENT_CHANGE_HOTPLUG) {
            backend->log(AQ_LOG_DEBUG, std::format("drm: Got a hotplug event for {}", gpuName));
            recheckOutputs();
        } else if (E.type == CSessionDevice::AQ_SESSION_EVENT_CHANGE_LEASE) {
            backend->log(AQ_LOG_DEBUG, std::format("drm: Got a lease event for {}", gpuName));
            scanLeases();
        }
    });

    listeners.gpuRemove = gpu->events.remove.listen([this] {
        std::erase_if(backend->implementations, [this](const auto& impl) { return impl->drmFD() == this->drmFD(); });
        backend->events.pollFDsChanged.emit();
    });

    return true;
}

eBackendType Aquamarine::CDRMBackend::type() {
    return eBackendType::AQ_BACKEND_DRM;
}

void Aquamarine::CDRMBackend::markRedundantTiles() {
    for (const auto& conn : connectors) {
        conn->parseTileInfo();
        conn->tilingRedundant = false;
    }

    std::unordered_map<uint32_t, std::vector<SP<SDRMConnector>>> tileGroups;
    for (const auto& conn : connectors) {
        if (conn->status != DRM_MODE_CONNECTED || conn->tileInfo.groupId == 0)
            continue;
        tileGroups[conn->tileInfo.groupId].emplace_back(conn);
    }

    for (auto& [groupId, members] : tileGroups) {
        if (members.size() <= 1)
            continue;

        const auto& ti         = members.at(0)->tileInfo;
        int         fullWidth  = ti.numHTile * ti.tileHSize;
        int         fullHeight = ti.numVTile * ti.tileVSize;

        backend->log(AQ_LOG_DEBUG, std::format("drm: Tile group {} has {} members, full tiled resolution {}x{}", groupId, members.size(), fullWidth, fullHeight));

        // check if any single connector already has the full tiled resolution,
        // meaning the driver handles tiling internally
        SP<SDRMConnector> fullResConn;
        for (const auto& conn : members) {
            if (conn->maxMode.x >= fullWidth && conn->maxMode.y >= fullHeight) {
                fullResConn = conn;
                break;
            }
        }

        if (!fullResConn)
            continue;

        for (const auto& conn : members) {
            if (conn == fullResConn)
                continue;

            conn->tilingRedundant = true;
            backend->log(AQ_LOG_DEBUG,
                         std::format("drm: Connector {} marked as tiling redundant (tile group {}, driver-managed by {})", conn->szName, groupId, fullResConn->szName));
        }
    }
}

void Aquamarine::CDRMBackend::recheckOutputs() {
    scanConnectors();
    markRedundantTiles();

    // disconnect now to possibly free up crtcs
    for (const auto& conn : connectors) {
        if (conn->status != DRM_MODE_CONNECTED && conn->output) {
            backend->log(AQ_LOG_DEBUG, std::format("drm: Connector {} disconnected", conn->szName));
            conn->disconnect();
        }

        // also disconnect redundant tiled connectors that may have been previously connected
        if (conn->tilingRedundant && conn->output) {
            backend->log(AQ_LOG_DEBUG, std::format("drm: Disconnecting tiling-redundant connector {}", conn->szName));
            conn->disconnect();
        }
    }

    recheckCRTCs();

    // now that crtcs are assigned, connect outputs
    for (const auto& conn : connectors) {
        if (conn->status == DRM_MODE_CONNECTED && !conn->output && !conn->tilingRedundant) {
            if (!conn->crtc) {
                backend->log(AQ_LOG_DEBUG, std::format("drm: {} has no CRTC, deferring connection", conn->szName));
                continue;
            }

            backend->log(AQ_LOG_DEBUG, std::format("drm: Connector {} connected", conn->szName));

            auto drmConn = drmModeGetConnector(gpu->fd, conn->id);

            // ??? was valid 5 sec ago...
            if (!drmConn) {
                backend->log(AQ_LOG_ERROR, std::format("drm: Connector {} couldn't be connected, drm connector id is no longer valid??", conn->szName));
                continue;
            }

            conn->connect(drmConn);
            drmModeFreeConnector(drmConn);
        }
    }

    if (!updateSecondaryRendererState())
        backend->log(AQ_LOG_ERROR, std::format("drm: Failed to update renderer state for {}", gpu->path));
}

void Aquamarine::CDRMBackend::scanConnectors() {
    backend->log(AQ_LOG_DEBUG, std::format("drm: Scanning connectors for {}", gpu->path));

    auto resources = drmModeGetResources(gpu->fd);
    if (!resources) {
        backend->log(AQ_LOG_ERROR, std::format("drm: Scanning connectors for {} failed", gpu->path));
        return;
    }

    for (int i = 0; i < resources->count_connectors; ++i) {
        uint32_t          connectorID = resources->connectors[i];

        SP<SDRMConnector> conn;
        auto              drmConn = drmModeGetConnector(gpu->fd, connectorID);

        backend->log(AQ_LOG_DEBUG, std::format("drm: Scanning connector id {}", connectorID));

        if (!drmConn) {
            backend->log(AQ_LOG_ERROR, std::format("drm: Failed to get connector id {}", connectorID));
            continue;
        }

        auto it = std::ranges::find_if(connectors, [connectorID](const auto& e) { return e->id == connectorID; });
        if (it == connectors.end()) {
            backend->log(AQ_LOG_DEBUG, std::format("drm: Initializing connector id {}", connectorID));
            conn          = connectors.emplace_back(SP<SDRMConnector>(new SDRMConnector()));
            conn->self    = conn;
            conn->backend = self;
            conn->id      = connectorID;
            if (!conn->init(drmConn)) {
                backend->log(AQ_LOG_ERROR, std::format("drm: Connector id {} failed initializing", connectorID));
                connectors.pop_back();
                drmModeFreeConnector(drmConn);
                continue;
            }
        } else {
            backend->log(AQ_LOG_DEBUG, std::format("drm: Connector id {} already initialized", connectorID));
            conn = *it;
        }

        conn->status = drmConn->connection;

        conn->maxMode = {};
        for (int i = 0; i < drmConn->count_modes; ++i) {
            conn->maxMode.x = std::max<double>(conn->maxMode.x, drmConn->modes[i].hdisplay);
            conn->maxMode.y = std::max<double>(conn->maxMode.y, drmConn->modes[i].vdisplay);
        }

        if (conn->crtc)
            conn->recheckCRTCProps();

        backend->log(AQ_LOG_DEBUG, std::format("drm: Connector {} connection state: {}", connectorID, (int)drmConn->connection));

        drmModeFreeConnector(drmConn);
    }

    // cleanup hot unplugged connectors
    std::erase_if(connectors, [resources](auto& conn) {
        for (int i = 0; i < resources->count_connectors; ++i) {
            if (resources->connectors[i] == conn->id)
                return false;
        }
        conn->disconnect();
        return true;
    });

    drmModeFreeResources(resources);
}

void Aquamarine::CDRMBackend::scanLeases() {
    auto lessees = drmModeListLessees(gpu->fd);
    if (!lessees) {
        backend->log(AQ_LOG_ERROR, "drmModeListLessees failed");
        return;
    }

    for (auto const& c : connectors) {
        if (!c->output || !c->output->lease)
            continue;

        bool has = false;
        for (size_t i = 0; i < lessees->count; ++i) {
            if (lessees->lessees[i] == c->output->lease->lesseeID) {
                has = true;
                break;
            }
        }

        if (has)
            continue;

        backend->log(AQ_LOG_DEBUG, std::format("lessee {} gone, removing", c->output->lease->lesseeID));

        // don't terminate
        c->output->lease->active = false;

        auto l = c->output->lease;

        for (auto const& c2 : connectors) {
            if (!c2->output || c2->output->lease != c->output->lease)
                continue;

            c2->output->lease.reset();
        }

        l->destroy();
    }

    drmFree(lessees);
}

bool Aquamarine::CDRMBackend::start() {
    return true;
}

std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>> Aquamarine::CDRMBackend::pollFDs() {
    std::vector<SP<SPollFD>> fds = {makeShared<SPollFD>(gpu->fd, [this]() { dispatchEvents(); })};
    if (commitThread)
        fds.emplace_back(makeShared<SPollFD>(commitThread->completionFD(), [this]() { dispatchCommitResults(); }));
    return fds;
}

int Aquamarine::CDRMBackend::drmFD() {
    return gpu->fd;
}

int Aquamarine::CDRMBackend::drmRenderNodeFD() {
    return gpu->renderNodeFd;
}

eBackendGPUDriver Aquamarine::CDRMBackend::gpuDriver() {
    return driver;
}

uintptr_t Aquamarine::CDRMBackend::nextPageFlipID() {
    return ++m_lastPageFlipID;
}

SP<SDRMCRTC> Aquamarine::CDRMBackend::crtcByID(uint32_t id) {
    auto it = std::find_if(crtcs.begin(), crtcs.end(), [id](const auto& c) { return c->id == id; });
    return it == crtcs.end() ? nullptr : *it;
}

uintptr_t Aquamarine::SDRMCRTC::armPageFlip(CWeakPointer<SDRMConnector> connector, bool async, std::optional<uintptr_t> requestedID, uint64_t commitID, bool resultReady) {
    if (pendingFlip.id && !(pendingFlip.connector == connector)) {
        if (const auto PREV = pendingFlip.connector.lock()) {
            backend->log(AQ_LOG_ERROR, std::format("drm: crtc {} page-flip slot taken from {}, dropping its frame", id, PREV->szName));
            PREV->invalidateFrame();
        }
    }

    pendingFlip.id          = requestedID.value_or(backend->nextPageFlipID());
    pendingFlip.connector   = connector;
    pendingFlip.async       = async;
    pendingFlip.commitID    = commitID;
    pendingFlip.resultReady = resultReady;
    pendingFlip.early.valid = false;

    return pendingFlip.id.value();
}

void Aquamarine::SDRMCRTC::disarmPageFlip() {
    pendingFlip.id.reset();
    pendingFlip.connector.reset();
    pendingFlip.async       = false;
    pendingFlip.commitID    = 0;
    pendingFlip.resultReady = true;
    pendingFlip.early.valid = false;
}

static CWeakPointer<CDRMBackend> gDispatchingBackend;
static void                      handlePF(int fd, unsigned seq, unsigned tv_sec, unsigned tv_usec, unsigned crtc_id, void* data) {
    const auto BACKEND = gDispatchingBackend.lock();

    if (!BACKEND || BACKEND->drmFD() != fd)
        return;

    BACKEND->handlePageFlip(rc<uintptr_t>(data), seq, tv_sec, tv_usec, crtc_id);
}

void Aquamarine::CDRMBackend::handlePageFlip(uintptr_t flipID, unsigned int seq, unsigned int sec, unsigned int usec, uint32_t crtcID) {
    const auto CRTC = crtcByID(crtcID);
    if (!CRTC || !flipID || CRTC->pendingFlip.id != flipID) {
        log(AQ_LOG_DEBUG, std::format("drm: Ignoring a stale pf event, flip {} on crtc {}", flipID, crtcID));
        return;
    }

    if (!CRTC->pendingFlip.resultReady) {
        CRTC->pendingFlip.early = {
            .valid = true,
            .seq   = seq,
            .sec   = sec,
            .usec  = usec,
        };
        return;
    }

    const auto CONNECTOR = CRTC->pendingFlip.connector.lock();
    const auto ASYNC     = CRTC->pendingFlip.async;
    const auto COMMIT_ID = CRTC->pendingFlip.commitID;
    CRTC->disarmPageFlip();

    if (commitThread && COMMIT_ID)
        commitThread->releaseQueue(crtcID, COMMIT_ID);

    if (!CONNECTOR) {
        log(AQ_LOG_DEBUG, "drm: Ignoring a pf event whose connector is gone");
        return;
    }

    if (CONNECTOR->output && CONNECTOR->output->pendingAsyncCommit == COMMIT_ID)
        CONNECTOR->output->pendingAsyncCommit = 0;

    TRACE(log(AQ_LOG_TRACE, std::format("drm: pf event seq {} sec {} usec {} crtc {}", seq, sec, usec, crtcID)));

    if (!CONNECTOR->sched.frameInFlight()) {
        log(AQ_LOG_DEBUG, std::format("drm: Ignoring pf event on {}, no frame in flight?", CONNECTOR->szName));
        return;
    }

    CONNECTOR->sched.onFrameComplete();

    if (CONNECTOR->status != DRM_MODE_CONNECTED || !CONNECTOR->crtc || !CONNECTOR->output) {
        log(AQ_LOG_DEBUG, "drm: Ignoring a pf event from a disabled crtc / connector");
        return;
    }

    CFrameRunningGuard frameRunning(CONNECTOR->sched);

    if (!ASYNC || COMMIT_ID) {
        CONNECTOR->onPresent();

        uint32_t flags = IOutput::AQ_OUTPUT_PRESENT_HW_CLOCK | IOutput::AQ_OUTPUT_PRESENT_HW_COMPLETION | IOutput::AQ_OUTPUT_PRESENT_ZEROCOPY;
        if (!ASYNC)
            flags |= IOutput::AQ_OUTPUT_PRESENT_VSYNC;

        timespec presented = {.tv_sec = sc<time_t>(sec), .tv_nsec = sc<long>(usec * 1000)};

        if (gpuDriver() == AQ_BACKEND_GPU_DRIVER_NVIDIA && seq == 0)
            flags &= ~IOutput::AQ_OUTPUT_PRESENT_HW_CLOCK;

        CONNECTOR->output->events.present.emit(IOutput::SPresentEvent{
            .presented = sessionActive(),
            .when      = &presented,
            .seq       = seq,
            .refresh   = sc<int>(CONNECTOR->refresh ? (1000000000000LL / CONNECTOR->refresh) : 0),
            .flags     = flags,
            .commitID  = COMMIT_ID,
        });
    }

    if (sessionActive() && CONNECTOR->output->enabledState && !CONNECTOR->sched.frameScheduled())
        CONNECTOR->sched.frameReady.emit();
}

bool Aquamarine::CDRMBackend::dispatchEvents() {
    drmEventContext event = {
        .version            = 3,
        .page_flip_handler2 = ::handlePF,
    };

    // drmHandleEvent -> handlePF can dispatch another gpus fd so gDispatchingBackend is the wrong backend.
    const auto  PREVIOUS = gDispatchingBackend;
    CScopeGuard dispatchGuard([&PREVIOUS] { gDispatchingBackend = PREVIOUS; });
    gDispatchingBackend = self;

    if (drmHandleEvent(gpu->fd, &event) != 0)
        backend->log(AQ_LOG_ERROR, std::format("drm: Failed to handle event on fd {}", gpu->fd));

    return true;
}

uint32_t Aquamarine::CDRMBackend::capabilities() {
    if (getCursorFormats().empty())
        return 0;
    return eBackendCapabilities::AQ_BACKEND_CAPABILITY_POINTER;
}

bool Aquamarine::CDRMBackend::setCursor(SP<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot) {
    return false;
}

void Aquamarine::CDRMBackend::onReady() {
    backend->log(AQ_LOG_DEBUG, std::format("drm: Connectors size2 {}", connectors.size()));

    // init a drm renderer to gather gl formats.
    // if we are secondary, initMgpu will have done that
    if (!primary && rendererRequired) {
        auto a = CGBMAllocator::create(backend->reopenDRMNode(gpu->fd), backend);
        if (!a)
            backend->log(AQ_LOG_ERROR, "drm: onReady: no renderer for gl formats");
        else {
            auto r = CDRMRenderer::attempt(backend.lock(), gpu->renderNodeFd >= 0 ? gpu->renderNodeFd : gpu->fd);
            if (!r)
                backend->log(AQ_LOG_ERROR, "drm: onReady: no renderer for gl formats");
            else {
                TRACE(backend->log(AQ_LOG_TRACE, std::format("drm: onReady: gathered {} gl formats", r->formats.size())));
                buildGlFormats(r->formats);
                r.reset();
                a.reset();
            }
        }
    }

    for (auto const& c : connectors) {
        backend->log(AQ_LOG_DEBUG, std::format("drm: onReady: connector {}", c->id));
        if (!c->output)
            continue;

        backend->log(AQ_LOG_DEBUG, std::format("drm: onReady: connector {} has output name {}", c->id, c->output->name));

        // swapchain has to be created here because allocator is absent in connect if not ready
        auto primaryBackend  = primary ? primary : self;
        c->output->swapchain = CSwapchain::create(backend->primaryAllocator, primaryBackend.lock());
        c->output->swapchain->reconfigure(SSwapchainOptions{.length = 0, .scanout = true, .multigpu = !!primary, .scanoutOutput = c->output}); // mark the swapchain for scanout
        c->output->needsFrame = true;

        backend->events.newOutput.emit(SP<IOutput>(c->output));
    }

    if (!updateSecondaryRendererState()) {
        backend->log(AQ_LOG_ERROR, std::format("drm: Failed to initialize renderer state for {}", gpu->path));
        return;
    }
}

std::vector<SDRMFormat> Aquamarine::CDRMBackend::getRenderFormats() {
    for (auto const& p : planes) {
        if (p->type != DRM_PLANE_TYPE_PRIMARY)
            continue;

        return p->formats;
    }

    return {};
}

std::vector<SDRMFormat> Aquamarine::CDRMBackend::getRenderableFormats() {
    return glFormats;
}

std::vector<SDRMFormat> Aquamarine::CDRMBackend::getCursorFormats() {
    for (auto const& p : planes) {
        if (p->type != DRM_PLANE_TYPE_CURSOR)
            continue;

        if (primary) {
            TRACE(backend->log(AQ_LOG_TRACE, std::format("drm: getCursorFormats on secondary {}", gpu->path)));

            // this is a secondary GPU renderer. In order to receive buffers,
            // we'll force linear modifiers.
            // TODO: don't. Find a common maybe?
            auto fmts = p->formats;
            for (auto& fmt : fmts) {
                fmt.modifiers = {DRM_FORMAT_MOD_LINEAR};
            }
            return fmts;
        }

        return p->formats;
    }

    return {};
}

bool Aquamarine::CDRMBackend::createOutput(const std::string&) {
    return false;
}

int Aquamarine::CDRMBackend::getNonMasterFD() {
    int fd = open(gpuName.c_str(), O_RDWR | O_CLOEXEC);

    if (fd < 0) {
        backend->log(AQ_LOG_ERROR, "drm: couldn't dupe fd for non master");
        return -1;
    }

    if (drmIsMaster(fd) && drmDropMaster(fd) < 0) {
        backend->log(AQ_LOG_ERROR, "drm: couldn't drop master from duped fd");
        close(fd);
        return -1;
    }

    return fd;
}

SP<IAllocator> Aquamarine::CDRMBackend::preferredAllocator() {
    return backend->primaryAllocator;
}

std::vector<SP<IAllocator>> Aquamarine::CDRMBackend::getAllocators() {
    return {backend->primaryAllocator, dumbAllocator};
}

Hyprutils::Memory::CWeakPointer<IBackendImplementation> Aquamarine::CDRMBackend::getPrimary() {
    return primary;
}

bool Aquamarine::SDRMPlane::init(drmModePlane* plane) {
    id = plane->plane_id;

    if (!getDRMPlaneProps(backend->gpu->fd, id, &props))
        return false;

    if (props.values.color_range)
        getDRMPlaneColorRange(backend->gpu->fd, props.values.color_range, &colorRange);

    if (!getDRMProp(backend->gpu->fd, id, props.values.type, &type))
        return false;

    initialID = id;

    backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Plane {} has type {}", id, (int)type));

    backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Plane {} has {} formats", id, plane->count_formats));

    for (size_t i = 0; i < plane->count_formats; ++i) {
        if (type != DRM_PLANE_TYPE_CURSOR)
            formats.emplace_back(SDRMFormat{.drmFormat = plane->formats[i], .modifiers = {DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID}});
        else
            formats.emplace_back(SDRMFormat{.drmFormat = plane->formats[i], .modifiers = {DRM_FORMAT_MOD_LINEAR}});

        TRACE(backend->backend->log(AQ_LOG_TRACE, std::format("drm: | Format {}", fourccToName(plane->formats[i]))));
    }

    if (props.values.in_formats && backend->drmProps.supportsAddFb2Modifiers) {
        backend->backend->log(AQ_LOG_DEBUG, "drm: Plane: checking for modifiers");

        uint64_t blobID = 0;
        if (!getDRMProp(backend->gpu->fd, id, props.values.in_formats, &blobID)) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Plane: No blob id");
            return false;
        }

        auto blob = drmModeGetPropertyBlob(backend->gpu->fd, blobID);
        if (!blob) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Plane: No property");
            return false;
        }

        drmModeFormatModifierIterator iter = {0};
        while (drmModeFormatModifierBlobIterNext(blob, &iter)) {
            auto it = std::ranges::find_if(formats, [iter](const auto& e) { return e.drmFormat == iter.fmt; });

            TRACE(backend->backend->log(AQ_LOG_TRACE, std::format("drm: | Modifier 0x{:x} : {} with format {}", iter.mod, drmModifierToName(iter.mod), fourccToName(iter.fmt))));

            if (it == formats.end())
                formats.emplace_back(SDRMFormat{.drmFormat = iter.fmt, .modifiers = {iter.mod}});
            else
                it->modifiers.emplace_back(iter.mod);
        }

        drmModeFreePropertyBlob(blob);
    }

    for (size_t i = 0; i < backend->crtcs.size(); ++i) {
        uint32_t crtcBit = (1 << i);
        if (!(plane->possible_crtcs & crtcBit))
            continue;

        auto CRTC = backend->crtcs.at(i);
        if (type == DRM_PLANE_TYPE_PRIMARY && !CRTC->primary) {
            CRTC->primary = self.lock();
            break;
        }

        if (type == DRM_PLANE_TYPE_CURSOR && !CRTC->cursor) {
            CRTC->cursor = self.lock();
            break;
        }
    }

    return true;
}

SP<SDRMCRTC> Aquamarine::SDRMConnector::getCurrentCRTC(const drmModeConnector* connector) {
    uint32_t crtcID = 0;
    if (props.values.crtc_id) {
        TRACE(backend->backend->log(AQ_LOG_TRACE, "drm: Using crtc_id for finding crtc"));
        uint64_t value = 0;
        if (!getDRMProp(backend->gpu->fd, id, props.values.crtc_id, &value)) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Failed to get CRTC_ID");
            return nullptr;
        }
        crtcID = static_cast<uint32_t>(value);
    } else if (connector->encoder_id) {
        TRACE(backend->backend->log(AQ_LOG_TRACE, "drm: Using encoder_id for finding crtc"));
        auto encoder = drmModeGetEncoder(backend->gpu->fd, connector->encoder_id);
        if (!encoder) {
            backend->backend->log(AQ_LOG_ERROR, "drm: drmModeGetEncoder failed");
            return nullptr;
        }
        crtcID = encoder->crtc_id;
        drmModeFreeEncoder(encoder);
    } else {
        backend->backend->log(AQ_LOG_ERROR, "drm: Connector has neither crtc_id nor encoder_id");
        return nullptr;
    }

    if (crtcID == 0) {
        backend->backend->log(AQ_LOG_ERROR, "drm: getCurrentCRTC: No CRTC 0");
        return nullptr;
    }

    auto it = std::find_if(backend->crtcs.begin(), backend->crtcs.end(), [crtcID](const auto& e) { return e->id == crtcID; });

    if (it == backend->crtcs.end()) {
        backend->backend->log(AQ_LOG_ERROR, std::format("drm: Failed to find a CRTC with ID {}", crtcID));
        return nullptr;
    }

    return *it;
}

bool Aquamarine::SDRMConnector::init(drmModeConnector* connector) {
    if (!getDRMConnectorProps(backend->gpu->fd, id, &props))
        return false;
    if (props.values.Colorspace)
        getDRMConnectorColorspace(backend->gpu->fd, props.values.Colorspace, &colorspace);

    auto name = drmModeGetConnectorTypeName(connector->connector_type);
    if (!name)
        name = "ERROR";

    szName = std::format("{}-{}", name, connector->connector_type_id);
    backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Connector gets name {}", szName));

    possibleCrtcs = drmModeConnectorGetPossibleCrtcs(backend->gpu->fd, connector);
    if (!possibleCrtcs)
        backend->backend->log(AQ_LOG_ERROR, "drm: No CRTCs possible");

    setCRTC(getCurrentCRTC(connector));

    return true;
}

void Aquamarine::SDRMConnector::parseTileInfo() {
    tileInfo = {};

    if (!props.values.tile)
        return;

    size_t blobLen  = 0;
    char*  blobData = (char*)getDRMPropBlob(backend->gpu->fd, id, props.values.tile, &blobLen);
    if (!blobData || blobLen == 0) {
        free(blobData);
        return;
    }

    // TILE blob is a text string: "group_id:is_single_monitor:num_h:num_v:h_loc:v_loc:h_size:v_size"
    uint32_t groupId         = 0;
    int      isSingleMonitor = 0, numH = 0, numV = 0, hLoc = 0, vLoc = 0, hSize = 0, vSize = 0;
    if (sscanf(blobData, "%u:%d:%d:%d:%d:%d:%d:%d", &groupId, &isSingleMonitor, &numH, &numV, &hLoc, &vLoc, &hSize, &vSize) == 8 && groupId != 0) {
        tileInfo.groupId         = groupId;
        tileInfo.isSingleMonitor = isSingleMonitor != 0;
        tileInfo.numHTile        = numH;
        tileInfo.numVTile        = numV;
        tileInfo.tileHLoc        = hLoc;
        tileInfo.tileVLoc        = vLoc;
        tileInfo.tileHSize       = hSize;
        tileInfo.tileVSize       = vSize;

        backend->backend->log(AQ_LOG_DEBUG,
                              std::format("drm: Connector {} tile info: group={} tiles={}x{} pos=({},{}) size={}x{}", szName, groupId, numH, numV, hLoc, vLoc, hSize, vSize));
    }

    free(blobData);
}

void Aquamarine::SDRMConnector::releaseFBBuffer(const SP<CDRMFB> fb) {
    if (!fb)
        return;

    if (auto buf = fb->buffer.lock())
        buf->backendUnpin();
}

void Aquamarine::SDRMConnector::releaseFBReferences(DRMFBList* deferred) {
    DRMFBList  retired;
    const auto clearPlane = [&retired](SP<SDRMPlane> plane) {
        if (!plane)
            return;

        const bool SAME_BUFFER = plane->front == plane->back;
        if (plane->front)
            retired.emplace_back(std::move(plane->front));
        if (plane->back && !SAME_BUFFER)
            retired.emplace_back(std::move(plane->back));

        plane->front.reset();
        plane->back.reset();
        plane->last.reset();
        plane->backSet = false;
    };

    if (crtc) {
        clearPlane(crtc->primary);
        clearPlane(crtc->cursor);
        crtc->pendingCursor.reset();
    }

    if (deferred) {
        for (auto& fb : retired)
            deferred->emplace_back(std::move(fb));
        return;
    }

    for (const auto& fb : retired)
        releaseFBBuffer(fb);
}

Aquamarine::SDRMConnector::~SDRMConnector() {
    disconnect();
}

static int32_t calculateRefresh(const drmModeModeInfo& mode) {
    int32_t refresh = (mode.clock * 1000000LL / mode.htotal + mode.vtotal / 2) / mode.vtotal;

    if (mode.flags & DRM_MODE_FLAG_INTERLACE)
        refresh *= 2;

    if (mode.flags & DRM_MODE_FLAG_DBLSCAN)
        refresh /= 2;

    if (mode.vscan > 1)
        refresh /= mode.vscan;

    return refresh;
}

drmModeModeInfo* Aquamarine::SDRMConnector::getCurrentMode() {
    if (!crtc)
        return nullptr;

    if (crtc->props.values.mode_id) {
        size_t size = 0;
        return (drmModeModeInfo*)getDRMPropBlob(backend->gpu->fd, crtc->id, crtc->props.values.mode_id, &size);
    }

    auto drmCrtc = drmModeGetCrtc(backend->gpu->fd, crtc->id);
    if (!drmCrtc)
        return nullptr;
    if (!drmCrtc->mode_valid) {
        drmModeFreeCrtc(drmCrtc);
        return nullptr;
    }

    drmModeModeInfo* modeInfo = (drmModeModeInfo*)malloc(sizeof(drmModeModeInfo));
    if (!modeInfo) {
        drmModeFreeCrtc(drmCrtc);
        return nullptr;
    }

    *modeInfo = drmCrtc->mode;
    drmModeFreeCrtc(drmCrtc);

    return modeInfo;
}

IOutput::SParsedEDID Aquamarine::SDRMConnector::parseEDID(std::vector<uint8_t> data) {
    auto                 info   = di_info_parse_edid(data.data(), data.size());
    IOutput::SParsedEDID parsed = {};
    if (!info) {
        backend->backend->log(AQ_LOG_ERROR, "drm: failed to parse edid");
        return parsed;
    }

    auto edid       = di_info_get_edid(info);
    auto venProduct = di_edid_get_vendor_product(edid);
    auto pnpID      = std::string{venProduct->manufacturer, 3};
    if (PNPIDS.contains(pnpID))
        make = PNPIDS.at(pnpID);
    else
        make = pnpID;

    auto mod = di_info_get_model(info);
    auto ser = di_info_get_serial(info);

    model  = mod ? mod : "";
    serial = ser ? ser : "";

    free(mod);
    free(ser);

    parsed.make   = make;
    parsed.model  = model;
    parsed.serial = serial;

    const auto chromaticity = di_edid_get_chromaticity_coords(edid);
    if (chromaticity) {
        parsed.chromaticityCoords = IOutput::SChromaticityCoords{
            .red   = IOutput::xy{.x = chromaticity->red_x, .y = chromaticity->red_y},
            .green = IOutput::xy{.x = chromaticity->green_x, .y = chromaticity->green_y},
            .blue  = IOutput::xy{.x = chromaticity->blue_x, .y = chromaticity->blue_y},
            .white = IOutput::xy{.x = chromaticity->white_x, .y = chromaticity->white_y},
        };
        TRACE(backend->backend->log(AQ_LOG_TRACE,
                                    std::format("EDID: chromaticity coords {},{} {},{} {},{} {},{}", parsed.chromaticityCoords->red.x, parsed.chromaticityCoords->red.y,
                                                parsed.chromaticityCoords->green.x, parsed.chromaticityCoords->green.y, parsed.chromaticityCoords->blue.x,
                                                parsed.chromaticityCoords->blue.y, parsed.chromaticityCoords->white.y, parsed.chromaticityCoords->white.y)));
    }

    const auto* hdr_static_metadata = di_info_get_hdr_static_metadata(info);
    if (hdr_static_metadata && (hdr_static_metadata->pq || hdr_static_metadata->hlg)) {
        TRACE(backend->backend->log(AQ_LOG_TRACE, std::format("EDID: found HDR pq={} hlg={}", hdr_static_metadata->pq, hdr_static_metadata->hlg)));
        parsed.hdrMetadata = IOutput::SHDRMetadata{
            .desiredContentMaxLuminance      = hdr_static_metadata->desired_content_max_luminance,
            .desiredMaxFrameAverageLuminance = hdr_static_metadata->desired_content_max_frame_avg_luminance,
            .desiredContentMinLuminance      = hdr_static_metadata->desired_content_min_luminance,
            .supportsPQ                      = hdr_static_metadata->pq,
        };
    }

    const auto* ssc = di_info_get_supported_signal_colorimetry(info);
    if (ssc) {
        TRACE(backend->backend->log(AQ_LOG_TRACE, std::format("EDID: found colorimetry bt2020_rgb={}", ssc->bt2020_rgb)));
        parsed.supportsBT2020 = ssc->bt2020_rgb;
    }

    di_info_destroy(info);

    TRACE(backend->backend->log(AQ_LOG_TRACE, "EDID: parsed"));

    return parsed;
}

void Aquamarine::SDRMConnector::recheckCRTCProps() {
    if (!crtc || !output)
        return;

    uint64_t prop      = 0;
    canDoVrr           = props.values.vrr_capable && crtc->props.values.vrr_enabled && getDRMProp(backend->gpu->fd, id, props.values.vrr_capable, &prop) && prop;
    output->vrrCapable = canDoVrr;

    backend->backend->log(AQ_LOG_DEBUG,
                          std::format("drm: connector {} crtc is {} of vrr: props.vrr_capable -> {}, crtc->props.vrr_enabled -> {}", szName, (canDoVrr ? "capable" : "incapable"),
                                      props.values.vrr_capable, crtc->props.values.vrr_enabled));

    output->supportsExplicit = backend->drmProps.supportsTimelines && crtc->props.values.out_fence_ptr && crtc->primary->props.values.in_fence_fd;

    backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Explicit sync {}", output->supportsExplicit ? "supported" : "unsupported"));

    backend->backend->log(AQ_LOG_DEBUG, std::format("drm: connector {} crtc {} CTM", szName, (crtc->props.values.ctm ? "supports" : "doesn't support")));

    backend->backend->log(
        AQ_LOG_DEBUG,
        std::format("drm: connector {} crtc {} HDR ({})", szName, (props.values.hdr_output_metadata ? "supports" : "doesn't support"), props.values.hdr_output_metadata));

    backend->backend->log(AQ_LOG_DEBUG,
                          std::format("drm: connector {} crtc {} Colorspace ({})", szName, (props.values.Colorspace ? "supports" : "doesn't support"), props.values.Colorspace));
}

void Aquamarine::SDRMConnector::connect(drmModeConnector* connector) {
    if (output) {
        backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Not connecting connector {} because it's already connected", szName));
        return;
    }

    // max_bpc-less retry is per-sink and must not persist across hotplugs.
    maxBpcFailed = false;

    backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Connecting connector {}, {}", szName, crtc ? std::format("CRTC ID {}", crtc->id) : "no CRTC"));

    output            = SP<CDRMOutput>(new CDRMOutput(szName, backend, self.lock()));
    output->self      = output;
    output->connector = self.lock();

    backend->backend->log(AQ_LOG_DEBUG, "drm: Dumping detected modes:");

    auto currentModeInfo = getCurrentMode();

    for (int i = 0; i < connector->count_modes; ++i) {
        auto& drmMode = connector->modes[i];

        if (drmMode.flags & DRM_MODE_FLAG_INTERLACE) {
            backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Skipping mode {} because it's interlaced", i));
            continue;
        }

        auto aqMode         = makeShared<SOutputMode>();
        aqMode->pixelSize   = {drmMode.hdisplay, drmMode.vdisplay};
        aqMode->refreshRate = calculateRefresh(drmMode);
        aqMode->preferred   = (drmMode.type & DRM_MODE_TYPE_PREFERRED);
        aqMode->modeInfo    = drmMode;

        if (i == 1)
            fallbackMode = aqMode;

        output->modes.emplace_back(aqMode);

        if (currentModeInfo && std::memcmp(&drmMode, currentModeInfo, sizeof(drmModeModeInfo)) != 0) {
            output->state->setMode(aqMode);

            //uint64_t modeID = 0;
            // getDRMProp(backend->gpu->fd, crtc->id, crtc->props.mode_id, &modeID);
        }

        backend->backend->log(AQ_LOG_DEBUG,
                              std::format("drm: Mode {}: {}x{}@{:.2f}Hz {}", i, (int)aqMode->pixelSize.x, (int)aqMode->pixelSize.y, aqMode->refreshRate / 1000.0,
                                          aqMode->preferred ? " (preferred)" : ""));
    }

    if (!currentModeInfo && fallbackMode)
        output->state->setMode(fallbackMode);

    if (currentModeInfo)
        free(currentModeInfo);

    output->physicalSize = {(double)connector->mmWidth, (double)connector->mmHeight};

    backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Physical size {} (mm)", output->physicalSize));

    switch (connector->subpixel) {
        case DRM_MODE_SUBPIXEL_NONE: output->subpixel = eSubpixelMode::AQ_SUBPIXEL_NONE; break;
        case DRM_MODE_SUBPIXEL_UNKNOWN: output->subpixel = eSubpixelMode::AQ_SUBPIXEL_UNKNOWN; break;
        case DRM_MODE_SUBPIXEL_HORIZONTAL_RGB: output->subpixel = eSubpixelMode::AQ_SUBPIXEL_HORIZONTAL_RGB; break;
        case DRM_MODE_SUBPIXEL_HORIZONTAL_BGR: output->subpixel = eSubpixelMode::AQ_SUBPIXEL_HORIZONTAL_BGR; break;
        case DRM_MODE_SUBPIXEL_VERTICAL_RGB: output->subpixel = eSubpixelMode::AQ_SUBPIXEL_VERTICAL_RGB; break;
        case DRM_MODE_SUBPIXEL_VERTICAL_BGR: output->subpixel = eSubpixelMode::AQ_SUBPIXEL_VERTICAL_BGR; break;
        default: output->subpixel = eSubpixelMode::AQ_SUBPIXEL_UNKNOWN;
    }

    uint64_t prop = 0;
    if (getDRMProp(backend->gpu->fd, id, props.values.non_desktop, &prop)) {
        if (prop == 1)
            backend->backend->log(AQ_LOG_DEBUG, "drm: Non-desktop connector");
        output->nonDesktop = prop;
    }

    maxBpcBounds.fill(0);

    if (props.values.max_bpc && !introspectDRMPropRange(backend->gpu->fd, props.values.max_bpc, maxBpcBounds.data(), &maxBpcBounds[1]))
        backend->backend->log(AQ_LOG_ERROR, "drm: Failed to check max_bpc");

    size_t               edidLen  = 0;
    uint8_t*             edidData = (uint8_t*)getDRMPropBlob(backend->gpu->fd, id, props.values.edid, &edidLen);

    std::vector<uint8_t> edid{edidData, edidData + edidLen};
    auto                 parsedEDID = parseEDID(edid);

    free(edidData);
    edid = {};

    // TODO: subconnectors

    output->make        = parsedEDID.make;
    output->model       = parsedEDID.model;
    output->serial      = parsedEDID.serial;
    output->parsedEDID  = parsedEDID;
    output->description = std::format("{} {} {} ({})", make, model, serial, szName);
    output->needsFrame  = true;

    backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Description {}", output->description));

    status = DRM_MODE_CONNECTED;

    recheckCRTCProps();

    if (!backend->backend->ready)
        return;

    if (!backend->updateSecondaryRendererState()) {
        backend->backend->log(AQ_LOG_ERROR, std::format("drm: Failed to update renderer state for {} on connect", szName));
        return;
    }

    auto primaryBackend = backend->primary ? backend->primary : backend;
    output->swapchain   = CSwapchain::create(backend->backend->primaryAllocator, primaryBackend.lock());
    output->swapchain->reconfigure(SSwapchainOptions{.length = 0, .scanout = true, .multigpu = !!backend->primary, .scanoutOutput = output}); // mark the swapchain for scanout
    output->needsFrame = true;
    backend->backend->events.newOutput.emit(SP<IOutput>(output));
    output->scheduleFrame(IOutput::AQ_SCHEDULE_NEW_CONNECTOR);
}

void Aquamarine::SDRMConnector::disconnect() {
    if (!output) {
        if (backend && backend->backend)
            backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Not disconnecting connector {} because it's already disconnected", szName));
        return;
    }

    backend->cancelAsyncOutput(output.get());
    invalidateFrame();

    status = DRM_MODE_DISCONNECTED;
    releaseFBReferences();

    output->events.destroy.emit();
    output.reset();
}

bool Aquamarine::SDRMConnector::commitState(SDRMConnectorCommitData& data) {
    if (status != DRM_MODE_CONNECTED || !output)
        return false;

    const bool ok = backend->impl->commit(self.lock(), data);

    if (ok && !data.test) {
        output->state->internalState.drmFormat = data.outputState.drmFormat;
        if (data.committed & COutputState::AQ_OUTPUT_STATE_EXPLICIT_OUT_FENCE)
            output->state->internalState.explicitOutFence = data.outputState.explicitOutFence;

        applyCommit(data);
        if (!(data.flags & DRM_MODE_PAGE_FLIP_EVENT))
            onPresent(data.primaryFBChanged, data.cursorFBChanged, &data.retiredFBs);

        for (const auto& fb : data.retiredFBs)
            releaseFBBuffer(fb);
    }

    return ok;
}

void Aquamarine::SDRMConnector::applyCommit(SDRMConnectorCommitData& data) {
    const bool enable         = data.enabled && data.mainFB;
    const bool updatesPrimary = data.modeset || (data.committed & COutputState::AQ_OUTPUT_STATE_BUFFER);

    const auto retireBack = [&data](const SP<SDRMPlane>& plane) {
        if (plane->back && plane->back != plane->front)
            data.retiredFBs.emplace_back(std::move(plane->back));
        plane->back    = plane->front;
        plane->backSet = false;
    };

    if (enable && data.modeset && data.mainFB == crtc->primary->front && crtc->primary->back != crtc->primary->front)
        retireBack(crtc->primary);

    if (enable && updatesPrimary && data.mainFB != crtc->primary->front) {
        if (crtc->primary->back != crtc->primary->front && crtc->primary->back != data.mainFB)
            data.retiredFBs.emplace_back(std::move(crtc->primary->back));

        if (crtc->primary->back != data.mainFB) {
            crtc->primary->back = data.mainFB;
            data.mainFB->buffer->backendPin();
        }
        crtc->primary->backSet = true;
        data.primaryFBChanged  = true;
    }

    if (enable && crtc->cursor) {
        if (data.committed & COutputState::AQ_OUTPUT_STATE_CURSOR_SHAPE) {
            const auto desired = data.cursorVisible ? data.cursorFB : nullptr;

            if (desired == crtc->cursor->front) {
                if (crtc->cursor->back != crtc->cursor->front)
                    retireBack(crtc->cursor);
            } else {
                if (crtc->cursor->back != crtc->cursor->front && crtc->cursor->back != desired)
                    data.retiredFBs.emplace_back(std::move(crtc->cursor->back));

                if (crtc->cursor->back != desired) {
                    crtc->cursor->back = desired;
                    if (desired)
                        desired->buffer->backendPin();
                }

                crtc->cursor->backSet = true;
                data.cursorFBChanged  = true;
            }
        } else if (data.modeset && data.cursorFB == crtc->cursor->front && crtc->cursor->back != crtc->cursor->front)
            retireBack(crtc->cursor);

        if (crtc->pendingCursor == data.cursorFB)
            crtc->pendingCursor.reset();
    }

    if (data.committed & COutputState::AQ_OUTPUT_STATE_MODE)
        refresh = calculateRefresh(data.modeInfo);

    const bool wasEnabled = output->enabledState;
    output->enabledState  = data.enabled;

    if (!output->enabledState)
        invalidateFrame();

    if (!backend->updateSecondaryRendererState(&data.retiredFBs))
        backend->backend->log(AQ_LOG_ERROR, std::format("drm: Failed to update renderer state for {} on applyCommit", szName));

    if (wasEnabled != output->enabledState) {
        auto bk = backend.lock();
        if (bk) {
            bk->backend->log(AQ_LOG_DEBUG, std::format("drm: Connector {} enabledState changed {} -> {}", szName, wasEnabled, output->enabledState));
            auto weak = bk->self;
            bk->backend->addIdleEvent(makeShared<std::function<void(void)>>([weak] {
                auto b = weak.lock();
                if (b)
                    b->recheckOutputs();
            }));
        }
    }

    if (!output->enabledState) {
        releaseFBReferences(&data.retiredFBs);
        return;
    }
}

void Aquamarine::SDRMConnector::invalidateFrame() {
    sched.invalidate();

    if (crtc && crtc->pendingFlip.connector == self) {
        const auto COMMIT_ID = crtc->pendingFlip.commitID;
        if (backend->commitThread && COMMIT_ID)
            backend->commitThread->releaseQueue(crtc->id, COMMIT_ID);
        crtc->disarmPageFlip();

        if (output && COMMIT_ID && output->pendingAsyncCommit == COMMIT_ID) {
            output->pendingAsyncCommit = 0;

            timespec when = {};
            clock_gettime(CLOCK_MONOTONIC, &when);
            output->events.present.emit(IOutput::SPresentEvent{
                .presented = false,
                .when      = &when,
                .commitID  = COMMIT_ID,
            });
        }
    }
}

void Aquamarine::SDRMConnector::setCRTC(SP<SDRMCRTC> newCRTC) {
    if (crtc == newCRTC)
        return;

    if (output)
        backend->cancelAsyncOutput(output.get(), true);
    invalidateFrame();

    crtc = newCRTC;
}

void Aquamarine::SDRMConnector::onPresent(bool primary, bool cursor, DRMFBList* deferred) {
    DRMFBList  retired;
    const auto presentPlane = [&retired](const SP<SDRMPlane>& plane) {
        if (!plane || !plane->backSet)
            return;

        if (plane->front && plane->front != plane->back)
            retired.emplace_back(std::move(plane->front));
        plane->front   = plane->back;
        plane->backSet = false;
        plane->last.reset();
    };

    if (primary)
        presentPlane(crtc->primary);
    if (cursor)
        presentPlane(crtc->cursor);

    if (deferred) {
        for (auto& fb : retired)
            deferred->emplace_back(std::move(fb));
        return;
    }

    for (const auto& fb : retired)
        releaseFBBuffer(fb);
}

Aquamarine::CDRMOutput::~CDRMOutput() {
    if (backend)
        backend->cancelAsyncOutput(this);
    if (backend && backend->backend)
        backend->backend->removeIdleEvent(frameIdle);
    connector->sched.onFrameComplete();
    connector->sched.setFrameScheduled(false);
}

void Aquamarine::CDRMOutput::releaseMgpuResources() {
    mgpu.swapchain.reset();
    mgpu.cursorSwapchain.reset();

    if (swapchain) {
        auto options   = swapchain->currentOptions();
        options.length = 0;
        swapchain->reconfigure(options);
    }
}

bool Aquamarine::CDRMOutput::commit() {
    if (!connector->crtc)
        return commitState();

    const auto QUEUE_KEY = connector->crtc->id;
    if (pendingAsyncCommit)
        backend->cancelAsyncOutput(this, true);
    if (pendingAsyncCommit)
        return false;
    if (!backend->pauseCommitQueue(QUEUE_KEY))
        return false;
    CScopeGuard resume([this, QUEUE_KEY] { backend->resumeCommitQueue(QUEUE_KEY); });
    return commitState();
}

bool Aquamarine::CDRMOutput::test() {
    if (!connector->crtc)
        return commitState(true);

    const auto QUEUE_KEY = connector->crtc->id;
    if (pendingAsyncCommit)
        backend->cancelAsyncOutput(this, true);
    if (pendingAsyncCommit)
        return false;
    if (!backend->pauseCommitQueue(QUEUE_KEY))
        return false;
    CScopeGuard resume([this, QUEUE_KEY] { backend->resumeCommitQueue(QUEUE_KEY); });
    return commitState(true);
}

void Aquamarine::CDRMOutput::setCursorVisible(bool visible) {
    cursorVisible = visible;
    scheduleFrame(AQ_SCHEDULE_CURSOR_VISIBLE);
}

bool Aquamarine::CDRMOutput::prepareAsyncCommitData(const COutputState::CSnapshot& snapshot, SDRMConnectorCommitData& data, Hyprutils::OS::CFileDescriptor& mgpuFence,
                                                    bool& mgpuAcquired, bool& requiresSync) {
    const auto& STATE = snapshot.state();

    data.outputState   = STATE;
    data.cursorPos     = cursorPos;
    data.cursorHotspot = cursorHotspot;
    data.cursorVisible = cursorVisible;

    SP<CDRMFB> drmFB;
    if (backend->shouldBlit()) {
        if (!backend->rendererState.renderer) {
            backend->log(AQ_LOG_DEBUG, "drm: No renderer attached to backend when required for asynchronous blitting, initializing");
            if (!backend->initMgpu() || !backend->rendererState.renderer || !backend->rendererState.allocator) {
                backend->log(AQ_LOG_ERROR, "drm: Failed to initialize renderer backend for asynchronous blitting");
                return false;
            }
        }

        if (!mgpu.swapchain)
            mgpu.swapchain = CSwapchain::create(backend->rendererState.allocator, backend.lock());

        auto       options = swapchain ? swapchain->currentOptions() : mgpu.swapchain->currentOptions();
        const auto ATTRS   = snapshot.state().buffer->dmabuf();
        options.size       = STATE.buffer->size;
        if (options.format == DRM_FORMAT_INVALID)
            options.format = ATTRS.format;
        options.multigpu = false;
        options.cursor   = false;
        options.scanout  = true;
        if (options.length == 0)
            options.length = 2;

        const auto& CURRENT = mgpu.swapchain->currentOptions();
        if (CURRENT.length > 0 &&
            (CURRENT.length != options.length || CURRENT.size != options.size || CURRENT.format != options.format || CURRENT.multigpu != options.multigpu ||
             CURRENT.cursor != options.cursor || CURRENT.scanout != options.scanout)) {
            requiresSync = true;
            return false;
        }

        if (!mgpu.swapchain->reconfigure(options)) {
            backend->log(AQ_LOG_ERROR, "drm: Asynchronous commit requires blit, but the mGPU swapchain failed reconfiguring");
            return false;
        }

        const auto NEW_BUFFER = mgpu.swapchain->next(nullptr);
        if (!NEW_BUFFER) {
            backend->log(AQ_LOG_ERROR, "drm: Asynchronous commit requires blit, but the mGPU swapchain has no buffer");
            return false;
        }
        mgpuAcquired = true;

        SP<CDRMRenderer> primaryRenderer;
        if (backend->primary)
            primaryRenderer = backend->primary->rendererState.renderer;
        const auto BLIT = backend->rendererState.renderer->blit(STATE.buffer, NEW_BUFFER, primaryRenderer,
                                                                (STATE.committed & COutputState::AQ_OUTPUT_STATE_EXPLICIT_IN_FENCE) ? STATE.explicitInFence : -1);
        if (!BLIT.success) {
            backend->log(AQ_LOG_ERROR, "drm: Asynchronous commit requires blit, but blitting failed");
            return false;
        }

        static const bool NO_EXPLICIT = envEnabled("AQ_MGPU_NO_EXPLICIT");
        if (!BLIT.syncFD || NO_EXPLICIT || !supportsExplicit) {
            requiresSync = true;
            return false;
        }

        const int DUPLICATED_FENCE = fcntl(*BLIT.syncFD, F_DUPFD_CLOEXEC, 0);
        if (DUPLICATED_FENCE < 0) {
            backend->log(AQ_LOG_ERROR, std::format("drm: Failed to duplicate asynchronous mGPU fence: {}", strerror(errno)));
            return false;
        }

        mgpuFence                        = Hyprutils::OS::CFileDescriptor{DUPLICATED_FENCE};
        data.outputState.explicitInFence = mgpuFence.get();

        drmFB = CDRMFB::create(NEW_BUFFER, backend, nullptr);
    } else
        drmFB = CDRMFB::create(STATE.buffer, backend, nullptr);

    if (!drmFB || drmFB->dead) {
        backend->log(AQ_LOG_ERROR, "drm: Asynchronous commit buffer failed to import to KMS");
        return false;
    }

    data.mainFB = drmFB;
    if (connector->crtc->pendingCursor)
        data.cursorFB = connector->crtc->pendingCursor;
    else if (connector->crtc->cursor)
        data.cursorFB = connector->crtc->cursor->front;

    if (data.cursorFB && (data.cursorFB->dead || data.cursorFB->buffer->dmabuf().modifier == DRM_FORMAT_MOD_INVALID))
        data.cursorFB.reset();

    const auto MODE = STATE.mode ? STATE.mode : STATE.customMode;
    if (!MODE)
        return false;

    data.blocking  = false;
    data.modeset   = false;
    data.test      = false;
    data.enabled   = true;
    data.committed = STATE.committed;
    if (mgpuFence.isValid())
        data.committed |= COutputState::AQ_OUTPUT_STATE_EXPLICIT_IN_FENCE;
    data.flags = DRM_MODE_PAGE_FLIP_EVENT;
    if (STATE.presentationMode == AQ_OUTPUT_PRESENTATION_IMMEDIATE)
        data.flags |= DRM_MODE_PAGE_FLIP_ASYNC;
    if (STATE.committed & COutputState::AQ_OUTPUT_STATE_DAMAGE)
        data.damage = STATE.damage.copy();
    if (MODE->modeInfo)
        data.modeInfo = *MODE->modeInfo;
    else
        data.calculateMode(connector);

    return true;
}

bool Aquamarine::CDRMOutput::commitState(bool onlyTest) {
    if (!backend->backend->session->active) {
        backend->backend->log(AQ_LOG_ERROR, "drm: Session inactive");
        return false;
    }

    if (!connector->crtc) {
        backend->backend->log(AQ_LOG_ERROR, "drm: No CRTC attached to output");
        return false;
    }

    if (connector->status != DRM_MODE_CONNECTED || connector->output != self.lock()) {
        backend->backend->log(AQ_LOG_ERROR, "drm: Cannot commit a disconnected output");
        return false;
    }

    const auto     SNAPSHOT  = state->snapshot();
    const auto&    STATE     = SNAPSHOT.state();
    const uint32_t COMMITTED = STATE.committed;

    if (SNAPSHOT.error()) {
        backend->backend->log(AQ_LOG_ERROR, std::format("drm: Failed to duplicate explicit input fence: {}", strerror(SNAPSHOT.error())));
        return false;
    }

    if ((COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_ENABLED) && STATE.enabled) {
        if (!STATE.mode && !STATE.customMode) {
            backend->backend->log(AQ_LOG_ERROR, "drm: No mode on enable commit");
            return false;
        }
    }

    if (COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_FORMAT) {
        // verify the format is valid for the primary plane
        bool ok = false;
        for (auto const& f : getRenderFormats()) {
            if (f.drmFormat == STATE.drmFormat) {
                ok = true;
                break;
            }
        }

        if (!ok) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Selected format is not supported by the primary KMS plane");
            return false;
        }
    }

    if (STATE.enabled && STATE.drmFormat == DRM_FORMAT_INVALID) {
        backend->backend->log(AQ_LOG_ERROR, "drm: No format for output");
        return false;
    }

    if (STATE.adaptiveSync && !connector->canDoVrr) {
        backend->backend->log(AQ_LOG_ERROR, "drm: No Adaptive sync support for output");
        return false;
    }

    if (STATE.presentationMode == AQ_OUTPUT_PRESENTATION_IMMEDIATE && !backend->drmProps.supportsAsyncCommit) {
        backend->backend->log(AQ_LOG_ERROR, "drm: No Immediate presentation support in the backend");
        return false;
    }

    if ((COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_BUFFER) && !STATE.buffer) {
        backend->backend->log(AQ_LOG_ERROR, "drm: No buffer committed");
        return false;
    }

    if ((COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_BUFFER) && STATE.buffer->attachments.has<CDRMBufferUnimportable>()) {
        TRACE(backend->backend->log(AQ_LOG_TRACE, "drm: Cannot commit a KMS-unimportable buffer."));
        return false;
    }

    // If we are changing the rendering format, we may need to reconfigure the output (aka modeset)
    // which may result in some glitches
    const bool NEEDS_RECONFIG = SNAPSHOT.needsReconfig();

    const bool BLOCKING = NEEDS_RECONFIG || !(COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_BUFFER);

    const auto MODE = STATE.mode ? STATE.mode : STATE.customMode;

    if (!MODE) // modeless commits are invalid
        return false;

    uint32_t flags = 0;

    if (!onlyTest) {
        if (NEEDS_RECONFIG) {
            if (STATE.enabled)
                backend->backend->log(AQ_LOG_DEBUG,
                                      std::format("drm: Modesetting {} with {}x{}@{:.2f}Hz", name, (int)MODE->pixelSize.x, (int)MODE->pixelSize.y, MODE->refreshRate / 1000.F));
            else
                backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Disabling output {}", name));
        }

        if (STATE.enabled && NEEDS_RECONFIG && connector->sched.frameInFlight()) {
            // ALLOW_MODESET resets the CRTC, so the in-flight flip is either cancelled or
            // superseded by this commit. drop it either way; whatever event still arrives no
            // longer matches the crtcs id and is discarded.
            backend->backend->log(AQ_LOG_DEBUG, std::format("drm: page-flip on {} cancelled by modeset, clearing flip state", name));
            connector->invalidateFrame();
        }

        if (STATE.enabled && (COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_BUFFER) && connector->sched.frameInFlight()) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Cannot commit when a page-flip is awaiting");
            return false;
        }

        if (STATE.enabled && (COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_BUFFER))
            flags |= DRM_MODE_PAGE_FLIP_EVENT;
        if (STATE.presentationMode == AQ_OUTPUT_PRESENTATION_IMMEDIATE && (COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_BUFFER))
            flags |= DRM_MODE_PAGE_FLIP_ASYNC;
    }

    // we can't go further without a blit
    if (backend->primary && onlyTest)
        return true;

    SDRMConnectorCommitData data;
    data.outputState   = STATE;
    data.cursorPos     = cursorPos;
    data.cursorHotspot = cursorHotspot;
    data.cursorVisible = cursorVisible;

    // A commit that carries no new buffer has nothing to blit: STATE.buffer is the one we already copied
    SP<CDRMFB> blittedFB;
    if (backend->shouldBlit() && !(COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_BUFFER)) {
        const auto& LAST = connector->crtc->primary->back ? connector->crtc->primary->back : connector->crtc->primary->front;
        if (LAST && !LAST->dead)
            blittedFB = LAST;
    }

    if (STATE.buffer && STATE.enabled) {
        TRACE(backend->backend->log(AQ_LOG_TRACE, "drm: Committed a buffer, updating state"));

        SP<CDRMFB> drmFB;

        if (blittedFB)
            drmFB = blittedFB;
        else if (backend->shouldBlit()) {
            if (!backend->rendererState.renderer) {
                backend->backend->log(AQ_LOG_DEBUG, "drm: No renderer attached to backend when required for blitting, initializing");
                if (!backend->initMgpu() || !backend->rendererState.renderer || !backend->rendererState.allocator) {
                    backend->backend->log(AQ_LOG_ERROR, "drm: Failed to initialize renderer backend for blitting");
                    return false;
                }
            }

            TRACE(backend->backend->log(AQ_LOG_TRACE, "drm: Backend requires blit, blitting"));

            if (!mgpu.swapchain) {
                TRACE(backend->backend->log(AQ_LOG_TRACE, "drm: No swapchain for blit, creating"));
                mgpu.swapchain = CSwapchain::create(backend->rendererState.allocator, backend.lock());
            }

            auto OPTIONS = swapchain->currentOptions();
            auto bufDma  = STATE.buffer->dmabuf();
            OPTIONS.size = STATE.buffer->size;
            if (OPTIONS.format == DRM_FORMAT_INVALID)
                OPTIONS.format = bufDma.format;
            OPTIONS.multigpu = false; // this is not a shared swapchain, and additionally, don't make it linear, nvidia would be mad
            OPTIONS.cursor   = false;
            OPTIONS.scanout  = true;
            if (OPTIONS.length == 0) // releaseMgpuResources() cleared the swapchain and we committed without updating it.
                OPTIONS.length = 2;
            if (!mgpu.swapchain->reconfigure(OPTIONS)) {
                backend->backend->log(AQ_LOG_ERROR, "drm: Backend requires blit, but the mgpu swapchain failed reconfiguring");
                return false;
            }

            auto NEWAQBUF = mgpu.swapchain->next(nullptr);
            if (!NEWAQBUF) {
                backend->backend->log(AQ_LOG_ERROR, "drm: Backend requires blit, but the mgpu swapchain has no buffer");
                return false;
            }

            SP<Aquamarine::CDRMRenderer> primaryRenderer;
            if (backend->primary)
                primaryRenderer = backend->primary->rendererState.renderer;
            auto blitResult = backend->rendererState.renderer->blit(
                STATE.buffer, NEWAQBUF, primaryRenderer, (COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_EXPLICIT_IN_FENCE) ? STATE.explicitInFence : -1);
            if (!blitResult.success) {
                backend->backend->log(AQ_LOG_ERROR, "drm: Backend requires blit, but blit failed");
                return false;
            }

            // replace the explicit in fence if the blitting backend returned one, otherwise discard old. Passed fence from the client is wrong.
            // if the commit doesn't have an explicit fence, don't use the one we created, just fallback to implicit
            static auto NO_EXPLICIT = envEnabled("AQ_MGPU_NO_EXPLICIT");
            if (blitResult.syncFD.has_value() && !NO_EXPLICIT && (COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_EXPLICIT_IN_FENCE))
                data.outputState.explicitInFence = blitResult.syncFD.value();
            else
                data.outputState.explicitInFence = -1;

            drmFB = CDRMFB::create(NEWAQBUF, backend, nullptr); // will return attachment if present
        } else
            drmFB = CDRMFB::create(STATE.buffer, backend, nullptr); // will return attachment if present

        if (!drmFB) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Buffer failed to import to KMS");
            return false;
        }

        if (drmFB->dead) {
            backend->backend->log(AQ_LOG_ERROR, "drm: KMS buffer is dead?!");
            return false;
        }

        data.mainFB = drmFB;
    }

    // sometimes, our consumer could f up the swapchain format and change it without the state changing
    bool formatMismatch = false;
    if (data.mainFB) {
        if (const auto params = data.mainFB->buffer->dmabuf(); params.success && params.format != STATE.drmFormat) {
            // formats mismatch. Update the state format and roll with it
            backend->backend->log(AQ_LOG_WARNING,
                                  std::format("drm: Formats mismatch in commit, buffer is {} but output is set to {}. Modesetting to {}", fourccToName(params.format),
                                              fourccToName(STATE.drmFormat), fourccToName(params.format)));
            data.outputState.drmFormat = params.format;
            formatMismatch             = true;
            // TODO: reject if tearing? We will miss a frame event!
            flags &= ~DRM_MODE_PAGE_FLIP_ASYNC; // we cannot modeset with async pf
        }
    }

    if (connector->crtc->pendingCursor)
        data.cursorFB = connector->crtc->pendingCursor;
    else if (connector->crtc->cursor)
        data.cursorFB = connector->crtc->cursor->front;

    if (data.cursorFB) {
        // verify cursor format. This might be wrong on NVIDIA where linear buffers
        // fail to be created from gbm
        // TODO: add an API to detect this and request drm_dumb linear buffers. Or do something,
        // idk
        if (data.cursorFB->dead || data.cursorFB->buffer->dmabuf().modifier == DRM_FORMAT_MOD_INVALID) {
            TRACE(backend->backend->log(AQ_LOG_TRACE, "drm: Dropping invalid buffer for cursor plane"));
            data.cursorFB = nullptr;
        }
    }

    if (COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_HDR)
        data.hdrMetadata = STATE.hdrMetadata;

    data.blocking  = BLOCKING || formatMismatch;
    data.modeset   = NEEDS_RECONFIG || lastCommitNoBuffer || formatMismatch;
    data.flags     = flags;
    data.test      = onlyTest;
    data.enabled   = STATE.enabled;
    data.committed = COMMITTED;
    if (COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_DAMAGE)
        data.damage = STATE.damage.copy();
    if (MODE->modeInfo.has_value())
        data.modeInfo = *MODE->modeInfo;
    else
        data.calculateMode(connector);

    // A modeset commit must carry a buffer sized for the new mode: the atomic path
    // derives the plane geometry from it (and legacy SetCrtc has the same requirement),
    // so drivers without plane scaling (e.g. virtio-gpu) reject stale-size buffers,
    // making larger modes unreachable.
    // If the consumer already reconfigured the swapchain to the new size, attach a fresh buffer from it.
    bool acquiredModesetBuffer = false;
    if (data.modeset && data.mainFB && swapchain && data.mainFB->buffer->size != MODE->pixelSize && swapchain->currentOptions().size == MODE->pixelSize) {
        if (auto newBuf = swapchain->next(nullptr); newBuf) {
            if (auto newFB = CDRMFB::create(newBuf, backend, nullptr); newFB && !newFB->dead) {
                data.mainFB           = newFB;
                acquiredModesetBuffer = true;
            } else
                swapchain->rollback();
        }
    }

    // Re-send CTM when it changes, when preserving a non-identity CTM over a modeset,
    // or when we need one identity blob to clear unknown kernel state from before us.
    // Once a real CTM commit succeeds, ordinary identity modesets can skip the blob.
    if (shouldSubmitCTM(connector, STATE, data.modeset))
        data.ctm = STATE.ctm;

    bool ok = connector->commitState(data);

    // a buffer acquired only to validate/attempt the modeset isn't consumed by the
    // consumer: rewind the swapchain so the acquire cursor stays in sync with it.
    if (acquiredModesetBuffer && (onlyTest || !ok))
        swapchain->rollback();

    if (!ok && !data.modeset && !connector->commitTainted) {
        // attempt to re-modeset, however, flip a tainted flag if the modesetting fails
        // to avoid doing this over and over.
        data.modeset  = true;
        data.blocking = true;
        data.flags    = onlyTest ? 0 : DRM_MODE_PAGE_FLIP_EVENT;
        ok            = connector->commitState(data);

        if (!ok)
            connector->commitTainted = true;
    }

    // the mode may be fine but not the buffer layout: drivers can reject at commit time
    // modifiers they advertise in IN_FORMATS (e.g. i915 gen9 with Y-tiled/CCS above 4096px).
    // retry with linear buffers and keep them for this output if that works.
    if (!ok && data.modeset && !linearOnly && data.mainFB && swapchain && !backend->shouldBlit() && swapchain->currentOptions().scanoutOutput.get() == this) {
        const auto MOD = data.mainFB->buffer->dmabuf().modifier;
        if (MOD != DRM_FORMAT_MOD_INVALID && MOD != DRM_FORMAT_MOD_LINEAR) {
            backend->backend->log(AQ_LOG_WARNING, std::format("drm: Modeset on {} rejected with modifier {}, retrying with linear buffers", name, drmModifierToName(MOD)));

            const auto OPTIONS = swapchain->currentOptions();
            auto       CLEAR   = OPTIONS;
            CLEAR.length       = 0; // clears the buffers but keeps scanoutOutput, which the allocator reads on refill

            linearOnly = true;
            swapchain->reconfigure(CLEAR);

            if (swapchain->reconfigure(OPTIONS)) {
                if (auto newBuf = swapchain->next(nullptr); newBuf) {
                    if (auto newFB = CDRMFB::create(newBuf, backend, nullptr); newFB && !newFB->dead) {
                        data.mainFB = newFB;
                        ok          = connector->commitState(data);

                        if (ok)
                            state->setBuffer(newBuf); // the consumer still holds the rejected buffer, don't let it re-commit it
                        if (onlyTest || !ok)
                            swapchain->rollback();
                    }
                }
            }

            if (!ok) {
                // linear didn't help, restore the previous buffers
                linearOnly = false;
                swapchain->reconfigure(CLEAR);
                swapchain->reconfigure(OPTIONS);
                backend->backend->log(AQ_LOG_ERROR, std::format("drm: Linear retry on {} failed", name));
            }
        }
    }

    if (onlyTest || !ok)
        return ok;

    events.commit.emit();
    state->consume(SNAPSHOT);

    lastCommitNoBuffer = !data.mainFB;
    needsFrame         = false;

    if (ok)
        connector->commitTainted = false;

    if (data.flags & DRM_MODE_PAGE_FLIP_ASYNC) {
        // for tearing commits, we will send presentation feedback instantly, and rotate
        // drm framebuffers to properly send backendRelease events.
        // the last FB should already be gone from KMS because it's been immediately replaced

        // no completion and no vsync, because tearing
        uint32_t flags = IOutput::AQ_OUTPUT_PRESENT_HW_CLOCK | IOutput::AQ_OUTPUT_PRESENT_ZEROCOPY;

        timespec presented;
        clock_gettime(CLOCK_MONOTONIC, &presented);

        connector->output->events.present.emit(IOutput::SPresentEvent{
            .presented = backend->sessionActive(),
            .when      = &presented,
            .seq       = 0, /* unknown sequence for tearing */
            .refresh   = (int)(connector->refresh ? (1000000000000LL / connector->refresh) : 0),
            .flags     = flags,
        });

        connector->onPresent();
    }

    return ok;
}

SP<IBackendImplementation> Aquamarine::CDRMOutput::getBackend() {
    return backend.lock();
}

bool Aquamarine::CDRMOutput::setCursor(SP<IBuffer> buffer, const Vector2D& hotspot) {
    if (!connector->crtc)
        return false;

    // already hidden
    if (!buffer && !cursorVisible)
        return true;

    state->markCommitted(COutputState::AQ_OUTPUT_STATE_CURSOR_SHAPE);
    if (!buffer)
        setCursorVisible(false);
    else {
        auto bufferType = buffer->type();

        if (!buffer->good()) {
            backend->backend->log(AQ_LOG_ERROR, "drm: bad buffer passed to setCursor");
            return false;
        }

        if ((bufferType == eBufferType::BUFFER_TYPE_SHM && !buffer->shm().success) || (bufferType == eBufferType::BUFFER_TYPE_DMABUF && !buffer->dmabuf().success)) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Invalid buffer passed to setCursor");
            return false;
        }

        SP<CDRMFB> fb;

        if (backend->primary) {
            TRACE(backend->backend->log(AQ_LOG_TRACE, "drm: Backend requires cursor blit, blitting"));

            if (!backend->rendererState.renderer || !backend->rendererState.allocator) {
                backend->backend->log(AQ_LOG_DEBUG, "drm: No renderer attached to backend when required for cursor blitting, initializing");
                if (!backend->initMgpu() || !backend->rendererState.renderer || !backend->rendererState.allocator) {
                    backend->backend->log(AQ_LOG_ERROR, "drm: Failed to initialize renderer backend for cursor blitting");
                    return false;
                }
            }

            // TODO: will this not implode on drm_dumb?!

            if (!mgpu.cursorSwapchain) {
                TRACE(backend->backend->log(AQ_LOG_TRACE, "drm: No cursorSwapchain for blit, creating"));
                mgpu.cursorSwapchain = CSwapchain::create(backend->rendererState.allocator, backend.lock());
            }

            const auto FORMAT = bufferType == eBufferType::BUFFER_TYPE_SHM ? buffer->shm().format : buffer->dmabuf().format;
            const auto SIZE   = bufferType == eBufferType::BUFFER_TYPE_SHM ? buffer->shm().size : buffer->dmabuf().size;

            auto       OPTIONS = mgpu.cursorSwapchain->currentOptions();
            OPTIONS.multigpu   = false;
            OPTIONS.scanout    = true;
            OPTIONS.cursor     = true;
            OPTIONS.format     = FORMAT;
            OPTIONS.size       = SIZE;
            OPTIONS.length     = 2;

            if (!mgpu.cursorSwapchain->reconfigure(OPTIONS)) {
                backend->backend->log(AQ_LOG_ERROR, "drm: Backend requires blit, but the mgpu cursorSwapchain failed reconfiguring");
                return false;
            }

            auto NEWAQBUF = mgpu.cursorSwapchain->next(nullptr);
            if (!NEWAQBUF) {
                backend->backend->log(AQ_LOG_ERROR, "drm: Backend requires blit, but the mgpu cursorSwapchain has no buffer");
                return false;
            }

            SP<Aquamarine::CDRMRenderer> primaryRenderer;
            if (backend->primary)
                primaryRenderer = backend->primary->rendererState.renderer;
            if (!backend->rendererState.renderer->blit(buffer, NEWAQBUF, primaryRenderer).success) {
                backend->backend->log(AQ_LOG_ERROR, "drm: Backend requires blit, but cursor blit failed");
                return false;
            }

            fb = CDRMFB::create(NEWAQBUF, backend, nullptr); // will return attachment if present
        } else
            fb = CDRMFB::create(buffer, backend, nullptr);

        if (!fb) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Cursor buffer failed to import to KMS");
            return false;
        }

        cursorHotspot = hotspot;
        if (cursorMailbox)
            cursorMailbox->store(cursorPos);

        backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Cursor buffer imported into KMS with id {}", fb->id));

        connector->crtc->pendingCursor = fb;

        cursorVisible = true;
    }

    scheduleFrame(AQ_SCHEDULE_CURSOR_SHAPE);
    return true;
}

void Aquamarine::CDRMOutput::moveCursor(const Vector2D& coord, bool skipSchedule) {
    cursorPos = coord;
    if (cursorMailbox)
        cursorMailbox->store(coord);
    // cursorVisible = true;
    // if (!skipSchedule)
    state->markCommitted(COutputState::AQ_OUTPUT_STATE_CURSOR_POS);

    backend->impl->moveCursor(connector, skipSchedule);
}

void Aquamarine::CDRMOutput::scheduleFrame(const scheduleFrameReason reason) {
    TRACE(backend->backend->log(AQ_LOG_TRACE,
                                std::format("CDRMOutput::scheduleFrame: reason {}, needsFrame {}, isPageFlipPending {}, frameEventScheduled {}", (uint32_t)reason, needsFrame,
                                            connector->sched.frameInFlight(), connector->sched.frameScheduled())));
    needsFrame = true;

    if (!enabledState)
        return;

    // a scheduleFrame mid frame, reschedule one more.
    if (connector->sched.frameRunning()) {
        connector->sched.requestReschedule();
        return;
    }

    if (!connector->sched.canSchedule())
        return;

    connector->sched.setFrameScheduled(true);

    if (!frameIdle) {
        frameIdle = makeShared<std::function<void(void)>>([this, self_ = self, backend_ = backend]() {
            if (!self)
                return;

            connector->sched.setFrameScheduled(false);
            if (connector->sched.frameInFlight() || connector->sched.frameRunning())
                return;

            CFrameRunningGuard frameRunning(connector->sched);
            connector->sched.frameReady.emit();

            // above frame scheduled, and then committed, remove the idle frame. the pageflip will emit the frame.
            if (backend_ && backend_->backend && connector->sched.frameScheduled() && connector->sched.frameInFlight()) {
                backend_->backend->removeIdleEvent(frameIdle);
                connector->sched.setFrameScheduled(false);
            }
        });
    }

    backend->backend->addIdleEvent(frameIdle);
}

Vector2D Aquamarine::CDRMOutput::cursorPlaneSize() {
    return backend->drmProps.cursorSize;
}

bool Aquamarine::CDRMOutput::hasCursorPlane() const {
    return connector->status == DRM_MODE_CONNECTED && connector->crtc && connector->crtc->cursor && !connector->crtc->cursor->formats.empty();
}

std::optional<std::chrono::steady_clock::time_point> Aquamarine::CDRMOutput::nextVBlank() const {
    const auto BACKEND = backend.lock();
    if (!BACKEND || !BACKEND->sessionActive() || connector->status != DRM_MODE_CONNECTED || !connector->crtc || !enabledState || connector->refresh <= 0 ||
        (lease && lease->active))
        return std::nullopt;

    const auto& STATE = state->state();

    if (state->needsReconfig() || (STATE.committed & (COutputState::AQ_OUTPUT_STATE_ADAPTIVE_SYNC | COutputState::AQ_OUTPUT_STATE_PRESENTATION_MODE)) || !STATE.enabled ||
        STATE.adaptiveSync || STATE.presentationMode != AQ_OUTPUT_PRESENTATION_VSYNC)
        return std::nullopt;

    const auto REFRESH_PERIOD = std::chrono::nanoseconds{1'000'000'000'000LL / connector->refresh};

    for (size_t attempt = 0; attempt < 2; ++attempt) {
        uint64_t sequence = 0, lastVblankNs = 0;
        if (drmCrtcGetSequence(BACKEND->drmFD(), connector->crtc->id, &sequence, &lastVblankNs))
            return std::nullopt;

        if (BACKEND->gpuDriver() == AQ_BACKEND_GPU_DRIVER_NVIDIA && sequence == 0)
            return std::nullopt;

        const auto STEADY_BEFORE = std::chrono::steady_clock::now();
        timespec   monotonicNow  = {};
        if (clock_gettime(CLOCK_MONOTONIC, &monotonicNow))
            return std::nullopt;
        const auto STEADY_AFTER = std::chrono::steady_clock::now();

        if (lastVblankNs > sc<uint64_t>(std::chrono::nanoseconds::max().count()))
            return std::nullopt;

        const auto MONOTONIC_NOW = std::chrono::seconds{monotonicNow.tv_sec} + std::chrono::nanoseconds{monotonicNow.tv_nsec};
        const auto NEXT          = OutputTiming::predictNextVBlank(std::chrono::nanoseconds{lastVblankNs}, MONOTONIC_NOW, REFRESH_PERIOD);
        if (!NEXT)
            continue;

        const auto RESULT = OutputTiming::toSteadyClock(*NEXT, MONOTONIC_NOW, STEADY_BEFORE, STEADY_AFTER);
        if (RESULT)
            return RESULT;
    }

    return std::nullopt;
}

uint32_t Aquamarine::CDRMOutput::commitCapabilities() const {
    if (!backend || !backend->commitThread || !backend->atomic)
        return 0;

    uint32_t capabilities = AQ_OUTPUT_COMMIT_CAPABILITY_QUEUED | AQ_OUTPUT_COMMIT_CAPABILITY_TIMED;
    if (connector->canDoVrr)
        capabilities |= AQ_OUTPUT_COMMIT_CAPABILITY_VRR;
    if (backend->drmProps.supportsAsyncCommit)
        capabilities |= AQ_OUTPUT_COMMIT_CAPABILITY_TEARING;
    if (connector->crtc && connector->crtc->cursor)
        capabilities |= AQ_OUTPUT_COMMIT_CAPABILITY_LATE_CURSOR;
    return capabilities;
}

Aquamarine::IOutput::SCommitSubmission Aquamarine::CDRMOutput::commitAsync(const SCommitOptions& options) {
    const auto CAPABILITIES = commitCapabilities();
    if (!(CAPABILITIES & AQ_OUTPUT_COMMIT_CAPABILITY_QUEUED))
        return {.error = ENOTSUP};
    if (options.targetPresentation && !(CAPABILITIES & AQ_OUTPUT_COMMIT_CAPABILITY_TIMED))
        return {.error = ENOTSUP};
    if (options.lateCursor && !(CAPABILITIES & AQ_OUTPUT_COMMIT_CAPABILITY_LATE_CURSOR))
        return {.error = ENOTSUP};

    if (!backend->sessionActive() || connector->status != DRM_MODE_CONNECTED || connector->output != self.lock() || !connector->crtc)
        return {.error = ENODEV};

    auto SNAPSHOT = state->snapshot();
    if (SNAPSHOT.error())
        return {.error = SNAPSHOT.error()};

    const auto&        STATE     = SNAPSHOT.state();
    const auto         COMMITTED = STATE.committed;
    constexpr uint32_t ASYNC_PROPERTIES =
        COutputState::AQ_OUTPUT_STATE_BUFFER | COutputState::AQ_OUTPUT_STATE_DAMAGE | COutputState::AQ_OUTPUT_STATE_EXPLICIT_IN_FENCE | COutputState::AQ_OUTPUT_STATE_CURSOR_POS;

    if (!(COMMITTED & COutputState::AQ_OUTPUT_STATE_BUFFER) || (COMMITTED & ~ASYNC_PROPERTIES) || SNAPSHOT.needsReconfig() || lastCommitNoBuffer)
        return {.error = ENOTSUP};
    if (!STATE.buffer || !STATE.enabled || STATE.drmFormat == DRM_FORMAT_INVALID)
        return {.error = EINVAL};
    if (STATE.buffer->attachments.has<CDRMBufferUnimportable>())
        return {.error = EINVAL};
    if (STATE.adaptiveSync && !(CAPABILITIES & AQ_OUTPUT_COMMIT_CAPABILITY_VRR))
        return {.error = ENOTSUP};
    if (STATE.presentationMode == AQ_OUTPUT_PRESENTATION_IMMEDIATE && !(CAPABILITIES & AQ_OUTPUT_COMMIT_CAPABILITY_TEARING))
        return {.error = ENOTSUP};
    if (pendingAsyncCommit || connector->sched.frameInFlight())
        return {.error = EBUSY};

    SDRMConnectorCommitData        data;
    Hyprutils::OS::CFileDescriptor mgpuFence;
    bool                           mgpuAcquired = false, requiresSync = false;
    CScopeGuard                    rollbackMgpu([this, &mgpuAcquired] {
        if (mgpuAcquired && mgpu.swapchain)
            mgpu.swapchain->rollback();
    });
    if (!prepareAsyncCommitData(SNAPSHOT, data, mgpuFence, mgpuAcquired, requiresSync))
        return {.error = requiresSync ? ENOTSUP : EINVAL};

    const auto MAIN_BUFFER = data.mainFB ? data.mainFB->buffer.lock() : nullptr;
    const auto ATTRS       = MAIN_BUFFER ? MAIN_BUFFER->dmabuf() : SDMABUFAttrs{};
    if (ATTRS.success && ATTRS.format != STATE.drmFormat)
        return {.error = ENOTSUP};

    if (options.lateCursor && cursorVisible && connector->crtc->cursor)
        data.committed |= COutputState::AQ_OUTPUT_STATE_CURSOR_POS;

    auto* atomicImpl = dynamic_cast<CDRMAtomicImpl*>(backend->impl.get());
    if (!atomicImpl)
        return {.error = ENOTSUP};

    uint32_t atomicFlags   = 0;
    auto     atomicRequest = atomicImpl->prepareAsync(connector, data, atomicFlags);
    if (!atomicRequest)
        return {.error = EINVAL};

    auto asyncData           = makeUnique<CDRMAsyncCommitData>(std::move(SNAPSHOT));
    asyncData->output        = self.lock();
    asyncData->connector     = connector;
    asyncData->commitData    = std::move(data);
    asyncData->request       = std::move(atomicRequest);
    asyncData->mainBuffer    = asyncData->commitData.mainFB ? asyncData->commitData.mainFB->buffer.lock() : nullptr;
    asyncData->cursorBuffer  = asyncData->commitData.cursorFB ? asyncData->commitData.cursorFB->buffer.lock() : nullptr;
    asyncData->flags         = atomicFlags;
    asyncData->flipID        = backend->nextPageFlipID();
    asyncData->tearing       = asyncData->commitData.flags & DRM_MODE_PAGE_FLIP_ASYNC;
    asyncData->lateCursor    = options.lateCursor;
    asyncData->mgpuFence     = std::move(mgpuFence);
    asyncData->mgpuSwapchain = mgpuAcquired ? mgpu.swapchain : nullptr;
    asyncData->mgpuAcquired  = std::exchange(mgpuAcquired, false);
    asyncData->pinBuffers();

    if (options.lateCursor && cursorVisible && connector->crtc->cursor) {
        asyncData->cursorMailbox = cursorMailbox;
        asyncData->cursorPlaneID = connector->crtc->cursor->id;
        asyncData->cursorXProp   = connector->crtc->cursor->props.values.crtc_x;
        asyncData->cursorYProp   = connector->crtc->cursor->props.values.crtc_y;
        asyncData->cursorHotspot = cursorHotspot;
    }

    auto* asyncDataPtr          = asyncData.get();
    auto  request               = makeUnique<SDRMCommitThreadRequest>();
    request->ownerID            = asyncOwnerID;
    request->queueKey           = connector->crtc->id;
    request->targetPresentation = options.targetPresentation;
    if (options.targetPresentation)
        request->submitAt = *options.targetPresentation - backend->commitLeadTime;
    request->data = std::move(asyncData);

    const auto COMMIT_ID = backend->commitThread->enqueue(std::move(request));
    if (!COMMIT_ID) {
        auto* failedData = dynamic_cast<CDRMAsyncCommitData*>(request->data.get());
        if (failedData) {
            atomicImpl->finalizeAsync(*failedData->request, connector, failedData->commitData, false);
            failedData->rollbackMgpu();
            failedData->releasePins();
        }
        return {.error = ECANCELED};
    }
    const bool TEARING = asyncDataPtr->commitData.flags & DRM_MODE_PAGE_FLIP_ASYNC;
    connector->crtc->armPageFlip(connector, TEARING, asyncDataPtr->flipID, *COMMIT_ID, false);
    pendingAsyncCommit = *COMMIT_ID;
    connector->sched.onFrameSubmitted();
    state->consume(*asyncDataPtr->snapshot);
    lastCommitNoBuffer = false;
    needsFrame         = false;

    asyncCommitEventPending = true;
    ++backend->m_pendingAsyncCommitEvents;
    backend->backend->addIdleEvent(makeShared<std::function<void(void)>>([output = self, drmBackend = backend] {
        if (const auto OUTPUT = output.lock(); OUTPUT && drmBackend)
            drmBackend->emitAsyncCommitEvent(OUTPUT);
    }));

    return {.id = *COMMIT_ID};
}

size_t Aquamarine::CDRMOutput::getGammaSize() {
    if (!backend->atomic) {
        backend->log(AQ_LOG_ERROR, "No support for gamma on the legacy iface");
        return 0;
    }

    if (!connector->crtc) {
        backend->log(AQ_LOG_ERROR, "Can't get gamma size: no crtc");
        return 0;
    }

    uint64_t size = 0;
    if (!getDRMProp(backend->gpu->fd, connector->crtc->id, connector->crtc->props.values.gamma_lut_size, &size)) {
        backend->log(AQ_LOG_ERROR, "Couldn't get the gamma_size prop");
        return 0;
    }

    return size;
}

size_t Aquamarine::CDRMOutput::getDeGammaSize() {
    if (!backend->atomic) {
        backend->log(AQ_LOG_ERROR, "No support for gamma on the legacy iface");
        return 0;
    }

    if (!connector->crtc) {
        backend->log(AQ_LOG_ERROR, "Can't get degamma size: no crtc");
        return 0;
    }

    uint64_t size = 0;
    if (!getDRMProp(backend->gpu->fd, connector->crtc->id, connector->crtc->props.values.degamma_lut_size, &size)) {
        backend->log(AQ_LOG_ERROR, "Couldn't get the degamma_size prop");
        return 0;
    }

    return size;
}

std::vector<SDRMFormat> Aquamarine::CDRMOutput::getRenderFormats() {
    if (!connector->crtc || !connector->crtc->primary || connector->crtc->primary->formats.empty()) {
        backend->log(AQ_LOG_ERROR, "Can't get formats: no crtc");
        return {};
    }

    if (!linearOnly)
        return connector->crtc->primary->formats;

    // only linear (or implicit if the plane doesn't list it), see commitState
    auto fmts = connector->crtc->primary->formats;
    for (auto& f : fmts) {
        const bool HAS_LINEAR = std::ranges::find(f.modifiers, DRM_FORMAT_MOD_LINEAR) != f.modifiers.end();
        f.modifiers           = {HAS_LINEAR ? DRM_FORMAT_MOD_LINEAR : DRM_FORMAT_MOD_INVALID};
    }
    return fmts;
}

bool Aquamarine::CDRMOutput::pendingPageFlip() {
    return connector->sched.frameInFlight();
}

bool Aquamarine::CDRMOutput::pendingIdleFrame() {
    return connector->sched.frameScheduled();
}

int Aquamarine::CDRMOutput::getConnectorID() {
    return connector->id;
}

Aquamarine::CDRMOutput::CDRMOutput(const std::string& name_, Hyprutils::Memory::CWeakPointer<CDRMBackend> backend_, SP<SDRMConnector> connector_) :
    backend(backend_), connector(connector_) {
    name          = name_;
    asyncOwnerID  = backend->nextAsyncOwnerID();
    cursorMailbox = makeAtomicShared<CDRMCursorPositionMailbox>();
    cursorMailbox->store(cursorPos);

    // The scheduler's frameReady signal drives the public events.frame on this output.
    frameReadyListener = connector->sched.frameReady.listen([this]() { events.frame.emit(); });

    // scheduled from inside a running frame, schedule it once the running frame is done.
    rescheduleListener = connector->sched.rescheduleNeeded.listen([this]() { scheduleFrame(AQ_SCHEDULE_NEEDS_FRAME); });
}

SP<CDRMFB> Aquamarine::CDRMFB::create(SP<IBuffer> buffer_, Hyprutils::Memory::CWeakPointer<CDRMBackend> backend_, bool* isNew) {

    SP<CDRMFB> fb;

    if (isNew)
        *isNew = true;

    if (auto at = buffer_->attachments.get<CDRMBufferAttachment>()) {
        fb = at->fb;
        TRACE(backend_->log(AQ_LOG_TRACE, std::format("drm: CDRMFB: buffer has drmfb attachment with fb {:x}", (uintptr_t)fb.get())));
    }

    if (fb) {
        if (isNew)
            *isNew = false;
        return fb;
    }

    fb = SP<CDRMFB>(new CDRMFB(buffer_, backend_));

    if (!fb->id)
        return nullptr;

    buffer_->attachments.add(makeShared<CDRMBufferAttachment>(fb));

    return fb;
}

Aquamarine::CDRMFB::CDRMFB(SP<IBuffer> buffer_, Hyprutils::Memory::CWeakPointer<CDRMBackend> backend_) : buffer(buffer_), backend(backend_) {
    import();
}

void Aquamarine::CDRMFB::import() {
    auto attrs = buffer->dmabuf();
    if (!attrs.success) {
        backend->backend->log(AQ_LOG_ERROR, "drm: Buffer submitted has no dmabuf or a drm handle");
        return;
    }

    if (buffer->attachments.has<CDRMBufferUnimportable>()) {
        backend->backend->log(AQ_LOG_ERROR, "drm: Buffer submitted is unimportable");
        return;
    }

    // TODO: check format
    for (int i = 0; i < attrs.planes; ++i) {
        int ret = drmPrimeFDToHandle(backend->gpu->fd, attrs.fds.at(i), &boHandles[i]);
        if (ret) {
            backend->backend->log(AQ_LOG_ERROR, "drm: drmPrimeFDToHandle failed");
            drop();
            return;
        }

        TRACE(backend->backend->log(AQ_LOG_TRACE, std::format("drm: CDRMFB: plane {} has fd {}, got handle {}", i, attrs.fds.at(i), boHandles.at(i))));
    }

    id = submitBuffer();

    if (!id) {
        backend->backend->log(AQ_LOG_ERROR, "drm: Failed to submit a buffer to KMS");
        buffer->attachments.add(makeShared<CDRMBufferUnimportable>());
        drop();
        return;
    }

    TRACE(backend->backend->log(AQ_LOG_TRACE, std::format("drm: new buffer {}", id)));

    closeHandles();

    listeners.destroyBuffer = buffer->events.destroy.listen([this] {
        drop();
        dead      = true;
        id        = 0;
        boHandles = {0, 0, 0, 0};
    });
}

void Aquamarine::CDRMFB::reimport() {
    drop();
    dropped       = false;
    handlesClosed = false;
    boHandles     = {0, 0, 0, 0};

    import();
}

Aquamarine::CDRMFB::~CDRMFB() {
    drop();
}

void Aquamarine::CDRMFB::closeHandles() {
    if (handlesClosed)
        return;

    handlesClosed = true;

    std::vector<uint32_t> closed;

    for (size_t i = 0; i < 4; ++i) {
        if (boHandles.at(i) == 0)
            continue;

        bool exists = false;
        for (size_t j = 0; j < i; ++j) {
            if (boHandles.at(i) == boHandles.at(j)) {
                exists = true;
                break;
            }
        }
        if (exists)
            continue;

        if (drmCloseBufferHandle(backend->gpu->fd, boHandles.at(i)))
            backend->backend->log(AQ_LOG_ERROR, "drm: drmCloseBufferHandle failed");
    }

    boHandles = {0, 0, 0, 0};
}

void Aquamarine::CDRMFB::drop() {
    if (dropped)
        return;

    dropped = true;

    if (!id)
        return;

    closeHandles();

    TRACE(backend->backend->log(AQ_LOG_TRACE, std::format("drm: dropping buffer {}", id)));

    int ret = drmModeCloseFB(backend->gpu->fd, id);
    if (ret == -EINVAL)
        ret = drmModeRmFB(backend->gpu->fd, id);

    if (ret)
        backend->backend->log(AQ_LOG_ERROR, std::format("drm: Failed to close a buffer: {}", strerror(-ret)));
}

uint32_t Aquamarine::CDRMFB::submitBuffer() {
    uint32_t newID = 0;

    if (!buffer->dmabuf().success)
        return 0;

    auto                    attrs = buffer->dmabuf();
    std::array<uint64_t, 4> mods  = {0, 0, 0, 0};
    for (int i = 0; i < attrs.planes; ++i) {
        mods[i] = attrs.modifier;
    }

    if (backend->drmProps.supportsAddFb2Modifiers && attrs.modifier != DRM_FORMAT_MOD_INVALID) {
        TRACE(backend->backend->log(AQ_LOG_TRACE,
                                    std::format("drm: Using drmModeAddFB2WithModifiers to import buffer into KMS: Size {} with format {} and mod 0x{:x} : {}", attrs.size,
                                                fourccToName(attrs.format), attrs.modifier, drmModifierToName(attrs.modifier))));
        if (drmModeAddFB2WithModifiers(backend->gpu->fd, attrs.size.x, attrs.size.y, attrs.format, boHandles.data(), attrs.strides.data(), attrs.offsets.data(), mods.data(),
                                       &newID, DRM_MODE_FB_MODIFIERS)) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Failed to submit a buffer with drmModeAddFB2WithModifiers");
            return 0;
        }
    } else {
        if (attrs.modifier != DRM_FORMAT_MOD_INVALID && attrs.modifier != DRM_FORMAT_MOD_LINEAR) {
            backend->backend->log(AQ_LOG_ERROR, "drm: drmModeAddFB2WithModifiers unsupported and buffer has explicit modifiers");
            return 0;
        }

        TRACE(backend->backend->log(AQ_LOG_TRACE,
                                    std::format("drm: Using drmModeAddFB2 to import buffer into KMS: Size {} with format {} and mod 0x{:x} : {}", attrs.size,
                                                fourccToName(attrs.format), attrs.modifier, drmModifierToName(attrs.modifier))));

        if (drmModeAddFB2(backend->gpu->fd, attrs.size.x, attrs.size.y, attrs.format, boHandles.data(), attrs.strides.data(), attrs.offsets.data(), &newID, 0)) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Failed to submit a buffer with drmModeAddFB2");
            return 0;
        }
    }

    return newID;
}

void Aquamarine::SDRMConnectorCommitData::calculateMode(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector) {
    if (!connector)
        return;

    const auto& STATE = outputState;
    const auto  MODE  = STATE.mode ? STATE.mode : STATE.customMode;

    if (!MODE) {
        connector->backend->log(AQ_LOG_ERROR, "drm: no mode in calculateMode??");
        return;
    }

    di_cvt_options options = {
        .red_blank_ver = DI_CVT_REDUCED_BLANKING_NONE,
        .h_pixels      = (int)MODE->pixelSize.x,
        .v_lines       = (int)MODE->pixelSize.y,
        .ip_freq_rqd   = MODE->refreshRate ? MODE->refreshRate / 1000.0 : 60.0,
    };
    di_cvt_timing timing;

    di_cvt_compute(&timing, &options);

    uint16_t hsync_start = (int)MODE->pixelSize.x + timing.h_front_porch;
    uint16_t vsync_start = timing.v_lines_rnd + timing.v_front_porch;
    uint16_t hsync_end   = hsync_start + timing.h_sync;
    uint16_t vsync_end   = vsync_start + timing.v_sync;

    modeInfo = drmModeModeInfo{
        .clock       = (uint32_t)std::round(timing.act_pixel_freq * 1000),
        .hdisplay    = (uint16_t)MODE->pixelSize.x,
        .hsync_start = hsync_start,
        .hsync_end   = hsync_end,
        .htotal      = (uint16_t)(hsync_end + timing.h_back_porch),
        .vdisplay    = (uint16_t)timing.v_lines_rnd,
        .vsync_start = vsync_start,
        .vsync_end   = vsync_end,
        .vtotal      = (uint16_t)(vsync_end + timing.v_back_porch),
        .vrefresh    = (uint32_t)std::round(timing.act_frame_rate),
        .flags       = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC,
    };
    snprintf(modeInfo.name, sizeof(modeInfo.name), "%dx%d", (int)MODE->pixelSize.x, (int)MODE->pixelSize.y);

    TRACE(connector->backend->log(AQ_LOG_TRACE,
                                  std::format("drm: calculateMode: modeline dump: {} {} {} {} {} {} {} {} {} {} {}", modeInfo.clock, modeInfo.hdisplay, modeInfo.hsync_start,
                                              modeInfo.hsync_end, modeInfo.htotal, modeInfo.vdisplay, modeInfo.vsync_start, modeInfo.vsync_end, modeInfo.vtotal, modeInfo.vrefresh,
                                              modeInfo.flags)));
}

Aquamarine::CDRMBufferAttachment::CDRMBufferAttachment(SP<CDRMFB> fb_) : fb(fb_) {
    ;
}

SP<CDRMLease> Aquamarine::CDRMLease::create(std::vector<SP<IOutput>> outputs) {
    if (outputs.empty())
        return nullptr;

    if (outputs.at(0)->getBackend()->type() != AQ_BACKEND_DRM)
        return nullptr;

    auto backend = ((CDRMBackend*)outputs.at(0)->getBackend().get())->self.lock();

    for (auto const& o : outputs) {
        if (o->getBackend() != backend) {
            backend->log(AQ_LOG_ERROR, "drm lease: Mismatched backends");
            return nullptr;
        }
    }

    std::vector<uint32_t> objects;

    auto                  lease = SP<CDRMLease>(new CDRMLease);

    for (auto const& o : outputs) {
        auto drmo = ((CDRMOutput*)o.get())->self.lock();
        backend->log(AQ_LOG_DEBUG, std::format("drm lease: output {}, connector {}", drmo->name, drmo->connector->id));

        // FIXME: do we have to alloc a crtc here?
        if (!drmo->connector->crtc) {
            backend->log(AQ_LOG_ERROR, std::format("drm lease: output {} has no crtc", drmo->name));
            return nullptr;
        }

        backend->log(AQ_LOG_DEBUG, std::format("drm lease: crtc {}, primary {}", drmo->connector->crtc->id, drmo->connector->crtc->primary->id));

        objects.push_back(drmo->connector->id);
        objects.push_back(drmo->connector->crtc->id);
        objects.push_back(drmo->connector->crtc->primary->id);
        if (drmo->connector->crtc->cursor)
            objects.push_back(drmo->connector->crtc->cursor->id);

        lease->outputs.emplace_back(drmo);
    }

    backend->log(AQ_LOG_DEBUG, "drm lease: issuing a lease");

    int leaseFD = drmModeCreateLease(backend->gpu->fd, objects.data(), objects.size(), O_CLOEXEC, &lease->lesseeID);
    if (leaseFD < 0) {
        backend->log(AQ_LOG_ERROR, "drm lease: drm rejected a lease");
        return nullptr;
    }

    for (auto const& o : lease->outputs) {
        o->lease = lease;
    }

    lease->leaseFD = leaseFD;
    lease->backend = backend;

    backend->log(AQ_LOG_DEBUG, std::format("drm lease: lease granted with lessee id {}", lease->lesseeID));

    return lease;
}

Aquamarine::CDRMLease::~CDRMLease() {
    if (active)
        terminate();
    else
        destroy();
}

void Aquamarine::CDRMLease::terminate() {
    active = false;

    if (drmModeRevokeLease(backend->gpu->fd, lesseeID) < 0)
        backend->log(AQ_LOG_ERROR, "drm lease: Failed to revoke lease");

    destroy();
}

void Aquamarine::CDRMLease::destroy() {
    events.destroy.emit();
}
