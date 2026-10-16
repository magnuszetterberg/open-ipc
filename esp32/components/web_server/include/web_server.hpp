// A small HTTP server for the base station's web view (docs/DESIGN.md, M7): a page, each stream's video as
// MJPEG (multipart/x-mixed-replace, which a browser shows in an <img>), and a status document. Plain BSD
// sockets and no ESP-IDF calls, so it builds for the ESP32 (lwIP) and for Linux, where the host tests play it.
//
// Single-threaded, like rtsp_server: the caller's loop calls poll() and the jpeg_*() calls from one task, so
// nothing is locked. Everything is allocated in start(). There is no frame buffer: each MJPEG viewer gets a
// JPEG piece by piece as it is rebuilt (rtp_jpeg_streamer). A viewer whose buffer can't take the next piece
// loses the rest of that JPEG, and joins again at the next one it has room for.
#pragma once

#include <stddef.h>
#include <stdint.h>

struct web_config
{
    uint16_t port = 80;
    int max_clients = 4;          // MJPEG viewers, page and status requests together
    size_t client_buffer = 4096;  // per client: response headers, the status document, MJPEG waiting for TCP
    // Asked before data is handed to TCP, whose buffers come from the heap: false keeps it waiting here. The
    // base station says no while RAM runs low, so viewers can't starve its radio. Null: always yes.
    bool (*may_send)(size_t size) = nullptr;
};

struct web_client;

class web_server
{
public:
    ~web_server();

    // names: each stream's name, e.g. "cam0" is http://<host>/cam0.mjpg. The array must outlive the server.
    bool start(const web_config &config, const char *const *names, int streams);

    // The page served at /. Not copied: it must outlive the server (firmware embeds it).
    void set_page(const char *html, size_t size);

    // The document served at /status.json, copied (up to status_max bytes).
    static const size_t status_max = 1024;
    void set_status(const char *json, size_t size);

    // Accepts clients, answers their requests and sends what their buffers hold. Waits up to timeout_ms.
    void poll(int timeout_ms);

    // A stream's JPEG, a piece at a time, to its MJPEG viewers. complete: the JPEG ended with EOI; otherwise
    // it was cut off (a packet lost on the way).
    void jpeg_begin(int stream);
    void jpeg_data(int stream, const uint8_t *data, size_t size);
    void jpeg_end(int stream, bool complete);

    // MJPEG viewers of a stream.
    int viewers(int stream) const;

    uint32_t jpegs_sent = 0;  // JPEGs that reached a viewer whole
    uint32_t jpegs_cut = 0;   // JPEGs a viewer got only part of: cut off on the way, or the viewer too slow
    uint32_t jpegs_skipped = 0;  // JPEGs a viewer didn't get at all: still behind with the ones before
    uint32_t refused = 0;     // connections turned away: every client slot in use

private:
    void handle_request(web_client &c);
    void respond(web_client &c, const char *status, const char *type, const char *body, size_t size, bool copy);
    void close_client(web_client &c);
    void flush(web_client &c);
    bool append(web_client &c, const void *data, size_t size);

    web_config config;
    const char *const *names = nullptr;
    int streams = 0;
    int listen_fd = -1;
    web_client *clients = nullptr;
    const char *page = nullptr;
    size_t page_size = 0;
    char status[status_max];
    size_t status_size = 0;
};
