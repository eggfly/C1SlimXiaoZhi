#include "boards/c1_slim/launcher_lease.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/log.h"

#define TAG "Lease"

namespace c1xz {
namespace {

const char kLockPath[] = "/dev/shm/c1ancher-external-app.lock";

// Processes that own the screen when we start. mpenMain is the factory UI;
// app_daemon is its supervisor and would restart it if it were killed.
const char* kForegroundUiNames[] = {"mpenMain"};

}  // namespace

LauncherLease::LauncherLease() = default;

LauncherLease::~LauncherLease() { Release(); }

std::vector<int> LauncherLease::FindPidsByName(const std::string& name) {
    std::vector<int> pids;
    DIR* dir = opendir("/proc");
    if (dir == nullptr) {
        return pids;
    }
    dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        int pid = atoi(entry->d_name);
        if (pid <= 0) {
            continue;
        }
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/comm", pid);
        FILE* f = fopen(path, "r");
        if (f == nullptr) {
            continue;
        }
        char comm[128] = {0};
        if (fgets(comm, sizeof(comm), f) != nullptr) {
            std::string value(comm);
            while (!value.empty() && (value.back() == '\n' || value.back() == ' ')) {
                value.pop_back();
            }
            if (value == name) {
                pids.push_back(pid);
            }
        }
        fclose(f);
    }
    closedir(dir);
    return pids;
}

bool LauncherLease::Acquire() {
    // The package manager passes the exclusive lease across exec, so first look
    // for an inherited descriptor pointing at the same file.
    struct stat expected {};
    if (lstat(kLockPath, &expected) == 0 && S_ISREG(expected.st_mode)) {
        DIR* dir = opendir("/proc/self/fd");
        if (dir != nullptr) {
            dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                int fd = atoi(entry->d_name);
                if (fd < 3) {
                    continue;
                }
                struct stat actual {};
                if (fstat(fd, &actual) == 0 && actual.st_ino == expected.st_ino &&
                    actual.st_dev == expected.st_dev) {
                    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
                        lock_fd_ = fd;
                        fcntl(fd, F_SETFD, FD_CLOEXEC);
                        closedir(dir);
                        C1XZ_LOGI(TAG, "reusing the inherited display lease");
                        return true;
                    }
                }
            }
            closedir(dir);
        }
    }

    int fd = open(kLockPath, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        // /dev/shm may not exist on a stripped image; do not block startup on
        // a convention that nothing else on this system is using.
        C1XZ_LOGW(TAG, "cannot open %s: %s; continuing without a display lease", kLockPath,
                  strerror(errno));
        return true;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        C1XZ_LOGE(TAG, "another app already owns the display");
        return false;
    }
    lock_fd_ = fd;
    return true;
}

void LauncherLease::SuspendForeignUi() {
    for (const char* name : kForegroundUiNames) {
        for (int pid : FindPidsByName(name)) {
            if (pid == getpid()) {
                continue;
            }
            if (kill(pid, SIGSTOP) == 0) {
                suspended_pids_.push_back(pid);
                C1XZ_LOGI(TAG, "paused %s (pid %d)", name, pid);
            } else {
                C1XZ_LOGW(TAG, "could not pause %s (pid %d): %s", name, pid, strerror(errno));
            }
        }
    }
}

void LauncherLease::Release() {
    for (int pid : suspended_pids_) {
        if (kill(pid, SIGCONT) == 0) {
            C1XZ_LOGI(TAG, "resumed pid %d", pid);
        }
    }
    suspended_pids_.clear();

    if (lock_fd_ >= 0) {
        flock(lock_fd_, LOCK_UN);
        close(lock_fd_);
        lock_fd_ = -1;
    }
}

}  // namespace c1xz
