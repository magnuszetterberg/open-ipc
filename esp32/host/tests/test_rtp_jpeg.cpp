// RFC 2435 RTP/JPEG: what jpeg_parse accepts, packet layout, packetize -> depacketize, loss, and a real
// decoder: GStreamer's rtpjpegdepay ! jpegdec (what receiver.sh uses) must decode our packets to the
// same pixels as the original JPEG.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "check.hpp"
#include "proc.hpp"
#include "rtp_jpeg.hpp"

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

struct packet_list : rtp_jpeg_sink
{
    std::vector<std::vector<uint8_t>> packets;
    void send_rtp(const uint8_t *p, size_t size) override
    {
        packets.emplace_back(p, p + size);
    }
};

struct udp_out : rtp_jpeg_sink
{
    int fd, port;
    void send_rtp(const uint8_t *p, size_t size) override
    {
        udp_send(fd, port, p, size);
        usleep(100);
    }
};

static size_t offset_of(const std::vector<uint8_t> &p)
{
    return (size_t)p[13] << 16 | p[14] << 8 | p[15];
}

static void check_parse()
{
    std::vector<uint8_t> j422 = read_file(DATA_DIR "/test_422.jpg");
    std::vector<uint8_t> j420 = read_file(DATA_DIR "/test_420.jpg");
    std::vector<uint8_t> optimal = read_file(DATA_DIR "/test_optimal_huffman.jpg");
    std::vector<uint8_t> chroma_1x2 = read_file(DATA_DIR "/test_chroma_1x2.jpg");
    CHECK(!j422.empty() && !j420.empty() && !optimal.empty() && !chroma_1x2.empty());

    jpeg_info info;
    CHECK(jpeg_parse(j422.data(), j422.size(), &info) == RTP_JPEG_OK);
    CHECK(info.width == 640 && info.height == 480 && info.type == 0);
    CHECK(info.scan > j422.data() && info.scan + info.scan_size == j422.data() + j422.size() - 2);  // up to EOI

    CHECK(jpeg_parse(j420.data(), j420.size(), &info) == RTP_JPEG_OK);
    CHECK(info.width == 320 && info.height == 240 && info.type == 1);

    // Huffman tables RFC 2435 receivers wouldn't know: refused, not sent wrong
    CHECK(jpeg_parse(optimal.data(), optimal.size(), &info) == RTP_JPEG_UNSUPPORTED);
    // Chroma sampled 1x2 (ffmpeg's yuvj422p): no RFC 2435 type for it
    CHECK(jpeg_parse(chroma_1x2.data(), chroma_1x2.size(), &info) == RTP_JPEG_UNSUPPORTED);

    // A camera buffer that runs past EOI is fine
    std::vector<uint8_t> padded = j422;
    padded.insert(padded.end(), 100, 0);
    CHECK(jpeg_parse(padded.data(), padded.size(), &info) == RTP_JPEG_OK);
    CHECK(info.scan + info.scan_size == padded.data() + j422.size() - 2);

    std::vector<uint8_t> not_jpeg(100, 0x55);
    CHECK(jpeg_parse(not_jpeg.data(), not_jpeg.size(), &info) == RTP_JPEG_NOT_JPEG);
    CHECK(jpeg_parse(j422.data(), 300, &info) == RTP_JPEG_NOT_JPEG);  // cut short before the scan
}

static void check_packets_and_round_trip()
{
    std::vector<uint8_t> jpeg = read_file(DATA_DIR "/test_422.jpg");
    jpeg_info original;
    CHECK(jpeg_parse(jpeg.data(), jpeg.size(), &original) == RTP_JPEG_OK);

    packet_list out;
    rtp_jpeg_config config;  // max_packet 1448: wfb_tx's max_payload
    rtp_jpeg_packetizer packetizer(out, config);
    CHECK(packetizer.send_frame(jpeg.data(), jpeg.size(), 90000) == RTP_JPEG_OK);
    CHECK(out.packets.size() > 1);

    size_t expect_offset = 0;
    for (size_t i = 0; i < out.packets.size(); i++)
    {
        const std::vector<uint8_t> &p = out.packets[i];
        CHECK(p.size() <= config.max_packet);
        CHECK(p[0] == 0x80 && (p[1] & 0x7f) == 26);
        CHECK(((p[1] & 0x80) != 0) == (i + 1 == out.packets.size()));  // marker on the last packet only
        CHECK((uint16_t)(p[2] << 8 | p[3]) == i);
        CHECK(offset_of(p) == expect_offset);
        CHECK(p[16] == 0 && p[17] == 255 && p[18] == 640 / 8 && p[19] == 480 / 8);  // type 0, Q 255, 80x60
        size_t data = p.size() - 20 - (i == 0 ? 132 : 0);
        expect_offset += data;
    }
    CHECK(expect_offset == original.scan_size);

    // Back together: a JPEG with the same size, sampling, quantization tables and scan data
    static uint8_t frame[65536], scan[65536];
    rtp_jpeg_depacketizer depacketizer(frame, sizeof(frame), scan, sizeof(scan));
    size_t size = 0;
    for (const std::vector<uint8_t> &p : out.packets)
    {
        size = depacketizer.push(p.data(), p.size());
    }
    CHECK(size > 0);
    jpeg_info rebuilt;
    CHECK(jpeg_parse(frame, size, &rebuilt) == RTP_JPEG_OK);
    CHECK(rebuilt.width == 640 && rebuilt.height == 480 && rebuilt.type == 0);
    CHECK(memcmp(rebuilt.qtable[0], original.qtable[0], 64) == 0 && memcmp(rebuilt.qtable[1], original.qtable[1], 64) == 0);
    CHECK(rebuilt.scan_size == original.scan_size && memcmp(rebuilt.scan, original.scan, original.scan_size) == 0);

    // A lost packet drops that frame only; the next one comes through
    out.packets.clear();
    CHECK(packetizer.send_frame(jpeg.data(), jpeg.size(), 93000) == RTP_JPEG_OK);
    CHECK(packetizer.send_frame(jpeg.data(), jpeg.size(), 96000) == RTP_JPEG_OK);
    size_t per_frame = out.packets.size() / 2;
    int frames = 0;
    for (size_t i = 0; i < out.packets.size(); i++)
    {
        if (i == 3)
        {
            continue;  // lost in the first frame
        }
        frames += depacketizer.push(out.packets[i].data(), out.packets[i].size()) > 0;
    }
    CHECK(per_frame > 3 && frames == 1 && depacketizer.frames_dropped == 1);

    // Limits
    rtp_jpeg_config tiny = config;
    tiny.max_packet = 100;
    rtp_jpeg_packetizer small(out, tiny);
    CHECK(small.send_frame(jpeg.data(), jpeg.size(), 0) == RTP_JPEG_BAD_CONFIG);
}

// GStreamer decodes the JPEG file directly, and our RTP packets through rtpjpegdepay: same pixels.
static void check_gstreamer()
{
    std::string tmp = "/tmp/test_rtp_jpeg_" + std::to_string(getpid());
    std::string direct = tmp + "_direct.yuv", via_rtp = tmp + "_rtp.yuv";
    const char *raw = "video/x-raw,format=I420";
    int log_fd;
    pid_t p = spawn({"/usr/bin/gst-launch-1.0", "-q", "filesrc", "location=" DATA_DIR "/test_422.jpg", "!", "jpegdec", "!",
                     "videoconvert", "!", raw, "!", "filesink", "location=" + direct},
                    &log_fd);
    usleep(500 * 1000);
    stop(p, log_fd, 0);  // signal 0: just wait for it to finish
    waitpid(p, nullptr, 0);

    int port = free_port();
    p = spawn({"/usr/bin/gst-launch-1.0", "-q", "-e", "udpsrc", "port=" + std::to_string(port),
               "caps=application/x-rtp,media=video,encoding-name=JPEG,payload=26,clock-rate=90000", "!",
               "rtpjpegdepay", "!", "jpegdec", "!", "videoconvert", "!", raw, "!", "filesink", "location=" + via_rtp},
              &log_fd);
    std::vector<uint8_t> jpeg = read_file(DATA_DIR "/test_422.jpg");
    int send_port = 0;
    udp_out out;
    out.fd = udp_socket(&send_port);
    out.port = port;
    rtp_jpeg_packetizer packetizer(out, rtp_jpeg_config());
    const int frames = 3;
    for (int i = 0; i < frames; i++)
    {
        CHECK(packetizer.send_frame(jpeg.data(), jpeg.size(), 3000 * i) == RTP_JPEG_OK);
        usleep(30 * 1000);
    }
    usleep(500 * 1000);
    stop(p, log_fd, SIGINT);  // -e: SIGINT ends the stream cleanly, so filesink writes everything
    close(out.fd);

    std::vector<uint8_t> a = read_file(direct), b = read_file(via_rtp);
    const size_t frame_size = 640 * 480 * 3 / 2;
    CHECK(a.size() == frame_size);
    CHECK(b.size() >= frame_size && b.size() % frame_size == 0);
    CHECK(b.size() >= frame_size && memcmp(a.data(), b.data(), frame_size) == 0);
    printf("gstreamer: %zu of %d frames decoded from RTP, first one %s the JPEG decoded directly\n",
           b.size() / frame_size, frames, b.size() >= frame_size && memcmp(a.data(), b.data(), frame_size) == 0 ? "identical to" : "DIFFERENT from");
    unlink(direct.c_str());
    unlink(via_rtp.c_str());
}

int main()
{
    check_parse();
    check_packets_and_round_trip();
    check_gstreamer();
    return check_result("test_rtp_jpeg");
}
