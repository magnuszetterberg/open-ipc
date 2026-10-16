Test JPEGs made from synthetic test patterns, not camera pictures, and one radar recording.

| file | made with | layout |
|---|---|---|
| `test_422.jpg` | `gst-launch-1.0 videotestsrc pattern=smpte num-buffers=1 ! video/x-raw,format=Y42B,width=640,height=480 ! jpegenc quality=85 ! filesink location=test_422.jpg` | Y 2x1, Cb/Cr 1x1, two quantization tables, standard Huffman tables: the OV2640's layout (RFC 2435 type 0) |
| `test_420.jpg` | `ffmpeg -f lavfi -i testsrc2=size=320x240 -frames:v 1 -pix_fmt yuvj420p -q:v 6 -huffman default test_420.jpg` | Y 2x2, Cb/Cr 1x1, standard Huffman tables (type 1) |
| `test_chroma_1x2.jpg` | `ffmpeg -f lavfi -i testsrc2=size=640x480 -frames:v 1 -pix_fmt yuvj422p -q:v 6 -huffman default test_chroma_1x2.jpg` | Y 2x2, Cb/Cr 1x2: no RFC 2435 type carries it |
| `test_optimal_huffman.jpg` | `ffmpeg -f lavfi -i testsrc2=size=160x120 -frames:v 1 -pix_fmt yuvj422p -q:v 6 -huffman optimal test_optimal_huffman.jpg` | its own Huffman tables: RFC 2435 receivers wouldn't know them |
| `radar_r1.bin` | 10 s of the SR250 radar's measurements as they came over the link (pc/radar_view.py's input), each as a 16-bit length and an "R1" payload | 196 measurements, 2 antennas, 120 taps, a quiet room |
