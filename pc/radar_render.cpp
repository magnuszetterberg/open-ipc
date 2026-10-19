// The radar camera's data, drawn as video on this PC (docs/DESIGN.md, M9 and M10): reads its "D1" payloads,
// one per measurement, and writes the radar picture as MJPEG to stdout, for ffplay (./ipc receive N) and the
// burn-in script. The picture is radar_picture's, the core's drawing (esp32/components/radar_picture).
//
//   radar_render udp PORT        the payloads as wfb_rx delivers them (link.sh), one per datagram
//   radar_render http URL        http://<base>/camN.data from the base station: a 16-bit little-endian
//                                length, then the payload, each; connected again when it drops
//   --fps N                      pictures a second (default 10), whether data comes or not
//
// Built by pc/build-radar-render.sh.
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <string>

#include "radar_picture.hpp"

static radar_picture picture;
static radar_data latest;
static uint8_t jpeg[65536];

static double now_s()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void payload(const uint8_t *p, size_t size)
{
    radar_data d;
    if (radar_data_unpack(p, size, &d))
    {
        latest = d;
        picture.add(d);   // every measurement goes into the trace, as on the radar camera
    }
}

static int open_udp(int port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (sockaddr *)&addr, sizeof(addr)) != 0)
    {
        fprintf(stderr, "radar_render: can't listen on UDP %d: %s\n", port, strerror(errno));
        exit(1);
    }
    return fd;
}

// http://host[:port]/path: connected and asked; -1 if that failed (tried again later)
static int open_http(const std::string &url, std::string *host_out)
{
    if (url.rfind("http://", 0) != 0)
    {
        fprintf(stderr, "radar_render: not an http:// URL: %s\n", url.c_str());
        exit(1);
    }
    std::string rest = url.substr(7);
    size_t slash = rest.find('/');
    std::string hostport = rest.substr(0, slash), path = slash == std::string::npos ? "/" : rest.substr(slash);
    size_t colon = hostport.find(':');
    std::string host = hostport.substr(0, colon), port = colon == std::string::npos ? "80" : hostport.substr(colon + 1);
    *host_out = host;
    addrinfo hints = {}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0)
    {
        return -1;
    }
    int fd = -1;
    for (addrinfo *a = res; a != nullptr && fd < 0; a = a->ai_next)   // "localhost" may be ::1 first
    {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) != 0)
        {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    if (fd >= 0)
    {
        std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
        if (send(fd, request.data(), request.size(), MSG_NOSIGNAL) != (ssize_t)request.size())
        {
            close(fd);
            fd = -1;
        }
    }
    return fd;
}

int main(int argc, char **argv)
{
    std::string mode = argc > 2 ? argv[1] : "", source = argc > 2 ? argv[2] : "";
    double fps = 10;
    for (int i = 3; i + 1 < argc; i += 2)
    {
        if (strcmp(argv[i], "--fps") == 0)
        {
            fps = atof(argv[i + 1]);
        }
    }
    if ((mode != "udp" && mode != "http") || fps <= 0 || fps > 50)
    {
        fprintf(stderr, "usage: radar_render udp PORT | http http://<base>/camN.data [--fps N]  (MJPEG to stdout)\n");
        return 1;
    }

    int fd = mode == "udp" ? open_udp(atoi(source.c_str())) : -1;
    std::string host, held;          // http: what has arrived of the next record
    bool in_body = false;            // http: past the response headers
    double retry_at = 0, next_picture = now_s();
    for (;;)
    {
        if (mode == "http" && fd < 0 && now_s() >= retry_at)
        {
            fd = open_http(source, &host);
            held.clear();
            in_body = false;
            retry_at = now_s() + 2;
            if (fd < 0)
            {
                fprintf(stderr, "radar_render: can't reach %s, trying again\n", source.c_str());
            }
        }
        int wait_ms = (int)((next_picture - now_s()) * 1000);
        pollfd p = {fd, POLLIN, 0};
        if (poll(&p, fd >= 0 ? 1 : 0, wait_ms > 0 ? wait_ms : 0) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR)))
        {
            static uint8_t buf[65536];
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (mode == "udp" && n > 0)
            {
                payload(buf, (size_t)n);
            }
            else if (mode == "http" && n <= 0)
            {
                close(fd);   // dropped (the base station restarted?): connect again
                fd = -1;
            }
            else if (mode == "http")
            {
                held.append((const char *)buf, (size_t)n);
                if (!in_body)
                {
                    size_t end = held.find("\r\n\r\n");
                    if (end == std::string::npos)
                    {
                        continue;
                    }
                    if (held.rfind("HTTP/1.1 200", 0) != 0)
                    {
                        fprintf(stderr, "radar_render: %s answered %s\n", source.c_str(),
                                held.substr(0, held.find('\r')).c_str());
                        close(fd);
                        fd = -1;
                        continue;
                    }
                    held.erase(0, end + 4);
                    in_body = true;
                }
                size_t at = 0;
                while (at + 2 <= held.size())
                {
                    size_t size = (uint8_t)held[at] | (uint8_t)held[at + 1] << 8;
                    if (at + 2 + size > held.size())
                    {
                        break;
                    }
                    payload((const uint8_t *)held.data() + at + 2, size);
                    at += 2 + size;
                }
                held.erase(0, at);
            }
        }
        if (now_s() >= next_picture)
        {
            next_picture += 1 / fps;
            if (next_picture < now_s())
            {
                next_picture = now_s() + 1 / fps;   // behind: no burst to catch up
            }
            picture.draw(latest);
            size_t size = jpeg_encode_422(picture.y, picture.cb, picture.cr, radar_picture::WIDTH,
                                          radar_picture::HEIGHT, 85, jpeg, sizeof(jpeg));
            if (size > 0 && (fwrite(jpeg, 1, size, stdout) != size || fflush(stdout) != 0))
            {
                return 0;   // the player closed
            }
        }
    }
}
