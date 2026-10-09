// rtsp_server against real clients: ffmpeg plays a stream over RTSP, with RTP interleaved in TCP and over
// UDP, and must decode the same pixels as the JPEG decoded directly. A raw exchange checks the replies.
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

#include <string>
#include <vector>

#include "check.hpp"
#include "proc.hpp"
#include "rtp_jpeg.hpp"
#include "rtsp_server.hpp"

static std::vector<uint8_t> read_file(const std::string &path)
{
    std::vector<uint8_t> data;
    FILE *fp = fopen(path.c_str(), "rb");
    if (fp == NULL)
    {
        return data;
    }
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
    {
        data.insert(data.end(), buf, buf + n);
    }
    fclose(fp);
    return data;
}

static int free_tcp_port()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, (sockaddr *)&addr, sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(fd, (sockaddr *)&addr, &len);
    close(fd);
    return ntohs(addr.sin_port);
}

struct to_server : rtp_jpeg_sink
{
    rtsp_server *server;
    int stream;
    void send_rtp(const uint8_t *p, size_t size) override
    {
        server->send_rtp(stream, p, size);
    }
};

// Feeds a camera's worth of frames (25 fps) to stream 1 while serving, until the child exits or time runs out.
static bool serve_until_done(rtsp_server &server, const std::vector<uint8_t> &jpeg, pid_t child, int seconds)
{
    to_server sink;
    sink.server = &server;
    sink.stream = 1;
    rtp_jpeg_packetizer packetizer(sink, rtp_jpeg_config());
    uint64_t start = now_us(), next_frame = start;
    uint32_t ts = 0;
    while (now_us() - start < (uint64_t)seconds * 1000000)
    {
        server.poll(5);
        if (now_us() >= next_frame)
        {
            packetizer.send_frame(jpeg.data(), jpeg.size(), ts);
            ts += 3600;  // 25 fps at 90 kHz
            next_frame += 40000;
        }
        int status;
        if (waitpid(child, &status, WNOHANG) == child)
        {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
    }
    kill(child, SIGTERM);
    waitpid(child, nullptr, 0);
    return false;
}

static std::string request(int port, const std::string &text)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (connect(fd, (sockaddr *)&addr, sizeof(addr)) != 0)
    {
        close(fd);
        return "";
    }
    send(fd, text.data(), text.size(), 0);
    return std::to_string(fd);  // the caller polls the server, then reads with read_reply()
}

static std::string read_reply(const std::string &fd_text)
{
    int fd = atoi(fd_text.c_str());
    char buf[2048] = {};
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
    close(fd);
    return n > 0 ? std::string(buf, n) : "";
}

int main()
{
    const char *const names[] = {"cam0", "cam1", "cam2"};
    rtsp_server server;
    rtsp_config config;
    config.port = free_tcp_port();
    CHECK(server.start(config, names, 3));
    std::string base = "rtsp://127.0.0.1:" + std::to_string(config.port) + "/";

    // Raw requests: the replies RTSP clients rely on
    std::string fd = request(config.port, "OPTIONS " + base + "cam1 RTSP/1.0\r\nCSeq: 7\r\n\r\n");
    for (int i = 0; i < 10; i++)
    {
        server.poll(10);
    }
    std::string reply = read_reply(fd);
    CHECK(reply.rfind("RTSP/1.0 200 OK\r\nCSeq: 7\r\n", 0) == 0 && reply.find("DESCRIBE") != std::string::npos);

    fd = request(config.port, "DESCRIBE " + base + "cam1 RTSP/1.0\r\nCSeq: 2\r\n\r\n");
    for (int i = 0; i < 10; i++)
    {
        server.poll(10);
    }
    reply = read_reply(fd);
    CHECK(reply.find("200 OK") != std::string::npos && reply.find("m=video 0 RTP/AVP 26") != std::string::npos &&
          reply.find("a=rtpmap:26 JPEG/90000") != std::string::npos);

    fd = request(config.port, "DESCRIBE " + base + "nope RTSP/1.0\r\nCSeq: 3\r\n\r\n");
    for (int i = 0; i < 10; i++)
    {
        server.poll(10);
    }
    CHECK(read_reply(fd).find("404 Not Found") != std::string::npos);

    // ffmpeg decodes the JPEG directly, then plays cam1 over RTSP: the same pixels both ways
    std::vector<uint8_t> jpeg = read_file(DATA_DIR "/test_422.jpg");
    std::string tmp = "/tmp/test_rtsp_" + std::to_string(getpid());
    int log_fd;
    pid_t p = spawn({"/usr/bin/ffmpeg", "-v", "error", "-y", "-i", DATA_DIR "/test_422.jpg", "-f", "rawvideo", "-pix_fmt",
                     "yuv420p", tmp + "_direct.yuv"},
                    &log_fd);
    waitpid(p, nullptr, 0);
    close(log_fd);
    std::vector<uint8_t> direct = read_file(tmp + "_direct.yuv");
    const size_t frame = 640 * 480 * 3 / 2;
    CHECK(direct.size() == frame);

    for (const char *transport : {"tcp", "udp"})
    {
        std::string out = tmp + "_" + transport + ".yuv";
        pid_t client = spawn({"/usr/bin/ffmpeg", "-v", "error", "-y", "-rtsp_transport", transport, "-i", base + "cam1",
                              "-frames:v", "5", "-f", "rawvideo", "-pix_fmt", "yuv420p", out},
                             &log_fd);
        bool ok = serve_until_done(server, jpeg, client, 10);
        close(log_fd);
        std::vector<uint8_t> got = read_file(out);
        CHECK(ok);
        CHECK(got.size() == 5 * frame);
        bool same = got.size() >= frame && direct.size() == frame && memcmp(got.data(), direct.data(), frame) == 0;
        CHECK(same);
        printf("rtsp over %s: ffmpeg decoded %zu frames, %s the JPEG decoded directly\n", transport, got.size() / frame,
               same ? "identical to" : "DIFFERENT from");
        unlink(out.c_str());
    }
    unlink((tmp + "_direct.yuv").c_str());
    printf("server: %u packets sent, %u dropped\n", server.sent, server.dropped);
    CHECK(server.playing(1) == 0);  // the clients left
    return check_result("test_rtsp");
}
