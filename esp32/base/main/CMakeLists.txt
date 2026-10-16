idf_component_register(SRCS "main.cpp"
                       REQUIRES esp_timer esp_ringbuf wfb_core radio keys eth rtsp_server rtp_jpeg web_server
                       EMBED_TXTFILES "web/index.html")
