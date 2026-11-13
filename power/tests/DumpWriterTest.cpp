// SPDX-License-Identifier: Apache-2.0
#include "DumpWriter.h"

#include <gtest/gtest.h>
#include <cstddef>
#include <cstdlib>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <string>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>

namespace marble::power {
namespace {
using namespace std::chrono_literals;

struct Pipe {
    int fds[2]{-1, -1};
    Pipe() { if (pipe(fds) != 0) std::abort(); }
    ~Pipe() { for (int fd : fds) if (fd >= 0) close(fd); }
};

TEST(DumpWriter, WritesPipeWithoutChangingCallerFlags) {
    Pipe pipe;
    const int flags = fcntl(pipe.fds[1], F_GETFL);
    ASSERT_TRUE(writeBoundedDump(pipe.fds[1], "power-state\n"));
    char buf[64]{};
    ASSERT_EQ(read(pipe.fds[0], buf, sizeof(buf)), 12);
    EXPECT_EQ(std::string(buf, 12), "power-state\n");
    EXPECT_EQ(fcntl(pipe.fds[1], F_GETFL), flags);
}

TEST(DumpWriter, FullPipeReturnsWithinBudgetAndPreservesFlags) {
    Pipe pipe;
    ASSERT_EQ(fcntl(pipe.fds[1], F_SETFL, O_NONBLOCK), 0);
    const std::string fill(4096, 'x');
    while (write(pipe.fds[1], fill.data(), fill.size()) > 0) {}
    ASSERT_EQ(errno, EAGAIN);
    ASSERT_EQ(fcntl(pipe.fds[1], F_SETFL, 0), 0);
    const int flags = fcntl(pipe.fds[1], F_GETFL);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(writeBoundedDump(pipe.fds[1], "state"));
    EXPECT_EQ(errno, ETIMEDOUT);
    // Wide scheduling margin; the helper's I/O wait budget is 100 ms.
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
    EXPECT_EQ(fcntl(pipe.fds[1], F_GETFL), flags);
}

TEST(DumpWriter, SocketUsesPerCallNonblockingFlag) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    const int flags = fcntl(fds[0], F_GETFL);
    EXPECT_TRUE(writeBoundedDump(fds[0], "hello"));
    char buf[5];
    EXPECT_EQ(read(fds[1], buf, sizeof(buf)), 5);
    EXPECT_EQ(std::string(buf, 5), "hello");
    EXPECT_EQ(fcntl(fds[0], F_GETFL), flags);
    close(fds[0]); close(fds[1]);
}

TEST(DumpWriter, FullSocketReturnsWithinBudgetAndPreservesFlags) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    const int flags = fcntl(fds[0], F_GETFL);
    const std::string fill(4096, 'x');
    while (send(fds[0], fill.data(), fill.size(), MSG_DONTWAIT | MSG_NOSIGNAL) > 0) {}
    EXPECT_EQ(errno, EAGAIN);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(writeBoundedDump(fds[0], "state"));
    EXPECT_EQ(errno, ETIMEDOUT);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
    EXPECT_EQ(fcntl(fds[0], F_GETFL), flags);
    close(fds[0]); close(fds[1]);
}

TEST(DumpWriter, ClosedPipeDoesNotDeliverSigpipeAndRestoresMask) {
    Pipe pipe;
    close(pipe.fds[0]); pipe.fds[0] = -1;
    sigset_t before, after;
    ASSERT_EQ(pthread_sigmask(SIG_SETMASK, nullptr, &before), 0);
    EXPECT_FALSE(writeBoundedDump(pipe.fds[1], "state"));
    ASSERT_EQ(pthread_sigmask(SIG_SETMASK, nullptr, &after), 0);
    EXPECT_EQ(sigismember(&before, SIGPIPE), sigismember(&after, SIGPIPE));
}

TEST(DumpWriter, InvalidFdOversizedAndNonPipeFailClosed) {
    EXPECT_FALSE(writeBoundedDump(-1, "state"));
    EXPECT_FALSE(writeBoundedDump(-1, std::string(16385, 'x')));
    EXPECT_EQ(errno, EOVERFLOW);
    Pipe pipe;
    EXPECT_FALSE(writeBoundedDump(pipe.fds[0], "state"));
    EXPECT_EQ(errno, EBADF);
    const int fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    ASSERT_GE(fd, 0);
    EXPECT_FALSE(writeBoundedDump(fd, "state"));
    EXPECT_EQ(errno, EOPNOTSUPP);
    close(fd);
}

// Models the SELinux policy on the device: the HAL may use and write a pipe
// owned by dumpstate, but getattr (fstat) and open (reopening /proc/self/fd/N)
// are denied. The writer must work with the descriptor exactly as received.
TEST(DumpWriter, PipeNeedsNeitherStatNorReopen) {
    Pipe pipe;
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
#define MARBLE_DENY(nr) \
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1), \
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES)
        sock_filter filter[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
            MARBLE_DENY(SYS_fstat),
            MARBLE_DENY(SYS_newfstatat),
            MARBLE_DENY(SYS_statx),
            MARBLE_DENY(SYS_openat),
#ifdef SYS_open
            MARBLE_DENY(SYS_open),
#endif
#ifdef SYS_stat
            MARBLE_DENY(SYS_stat),
#endif
#ifdef SYS_openat2
            MARBLE_DENY(SYS_openat2),
#endif
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
#undef MARBLE_DENY
        sock_fprog program{static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])), filter};
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
            prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) {
            _exit(77);
        }
        _exit(writeBoundedDump(pipe.fds[1], "power-state\n") ? 0 : 1);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    if (WEXITSTATUS(status) == 77) GTEST_SKIP() << "seccomp filters unavailable";
    ASSERT_EQ(WEXITSTATUS(status), 0);
    char buf[64]{};
    ASSERT_EQ(read(pipe.fds[0], buf, sizeof(buf)), 12);
    EXPECT_EQ(std::string(buf, 12), "power-state\n");
}

TEST(DumpWriter, ClosedReaderReportsEpipeAndLeavesNoPendingSignal) {
    Pipe pipe;
    close(pipe.fds[0]); pipe.fds[0] = -1;
    sigset_t block, old, pending;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    ASSERT_EQ(pthread_sigmask(SIG_BLOCK, &block, &old), 0);
    EXPECT_FALSE(writeBoundedDump(pipe.fds[1], "state"));
    EXPECT_EQ(errno, EPIPE);
    ASSERT_EQ(sigpending(&pending), 0);
    EXPECT_EQ(sigismember(&pending, SIGPIPE), 0);
    ASSERT_EQ(pthread_sigmask(SIG_SETMASK, &old, nullptr), 0);
}

TEST(DumpWriter, SlowPipeCanDrainWithoutUnboundedWait) {
    Pipe pipe;
    std::string payload(16000, 'x');
    std::string received;
    std::thread reader([&] {
        char buffer[1024];
        while (received.size() < payload.size()) {
            const ssize_t count = read(pipe.fds[0], buffer, sizeof(buffer));
            if (count <= 0) return;
            received.append(buffer, static_cast<size_t>(count));
        }
    });
    const bool success = writeBoundedDump(pipe.fds[1], payload);
    // Let the reader reach EOF even if the writer times out.
    close(pipe.fds[1]); pipe.fds[1] = -1;
    reader.join();
    EXPECT_TRUE(success);
    EXPECT_EQ(received, payload);
}

}  // namespace
}  // namespace marble::power
