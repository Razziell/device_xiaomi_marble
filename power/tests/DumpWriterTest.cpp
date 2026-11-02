// SPDX-License-Identifier: Apache-2.0
#include "DumpWriter.h"

#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
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
