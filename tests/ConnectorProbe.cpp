#include "ConnectorProbe.hpp"
#include "shared.hpp"

#include <array>
#include <barrier>
#include <cerrno>
#include <ctime>
#include <initializer_list>
#include <pthread.h>
#include <sched.h>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <hyprutils/memory/Casts.hpp>

#ifdef __linux__
#include <linux/capability.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace Aquamarine;
using namespace Hyprutils::Memory;

static uint64_t threadCPUTime() {
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts))
        throw std::runtime_error("clock_gettime");
    return (sc<uint64_t>(ts.tv_sec) * 1'000'000'000) + ts.tv_nsec;
}

static int policy() {
#ifdef __linux__
    // Read kernel policy
    // pthread_getschedparam may return stale cached values
    return sc<int>(syscall(SYS_sched_getscheduler, 0));
#else
    int         result = 0;
    sched_param param{};
    return pthread_getschedparam(pthread_self(), &result, &param) ? -1 : result;
#endif
}

// Burn CPU past the RT test's hard limit.
// Sleeping would reset RT accounting.
// Pass zero when only the worker's scheduling policy needs checking.
static int queryScheduling(uint64_t cpuTimeNS = 0) {
    int             ret            = 0;
    const auto      CALLER         = std::this_thread::get_id();
    const int       CALLER_POLICY  = policy();
    int             workerPolicy   = -1;
    bool            separateThread = false;
    CConnectorProbe probe([&](int, uint32_t) -> drmModeConnector* {
        workerPolicy = policy();
#ifdef __linux__
        workerPolicy &= ~SCHED_RESET_ON_FORK;
#endif
        separateThread = std::this_thread::get_id() != CALLER;
        if (cpuTimeNS) {
            const auto BEGIN = threadCPUTime();
            while (threadCPUTime() - BEGIN < cpuTimeNS) {
                ;
            }
        }
        errno = ENODEV;
        return nullptr;
    });

    auto*     result = probe.query(42, 1233);
    const int ERROR  = errno;
    EXPECT(result == nullptr, true);
    EXPECT(ERROR, ENODEV);
    EXPECT(workerPolicy, SCHED_OTHER);
    EXPECT(separateThread, true);
    EXPECT(policy(), CALLER_POLICY);
    return ret;
}

static int resultsAndFailures() {
    int                    ret = 0;
    const drmModeConnector EMPTY_CONNECTOR{
        .connection = DRM_MODE_UNKNOWNCONNECTION,
        .subpixel   = DRM_MODE_SUBPIXEL_UNKNOWN,
    };
    std::array connectors{
        EMPTY_CONNECTOR,
        EMPTY_CONNECTOR,
    };
    CConnectorProbe probe([&](int fd, uint32_t id) -> drmModeConnector* {
        if (fd < 0) {
            errno = EBADF;
            return nullptr;
        }
        if (id == 102)
            return nullptr; // Missing errno must become EIO.
        if (fd != 42 + sc<int>(id)) {
            errno = EINVAL;
            return nullptr;
        }

        return &connectors.at(id);
    });

    const auto      EXPECT_FAILURE = [&](int fd, uint32_t id, int expectedError) {
        const auto CONNECTOR = probe.query(fd, id);
        const int  ERROR     = errno;
        EXPECT(CONNECTOR == nullptr, true);
        EXPECT(ERROR, expectedError);
    };
    EXPECT_FAILURE(-1, 0, EBADF);
    EXPECT_FAILURE(42, 102, EIO);

    // Concurrent callers must receive their own connector and retain their errno.
    std::array<bool, 2>         matched{};
    std::array<std::jthread, 2> callers;
    std::barrier                start(2);
    for (uint32_t i = 0; i < matched.size(); ++i) {
        callers.at(i) = std::jthread([&, i] {
            start.arrive_and_wait();
            errno           = ERANGE;
            auto* connector = probe.query(42 + i, i);
            matched.at(i)   = connector == &connectors.at(i) && errno == ERANGE;
        });
    }
    for (auto& caller : callers)
        caller.join();
    for (const bool MATCHED : matched)
        EXPECT(MATCHED, true);
    return ret;
}

static int idleShutdown() {
    // Destruction must stop and join an idle worker.
    CConnectorProbe probe;
    return 0;
}

#ifdef __linux__
static int childTest(const std::function<int()>& test) {
    // Fork before creating workers so child cannot inherit their locked mutexes.
    const pid_t CHILD = fork();
    if (CHILD < 0)
        return 1;
    if (CHILD == 0) {
        alarm(5);
        int result = 1;
        try {
            result = test();
        } catch (const std::exception& e) { std::cout << e.what() << '\n'; }
        std::cout.flush();
        _exit(result);
    }
    int status = 0;
    while (waitpid(CHILD, &status, 0) < 0) {
        if (errno != EINTR)
            return 1;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    std::cout << "Test child terminated by signal " << WTERMSIG(status) << '\n';
    return 1;
}

static int realtimeTest(int flags) {
    // Isolate finite RT limit from the test runner and compositor.
    // Create worker only after promotion so inherited RT scheduling is exercised.
    return childTest([=] {
        const rlimit LIMIT = {
            .rlim_cur = 150000,
            .rlim_max = 200000,
        };
        const rlimit CORE_LIMIT = {
            .rlim_cur = 0,
            .rlim_max = 0,
        };
        const sched_param PARAM = {
            .sched_priority = 1,
        };
        if (setrlimit(RLIMIT_CORE, &CORE_LIMIT) || setrlimit(RLIMIT_RTTIME, &LIMIT)) {
            std::cout << "Failed to configure RT test limits, errno=" << errno << '\n';
            return 1;
        }
        if (syscall(SYS_sched_setscheduler, 0, SCHED_RR | flags, &PARAM)) {
            if (errno == EPERM || errno == EACCES) {
                std::cout << "SKIP: real-time test needs permission to set RT scheduling\n";
                return 77;
            }
            return 1;
        }
        // Drop capabilities so worker demotion cannot rely on CAP_SYS_NICE.
        __user_cap_header_struct header{
            .version = _LINUX_CAPABILITY_VERSION_3,
            .pid     = 0,
        };
        __user_cap_data_struct capabilities[2]{};
        if (syscall(SYS_capset, &header, capabilities))
            return 1;
        return queryScheduling(350'000'000);
    });
}
#endif

int main(int argc, char** argv) {
#ifdef __linux__
    if (argc == 2 && std::string_view(argv[1]) == "--rt") {
        for (const int FLAGS : {
                 0,
                 SCHED_RESET_ON_FORK,
             }) {
            const int RET = realtimeTest(FLAGS);
            if (RET)
                return RET;
        }
        return 0;
    }
#endif
    if (argc > 1)
        return 1;

    try {
        // Run every case even if an earlier one fails.
        return queryScheduling() | resultsAndFailures() | idleShutdown();
    } catch (const std::exception& e) {
        std::cout << e.what() << '\n';
        return 1;
    }
}
