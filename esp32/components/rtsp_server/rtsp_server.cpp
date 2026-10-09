#include "rtsp_server.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  // lwIP has no SIGPIPE to suppress
#endif

struct rtsp_client
{
    int fd = -1;
    sockaddr_in peer = {};
    char in[2048];          // request bytes not yet handled
    size_t in_len = 0;
    uint8_t *out = nullptr; // replies and interleaved RTP waiting to be sent
    size_t out_len = 0;
    int stream = -1;        // set by SETUP
    bool tcp = false;       // RTP interleaved in this connection, or over UDP
    uint8_t channel = 0;    // interleaved channel for RTP
    sockaddr_in udp_dest = {};
    uint32_t session = 0;
    bool playing = false;
    bool closing = false;   // TEARDOWN answered: close once the reply is out
};

static void set_nonblocking(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

rtsp_server::~rtsp_server()
{
    if (clients != nullptr)
    {
        for (int i = 0; i < config.max_clients; i++)
        {
            close_client(clients[i]);
            free(clients[i].out);
        }
        delete[] clients;
    }
    if (listen_fd >= 0)
    {
        close(listen_fd);
    }
    if (udp_fd >= 0)
    {
        close(udp_fd);
    }
}

bool rtsp_server::start(const rtsp_config &cfg, const char *const *stream_names, int stream_count)
{
    config = cfg;
    names = stream_names;
    streams = stream_count;

    clients = new rtsp_client[config.max_clients];
    for (int i = 0; i < config.max_clients; i++)
    {
        clients[i].out = static_cast<uint8_t *>(malloc(config.client_buffer));
        if (clients[i].out == nullptr)
        {
            return false;
        }
    }

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        return false;
    }
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(config.port);
    if (bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || listen(listen_fd, 4) != 0)
    {
        return false;
    }
    set_nonblocking(listen_fd);

    // One socket sends RTP to every UDP client; its port is the server_port SETUP reports.
    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    addr.sin_port = 0;
    if (udp_fd < 0 || bind(udp_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
    {
        return false;
    }
    socklen_t len = sizeof(addr);
    getsockname(udp_fd, reinterpret_cast<sockaddr *>(&addr), &len);
    udp_port = ntohs(addr.sin_port);
    set_nonblocking(udp_fd);
    return true;
}

void rtsp_server::close_client(rtsp_client &c)
{
    if (c.fd >= 0)
    {
        close(c.fd);
    }
    uint8_t *out = c.out;
    c = rtsp_client();
    c.out = out;
}

void rtsp_server::flush(rtsp_client &c)
{
    while (c.out_len > 0)
    {
        ssize_t n = send(c.fd, c.out, c.out_len, MSG_NOSIGNAL);
        if (n <= 0)
        {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                close_client(c);
            }
            return;
        }
        memmove(c.out, c.out + n, c.out_len - n);
        c.out_len -= n;
    }
    if (c.closing)
    {
        close_client(c);
    }
}

// The stream a URL names: "rtsp://host:port/cam0" or ".../cam0/trackID=0" is "cam0". -1 if none.
static int find_stream(const char *url, const char *const *names, int streams)
{
    const char *path = strstr(url, "://");
    path = path ? strchr(path + 3, '/') : url;
    if (path == nullptr)
    {
        return -1;
    }
    path++;
    for (int i = 0; i < streams; i++)
    {
        size_t n = strlen(names[i]);
        if (strncmp(path, names[i], n) == 0 && (path[n] == '\0' || path[n] == '/'))
        {
            return i;
        }
    }
    return -1;
}

// The value of a header line in a request, copied into value. False if it isn't there.
static bool header(const char *request, const char *name, char *value, size_t size)
{
    size_t n = strlen(name);
    for (const char *line = strstr(request, "\r\n"); line != nullptr; line = strstr(line + 2, "\r\n"))
    {
        const char *h = line + 2;
        if (strncasecmp(h, name, n) == 0 && h[n] == ':')
        {
            h += n + 1;
            while (*h == ' ')
            {
                h++;
            }
            size_t len = strcspn(h, "\r\n");
            len = len < size - 1 ? len : size - 1;
            memcpy(value, h, len);
            value[len] = '\0';
            return true;
        }
    }
    return false;
}

void rtsp_server::handle_request(rtsp_client &c, const char *request)
{
    char method[16] = {}, url[256] = {}, cseq[16] = "0", transport[256] = {};
    if (sscanf(request, "%15s %255s", method, url) != 2)
    {
        close_client(c);
        return;
    }
    header(request, "CSeq", cseq, sizeof(cseq));

    char reply[1024];
    char body[512] = "";
    const char *status = "200 OK";
    char extra[384] = "";
    int stream = find_stream(url, names, streams);

    if (strcmp(method, "OPTIONS") == 0)
    {
        snprintf(extra, sizeof(extra), "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n");
    }
    else if (strcmp(method, "DESCRIBE") == 0)
    {
        if (stream < 0)
        {
            status = "404 Not Found";
        }
        else
        {
            snprintf(body, sizeof(body),
                     "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=%s\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\na=control:*\r\n"
                     "m=video 0 RTP/AVP 26\r\na=rtpmap:26 JPEG/90000\r\na=control:trackID=0\r\n",
                     names[stream]);
            char base[300];
            size_t n = strlen(url);
            snprintf(base, sizeof(base), "%s%s", url, n > 0 && url[n - 1] == '/' ? "" : "/");
            snprintf(extra, sizeof(extra), "Content-Base: %s\r\nContent-Type: application/sdp\r\n", base);
        }
    }
    else if (strcmp(method, "SETUP") == 0)
    {
        if (stream < 0 || !header(request, "Transport", transport, sizeof(transport)))
        {
            status = stream < 0 ? "404 Not Found" : "461 Unsupported Transport";
        }
        else
        {
            c.stream = stream;
            c.session = next_session++;
            const char *interleaved = strstr(transport, "interleaved=");
            const char *client_port = strstr(transport, "client_port=");
            if (strstr(transport, "RTP/AVP/TCP") != nullptr)
            {
                c.tcp = true;
                c.channel = interleaved ? (uint8_t)atoi(interleaved + 12) : 0;
                snprintf(extra, sizeof(extra), "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n", c.channel,
                         c.channel + 1);
            }
            else if (client_port != nullptr)
            {
                int rtp_port = atoi(client_port + 12);
                c.tcp = false;
                c.udp_dest = c.peer;
                c.udp_dest.sin_port = htons(rtp_port);
                snprintf(extra, sizeof(extra), "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\n",
                         rtp_port, rtp_port + 1, udp_port, udp_port + 1);
            }
            else
            {
                status = "461 Unsupported Transport";
            }
        }
    }
    else if (strcmp(method, "PLAY") == 0)
    {
        if (c.stream < 0)
        {
            status = "455 Method Not Valid in This State";
        }
        else
        {
            c.playing = true;
            snprintf(extra, sizeof(extra), "Range: npt=0.000-\r\n");
        }
    }
    else if (strcmp(method, "TEARDOWN") == 0)
    {
        c.playing = false;
        c.closing = true;
    }
    else if (strcmp(method, "GET_PARAMETER") != 0 && strcmp(method, "SET_PARAMETER") != 0)
    {
        status = "501 Not Implemented";
    }

    char session[48] = "";
    if (c.session != 0)
    {
        snprintf(session, sizeof(session), "Session: %08X;timeout=60\r\n", (unsigned)c.session);
    }
    int n = snprintf(reply, sizeof(reply), "RTSP/1.0 %s\r\nCSeq: %s\r\n%s%s%sContent-Length: %d\r\n\r\n%s", status, cseq,
                     session, extra, "Server: open-ipc base station\r\n", (int)strlen(body), body);
    if (n > 0 && c.out_len + (size_t)n <= config.client_buffer)
    {
        memcpy(c.out + c.out_len, reply, n);
        c.out_len += n;
    }
}

void rtsp_server::poll(int timeout_ms)
{
    if (clients == nullptr)
    {
        return;
    }
    fd_set rd, wr;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    FD_SET(listen_fd, &rd);
    int max_fd = listen_fd;
    for (int i = 0; i < config.max_clients; i++)
    {
        rtsp_client &c = clients[i];
        if (c.fd < 0)
        {
            continue;
        }
        FD_SET(c.fd, &rd);
        if (c.out_len > 0)
        {
            FD_SET(c.fd, &wr);
        }
        max_fd = c.fd > max_fd ? c.fd : max_fd;
    }
    timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (select(max_fd + 1, &rd, &wr, nullptr, &tv) <= 0)
    {
        return;
    }

    if (FD_ISSET(listen_fd, &rd))
    {
        sockaddr_in peer = {};
        socklen_t len = sizeof(peer);
        int fd = accept(listen_fd, reinterpret_cast<sockaddr *>(&peer), &len);
        if (fd >= 0)
        {
            rtsp_client *slot = nullptr;
            for (int i = 0; i < config.max_clients && slot == nullptr; i++)
            {
                slot = clients[i].fd < 0 ? &clients[i] : nullptr;
            }
            if (slot == nullptr)
            {
                close(fd);  // full
            }
            else
            {
                set_nonblocking(fd);
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, 1 /* TCP_NODELAY */, &one, sizeof(one));
                slot->fd = fd;
                slot->peer = peer;
            }
        }
    }

    for (int i = 0; i < config.max_clients; i++)
    {
        rtsp_client &c = clients[i];
        if (c.fd < 0)
        {
            continue;
        }
        if (FD_ISSET(c.fd, &rd))
        {
            ssize_t n = recv(c.fd, c.in + c.in_len, sizeof(c.in) - 1 - c.in_len, 0);
            if (n <= 0)
            {
                if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                {
                    close_client(c);
                }
                continue;
            }
            c.in_len += n;
            // Handle every complete message: RTSP requests, and interleaved RTCP from the client (skipped)
            for (;;)
            {
                if (c.in_len >= 4 && c.in[0] == '$')
                {
                    size_t frame = 4 + ((uint8_t)c.in[2] << 8 | (uint8_t)c.in[3]);
                    if (c.in_len < frame)
                    {
                        break;
                    }
                    memmove(c.in, c.in + frame, c.in_len - frame);
                    c.in_len -= frame;
                    continue;
                }
                c.in[c.in_len] = '\0';
                char *end = strstr(c.in, "\r\n\r\n");
                if (end == nullptr)
                {
                    if (c.in_len >= sizeof(c.in) - 1)
                    {
                        close_client(c);  // a request this long isn't one we serve
                    }
                    break;
                }
                *end = '\0';
                size_t used = (size_t)(end + 4 - c.in);
                handle_request(c, c.in);
                if (c.fd < 0)
                {
                    break;
                }
                memmove(c.in, c.in + used, c.in_len - used);
                c.in_len -= used;
            }
        }
        if (c.fd >= 0 && c.out_len > 0)
        {
            flush(c);
        }
    }
}

void rtsp_server::send_rtp(int stream, const uint8_t *packet, size_t size)
{
    if (clients == nullptr || size > 0xffff)
    {
        return;
    }
    for (int i = 0; i < config.max_clients; i++)
    {
        rtsp_client &c = clients[i];
        if (c.fd < 0 || !c.playing || c.stream != stream)
        {
            continue;
        }
        if (!c.tcp)
        {
            if (sendto(udp_fd, packet, size, 0, reinterpret_cast<sockaddr *>(&c.udp_dest), sizeof(c.udp_dest)) ==
                (ssize_t)size)
            {
                sent++;
            }
            else
            {
                dropped++;
            }
            continue;
        }
        if (c.out_len + 4 + size > config.client_buffer)
        {
            dropped++;  // whole packets only, so the interleaved framing stays intact
            continue;
        }
        uint8_t *p = c.out + c.out_len;
        p[0] = '$';
        p[1] = c.channel;
        p[2] = size >> 8;
        p[3] = size & 0xff;
        memcpy(p + 4, packet, size);
        c.out_len += 4 + size;
        sent++;
        flush(c);
    }
}

int rtsp_server::playing(int stream) const
{
    int n = 0;
    for (int i = 0; clients != nullptr && i < config.max_clients; i++)
    {
        n += clients[i].fd >= 0 && clients[i].playing && clients[i].stream == stream;
    }
    return n;
}
