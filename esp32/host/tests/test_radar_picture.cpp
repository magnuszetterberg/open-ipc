// radar_picture: the detection on the real radar's recorded measurements and on a simulated moving
// person, and the picture's JPEG: rtp_jpeg carries it unchanged, and GStreamer's jpegdec (what receiver.sh
// uses) decodes it. The last picture is written to radar_picture.jpg in the build folder, to look at.
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "check.hpp"
#include "proc.hpp"
#include "radar_picture.hpp"
#include "rtp_jpeg.hpp"

// The recording: per measurement a 16-bit length, then the R1 payload as it came over the link
static std::vector<std::vector<uint8_t>> read_recording(const char *path)
{
    std::vector<std::vector<uint8_t>> out;
    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
    {
        return out;
    }
    uint8_t len[2];
    while (fread(len, 1, 2, fp) == 2)
    {
        std::vector<uint8_t> p(len[0] | (len[1] << 8));
        if (fread(p.data(), 1, p.size(), fp) != p.size())
        {
            break;
        }
        out.push_back(p);
    }
    fclose(fp);
    return out;
}

struct packet_list : rtp_jpeg_sink
{
    std::vector<std::vector<uint8_t>> packets;
    void send_rtp(const uint8_t *p, size_t size) override
    {
        packets.emplace_back(p, p + size);
    }
};

static radar_detector detector;
static radar_picture picture;
static uint8_t jpeg[65536];

int main()
{
    std::vector<std::vector<uint8_t>> rec = read_recording(DATA_DIR "/radar_r1.bin");
    CHECK(rec.size() == 196);
    if (rec.empty())
    {
        return check_result("test_radar_picture");
    }

    // The recording: 2 antennas, 120 taps; the direct signal at tap 6 (as pc/radar_view.py found)
    int moving = 0;
    for (size_t i = 0; i < rec.size(); i++)
    {
        CHECK(detector.add_r1(rec[i].data(), rec[i].size()));
        CHECK(detector.finding.learning == (i < 20));
        if (detector.finding.moving)
        {
            moving++;
            CHECK(detector.finding.distance_m >= 0.3f && detector.finding.distance_m <= radar_detector::MAX_M);
        }
    }
    CHECK(detector.antennas == 2 && detector.taps == 120);
    CHECK(detector.zero_tap == 6);
    printf("recording: %d of %zu measurements with something moving, last at %.2f m\n", moving, rec.size(),
           (double)detector.finding.distance_m);
    CHECK(!detector.add_r1(rec[0].data(), 7));

    // A simulated person: the room as recorded, plus a reflection at one distance that changes by 10 dB
    // between measurements, as a person's does as they move. The finding has to follow it.
    radar_detector sim;
    std::vector<uint8_t> room = rec.back();
    auto measurement = [&](int tap, int i) {
        std::vector<uint8_t> p = room;
        if (tap >= 0 && i % 2)
        {
            for (int a = 0; a < 2; a++)
            {
                for (int k = tap - 1; k <= tap + 1; k++)
                {
                    uint8_t &v = p[8 + a * 121 + 1 + k];
                    v = (uint8_t)std::min(255, v + 20);   // half dB: +10 dB
                }
            }
        }
        return p;
    };
    int i = 0;
    for (; i < 60; i++)
    {
        std::vector<uint8_t> p = measurement(-1, i);
        sim.add_r1(p.data(), p.size());
    }
    CHECK(!sim.finding.learning && !sim.finding.moving);
    const int taps[] = {6 + 15, 6 + 25, 6 + 10};   // 2.25 m, 3.75 m, 1.5 m from the direct signal
    for (int tap : taps)
    {
        for (int n = 0; n < 30; n++, i++)
        {
            std::vector<uint8_t> p = measurement(tap, i);
            sim.add_r1(p.data(), p.size());
            picture.add(sim);
        }
        float want = (tap - 6) * radar_detector::TAP_M;
        printf("simulated person at %.2f m: found %s at %.2f m\n", (double)want, sim.finding.moving ? "moving" : "nothing",
               (double)sim.finding.distance_m);
        CHECK(sim.finding.moving && fabsf(sim.finding.distance_m - want) < 0.3f);
    }

    // The picture, as JPEG: a few KB, carried unchanged by rtp_jpeg, decodable
    picture.draw(sim);
    size_t n = jpeg_encode_422(picture.y, picture.cb, picture.cr, radar_picture::WIDTH, radar_picture::HEIGHT, 80,
                               jpeg, sizeof(jpeg));
    printf("picture: %zu bytes of JPEG\n", n);
    CHECK(n > 1000 && n < 30000);
    CHECK(jpeg_encode_422(picture.y, picture.cb, picture.cr, radar_picture::WIDTH, radar_picture::HEIGHT, 80, jpeg,
                          500) == 0);
    FILE *fp = fopen("radar_picture.jpg", "wb");
    CHECK(fp != NULL);
    if (fp)
    {
        fwrite(jpeg, 1, n, fp);
        fclose(fp);
    }

    packet_list sink;
    rtp_jpeg_config cfg;
    cfg.max_packet = 444;   // the RW612's payload size
    rtp_jpeg_packetizer packetizer(sink, cfg);
    CHECK(packetizer.send_frame(jpeg, n, 0) == RTP_JPEG_OK);
    static uint8_t frame[65536], scan[65536];
    rtp_jpeg_depacketizer depacketizer(frame, sizeof(frame), scan, sizeof(scan));
    size_t got = 0;
    for (auto &p : sink.packets)
    {
        got = depacketizer.push(p.data(), p.size());
    }
    CHECK(got == n && memcmp(frame, jpeg, n) == 0);
    printf("rtp: %zu packets, the JPEG rebuilt %s\n", sink.packets.size(), got == n && memcmp(frame, jpeg, n) == 0 ? "the same" : "DIFFERENT");

    int fd;
    pid_t p = spawn({"/usr/bin/gst-launch-1.0", "-q", "filesrc", "location=radar_picture.jpg", "!", "jpegdec", "!",
                     "videoconvert", "!", "video/x-raw,format=GRAY8", "!", "fdsink"},
                    &fd);
    std::string decoded;   // read it all before waiting: the picture is more than a pipe holds
    char buf[4096];
    ssize_t got_bytes;
    while ((got_bytes = read(fd, buf, sizeof(buf))) > 0)
    {
        decoded.append(buf, got_bytes);
    }
    close(fd);
    waitpid(p, nullptr, 0);
    printf("gstreamer: %zu bytes decoded\n", decoded.size());
    CHECK(decoded.size() == (size_t)radar_picture::WIDTH * radar_picture::HEIGHT);

    return check_result("test_radar_picture");
}
