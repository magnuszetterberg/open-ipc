// Helpers for host tests that talk UDP on 127.0.0.1 and run other programs.
#pragma once
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "check.hpp"

extern char **environ;

static inline uint64_t now_us()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// A UDP socket on 127.0.0.1; port 0 picks a free one. Returns the port through *port.
static inline int udp_socket(int *port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(*port);
    bind(fd, (sockaddr *)&addr, sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(fd, (sockaddr *)&addr, &len);
    *port = ntohs(addr.sin_port);
    return fd;
}

static inline int free_port()
{
    int port = 0;
    close(udp_socket(&port));
    return port;
}

static inline void udp_send(int fd, int port, const void *buf, size_t size)
{
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    sendto(fd, buf, size, 0, (sockaddr *)&addr, sizeof(addr));
}

// Read one datagram, waiting up to timeout_ms. Returns its size, or -1 on timeout.
static inline ssize_t udp_recv(int fd, uint8_t *buf, size_t size, int timeout_ms)
{
    pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, timeout_ms) <= 0)
    {
        return -1;
    }
    return recv(fd, buf, size, 0);
}

// Start a wfb-ng binary with its stdout on a pipe (its stats lines). Returns the pid.
static inline pid_t spawn(std::vector<std::string> args, int *stdout_fd)
{
    int pipe_fds[2];
    CHECK(pipe(pipe_fds) == 0);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
    int devnull = open("/dev/null", O_WRONLY);
    posix_spawn_file_actions_adddup2(&actions, devnull, STDERR_FILENO);

    std::vector<char *> argv;
    for (std::string &a : args)
    {
        argv.push_back(&a[0]);
    }
    argv.push_back(nullptr);
    pid_t pid = -1;
    CHECK(posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ) == 0);
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_fds[1]);
    close(devnull);
    *stdout_fd = pipe_fds[0];
    usleep(300 * 1000);  // let it bind its sockets before anything is sent
    return pid;
}

static inline std::string stop(pid_t pid, int stdout_fd, int sig = SIGTERM)
{
    kill(pid, sig);
    waitpid(pid, nullptr, 0);
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = read(stdout_fd, buf, sizeof(buf))) > 0)
    {
        out.append(buf, n);
    }
    close(stdout_fd);
    return out;
}
