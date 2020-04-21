//
// Created by Administrator on 2020/4/9.
//

#ifndef REGISTCENTER2_0_SOCKET_HEADER_H
#define REGISTCENTER2_0_SOCKET_HEADER_H
#ifndef FD_SETSIZE
#define FD_SETSIZE    10000
#endif

#ifndef MAX_COUNT
#define MAX_COUNT 10000
#endif
#ifdef _WIN64

#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include<sys/socket.h>
#include <arpa/inet.h>
#endif
#endif //REGISTCENTER2_0_SOCKET_HEADER_H
