// web_server with rtp_jpeg_streamer, against real clients: the page and the status document; ffmpeg plays a
// camera's MJPEG and must decode the same pixels as the JPEG decoded directly; with packets lost, every part
// of the stream is the whole JPEG or the start of it; a viewer that stops reading doesn't hold up the others;
// a client over the limit is turned away.
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

#include <string>
#include <vector>

#include "check.hpp"
#include "proc.hpp"
#include "rtp_jpeg.hpp"
#include "web_server.hpp"

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

// The base station's wiring: streamer -> web server, one stream.
struct to_web : jpeg_stream_sink
{
    web_server *server;
    int stream;
    void jpeg_begin() override
    {
        server->jpeg_begin(stream);
    }
    void jpeg_data(const uint8_t *d, size_t n) override
    {
        server->jpeg_data(stream, d, n);
    }
    void jpeg_end(bool complete) override
    {
        server->jpeg_end(stream, complete);
    }
};

struct packet_list : rtp_jpeg_sink
{
    std::vector<std::vector<uint8_t>> packets;
    void send_rtp(const uint8_t *p, size_t size) override
    {
        packets.emplace_back(p, p + size);
    }
};

// A camera on stream 1: one frame's RTP packets every 40 ms (25 fps, or interval_us), through the streamer. lose(frame, k)
// says whether to drop packet k of a frame.
struct camera
{
    packet_list frame;
    to_web sink;
    rtp_jpeg_streamer streamer{sink};
    uint64_t next_us = 0;
    uint64_t interval_us = 40000;
    int frames = 0;
    bool (*lose)(int frame, size_t k) = nullptr;

    camera(web_server &server, const std::vector<uint8_t> &jpeg)
    {
        sink.server = &server;
        sink.stream = 1;
        rtp_jpeg_packetizer packetizer(frame, rtp_jpeg_config());
        CHECK(packetizer.send_frame(jpeg.data(), jpeg.size(), 0) == RTP_JPEG_OK);
    }
    void tick()
    {
        if (now_us() < next_us)
        {
            return;
        }
        next_us = now_us() + interval_us;
        for (size_t k = 0; k < frame.packets.size(); k++)
        {
            std::vector<uint8_t> p = frame.packets[k];
            uint32_t ts = 3600 * frames;  // a new frame each time: new timestamp
            p[4] = ts >> 24, p[5] = ts >> 16, p[6] = ts >> 8, p[7] = ts;
            if (lose == nullptr || !lose(frames, k))
            {
                streamer.push(p.data(), p.size());
            }
        }
        frames++;
    }
};

static int connect_to(int port, const std::string &request)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (connect(fd, (sockaddr *)&addr, sizeof(addr)) != 0)
    {
        close(fd);
        return -1;
    }
    send(fd, request.data(), request.size(), 0);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

static void read_some(int fd, std::string *into)
{
    char buf[65536];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
    {
        into->append(buf, n);
    }
}

// A whole request/response: the server is polled meanwhile. Returns everything the server sent.
static std::string fetch(web_server &server, int port, const std::string &request)
{
    int fd = connect_to(port, request);
    std::string got;
    uint64_t end = now_us() + 2000000;
    while (now_us() < end)
    {
        server.poll(5);
        char buf[4096];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0)
        {
            got.append(buf, n);
        }
        else if (n == 0)
        {
            break;  // the server closed it: response complete
        }
    }
    close(fd);
    return got;
}

// The JPEG parts of a multipart/x-mixed-replace response, in order.
static std::vector<std::string> parts_of(const std::string &stream)
{
    std::vector<std::string> parts;
    const std::string header = "\r\n--frame\r\nContent-Type: image/jpeg\r\n\r\n";
    size_t at = stream.find(header);
    while (at != std::string::npos)
    {
        size_t start = at + header.size(), next = stream.find(header, start);
        if (next == std::string::npos)
        {
            break;  // the last part may still be arriving
        }
        parts.push_back(stream.substr(start, next - start));
        at = next;
    }
    return parts;
}

int main()
{
    const char *const names[] = {"cam0", "cam1", "cam2"};
    static const char page[] = "<!doctype html><title>base</title><img src=\"/cam0.mjpg\">";
    web_server server;
    web_config config;
    config.port = free_tcp_port();
    config.max_clients = 3;
    config.client_buffer = 4096;
    CHECK(server.start(config, names, 3));
    server.set_page(page, sizeof(page) - 1);
    const char status[] = "{\"cams\":[]}";
    server.set_status(status, sizeof(status) - 1);
    int port = config.port;
    std::string base = "http://127.0.0.1:" + std::to_string(port) + "/";

    // The page, the status document, and what isn't there
    std::string reply = fetch(server, port, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(reply.rfind("HTTP/1.1 200 OK\r\n", 0) == 0 && reply.find("text/html") != std::string::npos);
    CHECK(reply.size() > sizeof(page) - 1 && reply.compare(reply.size() - (sizeof(page) - 1), std::string::npos, page) == 0);
    reply = fetch(server, port, "GET /status.json?1234 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(reply.find("application/json") != std::string::npos && reply.find("\r\n\r\n{\"cams\":[]}") != std::string::npos);
    CHECK(fetch(server, port, "GET /cam3.mjpg HTTP/1.1\r\n\r\n").find("404 Not Found") != std::string::npos);
    CHECK(fetch(server, port, "POST / HTTP/1.1\r\n\r\n").find("405") != std::string::npos);

    // ffmpeg decodes the JPEG directly, then plays cam1 over HTTP: the same pixels both ways
    std::vector<uint8_t> jpeg = read_file(DATA_DIR "/test_422.jpg");
    std::string tmp = "/tmp/test_web_" + std::to_string(getpid());
    int log_fd;
    pid_t p = spawn({"/usr/bin/ffmpeg", "-v", "error", "-y", "-i", DATA_DIR "/test_422.jpg", "-f", "rawvideo", "-pix_fmt",
                     "yuv420p", tmp + "_direct.yuv"},
                    &log_fd);
    waitpid(p, nullptr, 0);
    close(log_fd);
    std::vector<uint8_t> direct = read_file(tmp + "_direct.yuv");
    const size_t frame_size = 640 * 480 * 3 / 2;
    CHECK(direct.size() == frame_size);
    {
        camera cam(server, jpeg);
        std::string out = tmp + "_mjpeg.yuv";
        pid_t client = spawn({"/usr/bin/ffmpeg", "-v", "error", "-y", "-f", "mpjpeg", "-i", base + "cam1.mjpg", "-frames:v",
                              "5", "-f", "rawvideo", "-pix_fmt", "yuv420p", out},
                             &log_fd);
        bool ok = false;
        for (uint64_t end = now_us() + 10000000; now_us() < end;)
        {
            server.poll(5);
            cam.tick();
            int st;
            if (waitpid(client, &st, WNOHANG) == client)
            {
                ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
                client = -1;
                break;
            }
        }
        if (client > 0)
        {
            kill(client, SIGTERM);
            waitpid(client, nullptr, 0);
        }
        close(log_fd);
        std::vector<uint8_t> got = read_file(out);
        bool same = got.size() >= frame_size && memcmp(got.data(), direct.data(), frame_size) == 0;
        CHECK(ok && got.size() == 5 * frame_size && same);
        printf("mjpeg: ffmpeg decoded %zu frames, %s the JPEG decoded directly\n", got.size() / frame_size,
               same ? "identical to" : "DIFFERENT from");
        unlink(out.c_str());
    }
    unlink((tmp + "_direct.yuv").c_str());
    for (int i = 0; i < 20; i++)
    {
        server.poll(5);  // ffmpeg's connection closes
    }
    CHECK(server.viewers(1) == 0);

    // A packet lost in every third frame: those parts stop where it went missing, the others are whole
    static uint8_t whole[65536], scan[65536];
    size_t whole_size = 0;
    {
        camera ref(server, jpeg);
        rtp_jpeg_depacketizer d(whole, sizeof(whole), scan, sizeof(scan));
        for (const std::vector<uint8_t> &pk : ref.frame.packets)
        {
            whole_size = d.push(pk.data(), pk.size());
        }
        CHECK(whole_size > 0);
    }
    {
        camera cam(server, jpeg);
        cam.lose = [](int frame, size_t k) { return frame % 3 == 1 && k == 4; };
        int fd = connect_to(port, "GET /cam1.mjpg HTTP/1.1\r\n\r\n");
        std::string got;
        for (uint64_t end = now_us() + 1500000; now_us() < end;)
        {
            server.poll(5);
            cam.tick();
            read_some(fd, &got);
        }
        close(fd);
        CHECK(got.find("multipart/x-mixed-replace; boundary=frame") != std::string::npos);
        std::vector<std::string> parts = parts_of(got);
        int n_whole = 0, n_cut = 0;
        bool all_ok = true;
        for (const std::string &part : parts)
        {
            if (part.size() == whole_size && memcmp(part.data(), whole, whole_size) == 0)
            {
                n_whole++;
            }
            else if (part.size() < whole_size && memcmp(part.data(), whole, part.size()) == 0)
            {
                n_cut++;
            }
            else
            {
                all_ok = false;
            }
        }
        CHECK(all_ok && n_whole >= 15 && n_cut >= 7);
        printf("loss: %zu parts, %d whole, %d cut off where a packet went missing\n", parts.size(), n_whole, n_cut);
    }
    for (int i = 0; i < 20; i++)
    {
        server.poll(5);
    }

    // A viewer that stops reading: its JPEGs are cut off, another viewer still gets every one whole; and a
    // client over the limit (3) is turned away
    {
        camera cam(server, jpeg);
        int stuck = socket(AF_INET, SOCK_STREAM, 0);
        int small = 4096;
        setsockopt(stuck, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        CHECK(connect(stuck, (sockaddr *)&addr, sizeof(addr)) == 0);
        const char req[] = "GET /cam1.mjpg HTTP/1.1\r\n\r\n";
        send(stuck, req, sizeof(req) - 1, 0);
        int good = connect_to(port, "GET /cam1.mjpg HTTP/1.1\r\n\r\n");
        uint32_t behind_before = server.jpegs_cut + server.jpegs_skipped;
        cam.interval_us = 2000;  // 500 fps: past what Linux's socket buffers absorb for the stuck one in 2 s
        std::string got;
        for (uint64_t end = now_us() + 2000000; now_us() < end;)
        {
            server.poll(5);
            cam.tick();
            read_some(good, &got);
        }
        CHECK(server.viewers(1) == 2);
        int third = connect_to(port, "GET /cam0.mjpg HTTP/1.1\r\n\r\n");  // the last slot
        int fourth = -1;
        std::string turned_away;
        for (uint64_t end = now_us() + 300000; now_us() < end;)
        {
            server.poll(5);
            if (fourth < 0 && server.viewers(0) == 1)
            {
                fourth = connect_to(port, "GET / HTTP/1.1\r\n\r\n");
            }
            if (fourth >= 0)
            {
                read_some(fourth, &turned_away);
            }
        }
        CHECK(server.refused == 1 && turned_away.find("503 Service Unavailable") != std::string::npos);
        close(third);
        close(fourth);
        std::vector<std::string> parts = parts_of(got);
        int n_whole = 0;
        for (const std::string &part : parts)
        {
            n_whole += part.size() == whole_size && memcmp(part.data(), whole, whole_size) == 0;
        }
        CHECK(parts.size() >= 200 && n_whole == (int)parts.size());
        uint32_t behind = server.jpegs_cut + server.jpegs_skipped - behind_before;
        CHECK(behind > 100);
        printf("slow viewer: the reading one got %d of %zu parts whole; the stuck one missed %u JPEGs\n", n_whole,
               parts.size(), (unsigned)behind);
        close(stuck);
        close(good);
    }

    // may_send says no (the base station low on RAM): the reply waits in the server; then it goes
    {
        static bool allow = false;
        web_server held;
        web_config cfg;
        cfg.port = free_tcp_port();
        cfg.max_clients = 2;
        cfg.client_buffer = 4096;
        cfg.may_send = [](size_t) { return allow; };
        CHECK(held.start(cfg, names, 3));
        held.set_status(status, sizeof(status) - 1);
        int fd = connect_to(cfg.port, "GET /status.json HTTP/1.1\r\n\r\n");
        std::string got;
        for (uint64_t end = now_us() + 300000; now_us() < end;)
        {
            held.poll(5);
            read_some(fd, &got);
        }
        CHECK(got.empty());
        allow = true;
        for (uint64_t end = now_us() + 300000; now_us() < end;)
        {
            held.poll(5);
            read_some(fd, &got);
        }
        CHECK(got.find("{\"cams\":[]}") != std::string::npos);
        printf("may_send: held while it said no, sent once it said yes\n");
        close(fd);
    }
    return check_result("test_web");
}
