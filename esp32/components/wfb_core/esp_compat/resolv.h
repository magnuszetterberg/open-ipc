// Stand-in: on glibc <resolv.h> brings in the socket definitions wifibroadcast.hpp relies on
// (INADDR_ANY, SOCK_DGRAM); here lwIP provides them (see README.md).
#pragma once
#include <netinet/in.h>
#include <sys/socket.h>
