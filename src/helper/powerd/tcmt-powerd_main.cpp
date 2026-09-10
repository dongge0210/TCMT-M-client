// tcmt-powerd — root LaunchDaemon that publishes CPU/GPU/ANE power readings.
//
// macOS 27+ gates IOReport "Energy Model" channels behind Apple-signed
// binaries (root alone is not enough — verified: a root, unsigned process
// reads 0 while /usr/bin/powermetrics works). This daemon therefore runs
// powermetrics periodically as root and parses its output, publishing the
// milliwatt values to /tmp/tcmt-power (root-owned; /tmp is sticky so other
// users cannot replace it). The client side is PowerMonitor::ReadShmPower
// in IOReportSampler.mm (macOS 27+).
//
// Installed once via tools/install-powerd.sh (sudo); managed by launchd.

#include "Utils/Logger.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <csignal>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <thread>
#include <string>

// Publication protocol with the client (see PowerMonitor.h for the same
// constants; kept local so the daemon needs no TCMTCore dependency).
// Delivered as a root-owned file in /tmp: macOS PSHM is not readable across
// uids regardless of mode (verified), and /tmp is sticky so other users can
// never replace the daemon's file once created.
struct ShmPower {
    uint64_t magic;
    uint64_t seq;
    double cpuW;
    double gpuW;
    double aneW;
};
constexpr char     kShmName[] = "/tmp/tcmt-power";
constexpr uint64_t kShmMagic  = 0x54434D54504F5752ULL;  // "TCMTPOWR"
constexpr size_t   kShmSize   = sizeof(ShmPower);

namespace {

volatile sig_atomic_t g_done = 0;
void HandleSignal(int) { g_done = 1; }

// Run one powermetrics sample (Apple-signed; root-only) and parse the last
// "CPU Power: N mW" / "GPU Power: N mW" / "ANE Power: N mW" block.
bool SamplePowers(double& cpuW, double& gpuW, double& aneW) {
    // -n 2 -i 1000: two 1s-window samples (~2s+startup); last block wins.
    const char* kCmd = "/usr/bin/powermetrics -n 2 -i 1000 2>/dev/null";
    FILE* p = popen(kCmd, "r");
    if (!p) return false;

    char line[512];
    bool any = false;
    while (fgets(line, sizeof(line), p)) {
        unsigned long long v = 0;
        if (sscanf(line, "CPU Power: %llu mW", &v) == 1) { cpuW = (double)v; any = true; }
        else if (sscanf(line, "GPU Power: %llu mW", &v) == 1) { gpuW = (double)v; any = true; }
        else if (sscanf(line, "ANE Power: %llu mW", &v) == 1) { aneW = (double)v; any = true; }
    }
    int rc = pclose(p);
    return any && rc == 0;
}

}  // namespace

int main() {
    std::signal(SIGTERM, HandleSignal);
    std::signal(SIGINT, HandleSignal);

    Logger::Initialize("/var/log/tcmt-powerd.log");
    Logger::Info("tcmt-powerd starting (uid=" + std::to_string(geteuid()) + ")");

    // Create exclusively: a local user could otherwise pre-create the file and
    // have us write into theirs (squatting). On EEXIST (stale from a crash or
    // a foreign file) unlink — root can remove anything in sticky /tmp — and
    // retry; after our first success no other user can replace the file.
    int fd = -1;
    for (int attempt = 0; attempt < 3 && fd < 0; ++attempt) {
        fd = ::open(kShmName, O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd < 0 && errno == EEXIST) {
            unlink(kShmName);
            continue;
        }
    }
    if (fd < 0) { Logger::Error("tcmt-powerd: open failed for " + std::string(kShmName)); return 1; }

    ShmPower shm {};
    shm.magic = kShmMagic;
    uint64_t seq = 0;
    auto WriteShm = [&]() {
        if (lseek(fd, 0, SEEK_SET) < 0) return;
        ssize_t w = ::write(fd, &shm, sizeof(shm));
        (void)w;
    };

    Logger::Info("tcmt-powerd: publishing on '" + std::string(kShmName) + "'");

    int failStreak = 0;
    while (!g_done) {
        double cpuW = 0, gpuW = 0, aneW = 0;
        if (SamplePowers(cpuW, gpuW, aneW)) {
            failStreak = 0;
            shm.cpuW = cpuW;
            shm.gpuW = gpuW;
            shm.aneW = aneW;
            __sync_synchronize();
            shm.seq = ++seq;
            WriteShm();
        } else if (++failStreak == 1) {
            Logger::Error("tcmt-powerd: powermetrics sampling failed");
        }

        // Startup self-check: first three publishes logged so installs can be
        // verified from /var/log/tcmt-powerd.log without another tool.
        if (seq <= 3) {
            Logger::Info("publish cpu=" + std::to_string(shm.cpuW) +
                         "mW gpu=" + std::to_string(shm.gpuW) +
                         "mW ane=" + std::to_string(shm.aneW) + "mW");
        }

        // Wait out a ~2s cadence after powermetrics finished (~2.5-4s/cycle).
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (!g_done && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    close(fd);
    unlink(kShmName);
    Logger::Info("tcmt-powerd stopped");
    return 0;
}
