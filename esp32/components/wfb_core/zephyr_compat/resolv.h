// Stand-in: on glibc <resolv.h> brings in the socket definitions wifibroadcast.hpp relies on
// (INADDR_ANY, SOCK_DGRAM); here Zephyr's networking provides them (see README.md).
#pragma once
#include <zephyr/net/net_ip.h>
