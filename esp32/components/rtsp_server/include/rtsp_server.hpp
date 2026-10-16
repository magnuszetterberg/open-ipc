// A small RTSP server (RFC 2326) for the base station: one stream per camera, RTP/JPEG passed through
// untouched (docs/DESIGN.md: Base station out). Plain BSD sockets and no ESP-IDF calls, so it builds
// for the ESP32 (lwIP) and for Linux, where the host tests play it with ffmpeg.
//
// Single-threaded: the caller's loop calls poll() and send_rtp() from one task, so nothing is locked.
// Everything is allocated in start(); a client too slow to keep up loses whole packets, never part of one.
#pragma once

#include <stddef.h>
#include <stdint.h>

struct rtsp_config
{
    uint16_t port = 8554;
    int max_clients = 4;
    size_t client_buffer = 16384;  // per client: interleaved video waiting for its TCP connection
};

struct rtsp_client;

class rtsp_server
{
public:
    ~rtsp_server();

    // names: each stream's path, e.g. "cam0" is rtsp://<host>:8554/cam0. The array must outlive the server.
    bool start(const rtsp_config &config, const char *const *names, int streams);

    // Accepts clients, answers their requests and sends what their buffers hold. Waits up to timeout_ms
    // for something to happen.
    void poll(int timeout_ms);

    // One RTP packet of a stream, to every client playing it.
    void send_rtp(int stream, const uint8_t *packet, size_t size);

    // Clients playing a stream.
    int playing(int stream) const;

    uint32_t sent = 0;     // packets handed to clients
    uint32_t dropped = 0;  // packets a client's buffer had no room for

private:
    void handle_request(rtsp_client &c, const char *request);
    void close_client(rtsp_client &c);
    void flush(rtsp_client &c);

    rtsp_config config;
    const char *const *names = nullptr;
    int streams = 0;
    int listen_fd = -1;
    int udp_fd = -1;
    uint16_t udp_port = 0;
    uint32_t next_session = 0x10000;
    rtsp_client *clients = nullptr;
};
