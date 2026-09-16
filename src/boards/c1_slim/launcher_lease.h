#ifndef C1XZ_BOARDS_C1_SLIM_LAUNCHER_LEASE_H
#define C1XZ_BOARDS_C1_SLIM_LAUNCHER_LEASE_H

#include <string>
#include <vector>

namespace c1xz {

// Cooperates with whatever launcher owns the screen.
//
// Two conventions are already established on this device by the existing ports,
// and this follows both:
//
//   1. An exclusive flock on /dev/shm/c1ancher-external-app.lock means "one app
//      owns the display". The lock may also arrive as an inherited descriptor
//      when the package manager execs us.
//   2. The factory UI process is paused with SIGSTOP rather than killed, and
//      resumed with SIGCONT on exit. Killing it makes its supervisor restart it
//      about a second later, on top of us; pausing it also avoids the "正在初始化"
//      splash the user would otherwise see on the way back.
//
// The paused process keeps its memory, which matters: this device has about
// 50 MiB and the factory UI is the OOM killer's favourite target.
class LauncherLease {
public:
    LauncherLease();
    ~LauncherLease();

    LauncherLease(const LauncherLease&) = delete;
    LauncherLease& operator=(const LauncherLease&) = delete;

    // Takes the display lock. Returns false when another app already holds it.
    bool Acquire();

    // Pauses the factory UI processes. Safe to call when none are running.
    void SuspendForeignUi();

    // Resumes anything SuspendForeignUi() paused, and releases the lock.
    void Release();

private:
    int lock_fd_ = -1;
    std::vector<int> suspended_pids_;

    static std::vector<int> FindPidsByName(const std::string& name);
};

}  // namespace c1xz

#endif  // C1XZ_BOARDS_C1_SLIM_LAUNCHER_LEASE_H
