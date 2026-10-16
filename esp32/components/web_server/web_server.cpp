#include "web_server.hpp"

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

// Before each JPEG of a multipart/x-mixed-replace stream. The leading CRLF ends the previous part (or is
// the preamble, before the first); no Content-Length, because a JPEG is sent before its size is known.
static const char part_header[] = "\r\n--frame\r\nContent-Type: image/jpeg\r\n\r\n";

struct web_client
{
    int fd = -1;
    char in[512];                 // the request; only its first line matters
    size_t in_len = 0;
    bool answered = false;        // the request is handled; anything more it sends is ignored
    uint8_t *out = nullptr;       // response headers, status document, MJPEG waiting to be sent
    size_t out_len = 0;
    const char *body = nullptr;   // the page, sent from where it lives once out is empty
    size_t body_left = 0;
    int stream = -1;              // an MJPEG viewer of this stream
    bool in_jpeg = false;         // in the middle of a JPEG: the next piece belongs to it
    bool closing = false;         // close once everything is sent
};

static void set_nonblocking(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

web_server::~web_server()
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
}

bool web_server::start(const web_config &cfg, const char *const *stream_names, int stream_count)
{
    config = cfg;
    names = stream_names;
    streams = stream_count;
    if (config.client_buffer < status_max + 256)
    {
        return false;  // a status response must fit
    }

    clients = new web_client[config.max_clients];
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
    return true;
}

void web_server::set_page(const char *html, size_t size)
{
    page = html;
    page_size = size;
}

void web_server::set_status(const char *json, size_t size)
{
    status_size = size < sizeof(status) ? size : sizeof(status);
    memcpy(status, json, status_size);
}

void web_server::close_client(web_client &c)
{
    if (c.fd >= 0)
    {
        close(c.fd);
    }
    uint8_t *out = c.out;
    c = web_client();
    c.out = out;
}

void web_server::flush(web_client &c)
{
    while (c.out_len > 0 || c.body_left > 0)
    {
        const void *data = c.out_len > 0 ? (const void *)c.out : (const void *)c.body;
        size_t size = c.out_len > 0 ? c.out_len : c.body_left;
        ssize_t n = send(c.fd, data, size, MSG_NOSIGNAL);
        if (n <= 0)
        {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                close_client(c);
            }
            return;
        }
        if (c.out_len > 0)
        {
            memmove(c.out, c.out + n, c.out_len - n);
            c.out_len -= n;
        }
        else
        {
            c.body += n;
            c.body_left -= n;
        }
    }
    if (c.closing)
    {
        close_client(c);
    }
}

bool web_server::append(web_client &c, const void *data, size_t size)
{
    if (c.out_len + size > config.client_buffer)
    {
        flush(c);  // make room, if TCP will take some
        if (c.fd < 0 || c.out_len + size > config.client_buffer)
        {
            return false;
        }
    }
    memcpy(c.out + c.out_len, data, size);
    c.out_len += size;
    return true;
}

void web_server::respond(web_client &c, const char *status_line, const char *type, const char *body, size_t size,
                         bool copy)
{
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     status_line, type, (unsigned)size);
    append(c, head, (size_t)n);
    if (copy)
    {
        append(c, body, size);  // fits: client_buffer >= status_max + 256 (start)
    }
    else
    {
        c.body = body;
        c.body_left = size;
    }
    c.closing = true;
}

void web_server::handle_request(web_client &c)
{
    c.answered = true;
    char method[8] = {}, path[64] = {};
    c.in[c.in_len < sizeof(c.in) ? c.in_len : sizeof(c.in) - 1] = '\0';
    if (sscanf(c.in, "%7s %63s", method, path) != 2)
    {
        close_client(c);
        return;
    }
    char *query = strchr(path, '?');  // the page asks for status.json?<time> to get past caches
    if (query != nullptr)
    {
        *query = '\0';
    }
    if (strcmp(method, "GET") != 0)
    {
        static const char text[] = "only GET\n";
        respond(c, "405 Method Not Allowed", "text/plain", text, sizeof(text) - 1, true);
        return;
    }
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)
    {
        respond(c, "200 OK", "text/html; charset=utf-8", page ? page : "", page ? page_size : 0, false);
        return;
    }
    if (strcmp(path, "/status.json") == 0)
    {
        respond(c, "200 OK", "application/json", status, status_size, true);
        return;
    }
    for (int i = 0; i < streams; i++)
    {
        size_t n = strlen(names[i]);
        if (path[0] == '/' && strncmp(path + 1, names[i], n) == 0 && strcmp(path + 1 + n, ".mjpg") == 0)
        {
            static const char head[] = "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                                       "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
            append(c, head, sizeof(head) - 1);
            c.stream = i;
            return;
        }
    }
    static const char text[] = "not found\n";
    respond(c, "404 Not Found", "text/plain", text, sizeof(text) - 1, true);
}

void web_server::poll(int timeout_ms)
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
        web_client &c = clients[i];
        if (c.fd < 0)
        {
            continue;
        }
        FD_SET(c.fd, &rd);
        if (c.out_len > 0 || c.body_left > 0)
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
        int fd = accept(listen_fd, nullptr, nullptr);
        if (fd >= 0)
        {
            web_client *slot = nullptr;
            for (int i = 0; i < config.max_clients && slot == nullptr; i++)
            {
                slot = clients[i].fd < 0 ? &clients[i] : nullptr;
            }
            if (slot == nullptr)
            {
                static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\n"
                                           "Content-Length: 17\r\nConnection: close\r\n\r\ntoo many viewers\n";
                send(fd, busy, sizeof(busy) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
                close(fd);
                refused++;
            }
            else
            {
                set_nonblocking(fd);
                slot->fd = fd;
            }
        }
    }

    for (int i = 0; i < config.max_clients; i++)
    {
        web_client &c = clients[i];
        if (c.fd < 0)
        {
            continue;
        }
        if (FD_ISSET(c.fd, &rd))
        {
            char discard[64];
            char *into = c.answered ? discard : c.in + c.in_len;
            size_t room = c.answered ? sizeof(discard) : sizeof(c.in) - 1 - c.in_len;
            ssize_t n = recv(c.fd, into, room, 0);
            if (n <= 0)
            {
                if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                {
                    close_client(c);
                }
                continue;
            }
            if (!c.answered)
            {
                c.in_len += n;
                c.in[c.in_len] = '\0';
                // The whole request, or as much as fits: only its first line matters
                if (strstr(c.in, "\r\n\r\n") != nullptr || c.in_len >= sizeof(c.in) - 1)
                {
                    handle_request(c);
                }
            }
        }
        if (c.fd >= 0 && (c.out_len > 0 || c.body_left > 0))
        {
            flush(c);
        }
    }
}

void web_server::jpeg_begin(int stream)
{
    for (int i = 0; clients != nullptr && i < config.max_clients; i++)
    {
        web_client &c = clients[i];
        if (c.fd < 0 || c.stream != stream)
        {
            continue;
        }
        if (c.in_jpeg)
        {
            jpegs_cut++;  // the previous JPEG never ended (the caller's job, but keep the parts apart)
        }
        // Start a JPEG only for a viewer that has caught up: one with a backlog would likely be cut off.
        c.in_jpeg = c.out_len <= config.client_buffer / 4 && append(c, part_header, sizeof(part_header) - 1);
        if (c.in_jpeg)
        {
            flush(c);
        }
        else
        {
            jpegs_skipped++;
        }
    }
}

void web_server::jpeg_data(int stream, const uint8_t *data, size_t size)
{
    for (int i = 0; clients != nullptr && i < config.max_clients; i++)
    {
        web_client &c = clients[i];
        if (c.fd < 0 || c.stream != stream || !c.in_jpeg)
        {
            continue;
        }
        if (!append(c, data, size))
        {
            c.in_jpeg = false;  // too slow for this one: the rest of the JPEG is skipped
            jpegs_cut++;
            continue;
        }
        flush(c);
    }
}

void web_server::jpeg_end(int stream, bool complete)
{
    for (int i = 0; clients != nullptr && i < config.max_clients; i++)
    {
        web_client &c = clients[i];
        if (c.fd < 0 || c.stream != stream || !c.in_jpeg)
        {
            continue;
        }
        c.in_jpeg = false;
        if (complete)
        {
            jpegs_sent++;
        }
        else
        {
            jpegs_cut++;
        }
    }
}

int web_server::viewers(int stream) const
{
    int n = 0;
    for (int i = 0; clients != nullptr && i < config.max_clients; i++)
    {
        n += clients[i].fd >= 0 && clients[i].stream == stream;
    }
    return n;
}
