// wfb_core against the real wfb-ng binaries, without a radio (R1, design: Testing).
//   our wfb_tx -> wfb_rx -a (aggregator: takes forwarded packets over UDP)
//   wfb_tx -D (debug: sends to a UDP port instead of a WiFi card) -> our wfb_rx
// Both directions carry the wfb-ng packet behind a wrxfwd_t header, as wfb-ng's forwarder does.
// Not covered here: the 802.11 header, which the on-air test (M1) checks.
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
#include "keys.hpp"
#include "wfb_rx.hpp"
#include "wfb_tx.hpp"
#include "wifibroadcast.hpp"

extern char **environ;

static const int count = 100;

static std::string make_payload(int i)
{
    return "payload " + std::to_string(i) + std::string(i * 37 % 1400, 'x');
}

static uint64_t now_us()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// A UDP socket on 127.0.0.1; port 0 picks a free one. Returns the port through *port.
static int udp_socket(int *port)
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

static int free_port()
{
    int port = 0;
    close(udp_socket(&port));
    return port;
}

static void udp_send(int fd, int port, const void *buf, size_t size)
{
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    sendto(fd, buf, size, 0, (sockaddr *)&addr, sizeof(addr));
}

// Read one datagram, waiting up to timeout_ms. Returns its size, or -1 on timeout.
static ssize_t udp_recv(int fd, uint8_t *buf, size_t size, int timeout_ms)
{
    pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, timeout_ms) <= 0)
    {
        return -1;
    }
    return recv(fd, buf, size, 0);
}

// Start a wfb-ng binary with its stdout on a pipe (its stats lines). Returns the pid.
static pid_t spawn(std::vector<std::string> args, int *stdout_fd)
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

static std::string stop(pid_t pid, int stdout_fd)
{
    kill(pid, SIGTERM);
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

// Sum one field of wfb_rx's "PKT" stats lines: all:bytes:dec_err:session:data:uniq:fec_rec:lost:bad:out:out_bytes
static unsigned long pkt_field(const std::string &log, int field)
{
    unsigned long sum = 0;
    size_t pos = 0;
    while ((pos = log.find("\tPKT\t", pos)) != std::string::npos)
    {
        pos += 5;
        unsigned long v[11] = {};
        if (sscanf(log.c_str() + pos, "%lu:%lu:%lu:%lu:%lu:%lu:%lu:%lu:%lu:%lu:%lu", &v[0], &v[1], &v[2], &v[3],
                   &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10]) == 11)
        {
            sum += v[field];
        }
    }
    return sum;
}

// Sends our frames to wfb_rx's aggregator port, behind a wrxfwd_t header as wfb-ng's forwarder does.
struct forwarder : wfb_sink
{
    int fd, port;
    void send_frame(const uint8_t *buf, size_t size) override
    {
        uint8_t packet[sizeof(wrxfwd_t) + WFB_MAX_PACKET];
        wrxfwd_t hdr = {};
        memset(hdr.antenna, 0xff, sizeof(hdr.antenna));
        hdr.antenna[0] = 0;
        hdr.rssi[0] = -42;
        hdr.noise[0] = -70;
        hdr.freq = htons(2437);  // channel 6
        hdr.mcs_index = 3;
        hdr.bandwidth = 20;
        memcpy(packet, &hdr, sizeof(hdr));
        memcpy(packet + sizeof(hdr), buf, size);
        udp_send(fd, port, packet, sizeof(hdr) + size);
    }
};

struct payload_list : wfb_payload_sink
{
    std::vector<std::string> payloads;
    void send_payload(const uint8_t *buf, size_t size) override
    {
        payloads.emplace_back((const char *)buf, size);
    }
};

static void our_tx_to_real_rx(const uint8_t *drone_key)
{
    int out_port = 0;
    int out_fd = udp_socket(&out_port);
    int agg_port = free_port();
    int log_fd;
    pid_t rx = spawn({WFB_NG_DIR "/wfb_rx", "-a", std::to_string(agg_port), "-K", KEYS_DIR "/gs.key", "-c",
                      "127.0.0.1", "-u", std::to_string(out_port), "-p", "0", "-l", "100"},
                     &log_fd);

    int send_port = 0;
    forwarder link;
    link.fd = udp_socket(&send_port);
    link.port = agg_port;
    wfb_tx tx(link);
    wfb_tx_config config;
    CHECK(tx.init(drone_key, 64, config) == WFB_OK);
    for (int i = 0; i < count; i++)
    {
        std::string p = make_payload(i);
        CHECK(tx.send((const uint8_t *)p.data(), p.size(), now_us()) == WFB_OK);
        usleep(200);
    }
    CHECK(tx.close_block() == WFB_OK);

    std::vector<std::string> got;
    uint8_t buf[2048];
    ssize_t n;
    while ((int)got.size() < count && (n = udp_recv(out_fd, buf, sizeof(buf), 1000)) >= 0)
    {
        got.emplace_back((const char *)buf, n);
    }
    usleep(300 * 1000);  // one more stats line
    std::string log = stop(rx, log_fd);

    CHECK(got.size() == (size_t)count);
    for (size_t i = 0; i < got.size(); i++)
    {
        CHECK(got[i] == make_payload(i));
    }
    CHECK(pkt_field(log, 2) == 0);                       // no decryption errors
    CHECK(pkt_field(log, 9) == (unsigned long)count);    // every payload out
    printf("our tx -> wfb_rx: %zu of %d payloads, %lu decryption errors\n", got.size(), count, pkt_field(log, 2));
    close(out_fd);
    close(link.fd);
}

static void real_tx_to_our_rx(const uint8_t *gs_key)
{
    int dbg_port = 0;
    int dbg_fd = udp_socket(&dbg_port);
    int in_port = free_port();
    int log_fd;
    // -T 50: close a half-full block after 50 ms, so the last payloads arrive without more traffic
    pid_t tx = spawn({WFB_NG_DIR "/wfb_tx", "-K", KEYS_DIR "/drone.key", "-k", "8", "-n", "12", "-u",
                      std::to_string(in_port), "-p", "0", "-T", "50", "-D", std::to_string(dbg_port), "test0"},
                     &log_fd);

    payload_list out;
    wfb_rx rx(out);
    wfb_rx_config config;
    CHECK(rx.init(gs_key, 64, config) == WFB_OK);

    // Read while sending: 100 payloads make about 225 KB of packets, more than a socket buffer holds.
    uint8_t buf[sizeof(wrxfwd_t) + MAX_FORWARDER_PACKET_SIZE];
    auto receive = [&](int timeout_ms) {
        ssize_t n = udp_recv(dbg_fd, buf, sizeof(buf), timeout_ms);
        if (n > (ssize_t)sizeof(wrxfwd_t))
        {
            rx.process_packet(buf + sizeof(wrxfwd_t), n - sizeof(wrxfwd_t));
        }
        return n >= 0;
    };
    int send_port = 0;
    int send_fd = udp_socket(&send_port);
    for (int i = 0; i < count; i++)
    {
        std::string p = make_payload(i);
        udp_send(send_fd, in_port, p.data(), p.size());
        while (receive(1))
        {
        }
    }
    while ((int)out.payloads.size() < count && receive(1000))
    {
    }
    stop(tx, log_fd);

    CHECK(out.payloads.size() == (size_t)count);
    for (size_t i = 0; i < out.payloads.size(); i++)
    {
        CHECK(out.payloads[i] == make_payload(i));
    }
    CHECK(rx.stats.count_p_dec_err == 0 && rx.stats.count_p_bad == 0);
    printf("wfb_tx -> our rx: %zu of %d payloads, %u decryption errors\n", out.payloads.size(), count,
           rx.stats.count_p_dec_err);
    close(dbg_fd);
    close(send_fd);
}

int main()
{
    uint8_t drone_key[64], gs_key[64];
    CHECK(read_key("drone.key", drone_key, sizeof(drone_key)));
    CHECK(read_key("gs.key", gs_key, sizeof(gs_key)));

    our_tx_to_real_rx(drone_key);
    real_tx_to_our_rx(gs_key);
    return check_result("test_wfb_ng");
}
