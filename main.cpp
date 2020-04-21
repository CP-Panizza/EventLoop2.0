#include <iostream>

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "EventLoop.hpp"
#include "socket_header.h"

#ifdef _WIN64

int setnonblocking(SOCKET s) {
    unsigned long ul = 1;
    int ret = ioctlsocket(s, FIONBIO, &ul);//设置成非阻塞模式。
    if (ret == SOCKET_ERROR)//设置失败。
    {
        return -1;
    }
    return 1;
}

#else

int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

#endif

void Handle(Event *ev);

#ifndef _WIN64
static int CreateSocket(uint16_t port) {
    int new_socket = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (new_socket < 0) {
        std::cout << "[ERROR]>> create socket err!" << std::endl;
        exit(-1);
    }
    int reuse = 1;
    setsockopt(new_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr;
    bzero(&addr, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    int ret = bind(new_socket, (struct sockaddr *) &addr, sizeof(addr));

    if (ret < 0) {
        std::cout << "[ERROR]>> bind error" << std::endl;
        exit(-1);
    }

    ret = listen(new_socket, 16);

    if (ret < 0) {
        std::cout << "[ERROR]>> listen error" << std::endl;
        exit(-1);
    }

    printf("socket_fd= %d\n", new_socket);
    return new_socket;
}
#else


SOCKET CreateSocket(uint16_t port) {
    WORD dwVersion = MAKEWORD(2, 2);
    WSAData wsaData{};
    WSAStartup(dwVersion, &wsaData);
    sockaddr_in servaddr{};
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET; //网络类型
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port); //端口
    SOCKET new_socket;
    if ((new_socket = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
        printf("[ERROR]>> create socket error: %s(errno: %d)\n", strerror(errno), errno);
        WSACleanup();
        exit(-1);
    }

    bool bReAddr = true;
    if (SOCKET_ERROR == (setsockopt(new_socket, SOL_SOCKET, SO_REUSEADDR, (char *) &bReAddr, sizeof(bReAddr)))) {
        std::cout << "[ERROR]>> set resueaddr socket err!" << std::endl;
        WSACleanup();
        exit(-1);
    }

    if (bind(new_socket, (struct sockaddr *) &servaddr, sizeof(servaddr)) == INVALID_SOCKET) {
        printf("[ERROR]>> bind socket error: %s(errno: %d)\n", strerror(errno), errno);
        WSACleanup();
        exit(-1);
    }

    //监听，设置最大连接数10
    if (listen(new_socket, 10) == INVALID_SOCKET) {
        printf("[ERROR]>> listen socket error: %s(errno: %d)\n", strerror(errno), errno);
        WSACleanup();
        exit(-1);
    }

    if (setnonblocking(new_socket) == -1) {
        printf("[ERROR]>> set socket_fd nnonblock err");
        exit(-1);
    }
    return new_socket;
}

#endif

//void Echo(Event *ev){
//    int ret = -1;
//    int Total = 0;
//    int lenSend = 0;
//    struct timeval tv{};
//    tv.tv_sec = 3;
//    tv.tv_usec = 500;
//    fd_set wset;
//    while(true)
//    {
//        FD_ZERO(&wset);
//        FD_SET(ev->fd, &wset);
//        if(select(ev->el->max_fd + 1, nullptr, &wset, nullptr, &tv) > 0)//3.5秒之内可以send，即socket可以写入
//        {
//            lenSend = send(ev->fd, ev->buff + Total, ev->len - Total,0);
//            if(lenSend == -1)
//            {
//                printf("send -1 closed %d\n",ev->fd);
//                ev->Reset();
//                break;
//            }
//            Total += lenSend;
//            if(Total == ev->len)
//            {
//                printf("send success %d\n", ev->fd);
//                ev->el->CreateEvent(ev->fd, EventType::READ, HandleHello);
//                break;
//            }
//        }
//
//        else  //3.5秒之内socket还是不可以写入，认为发送失败
//        {
//            printf("send timeout closed %d\n", ev->fd);
//            ev->Reset();
//            break;
//        }
//    }
//}



void SendHTML(Event *ev){
    std::string html = R"(HTTP/1.1 200 OK
Content-Type:text/html
Content-Length:27

<html><h1>Hello</h1></html>)";
    int ret = -1;
    int Total = 0;
    int lenSend = 0;
    struct timeval tv{};
    tv.tv_sec = 3;
    tv.tv_usec = 500;
    fd_set wset;
    while(true)
    {
        FD_ZERO(&wset);
        FD_SET(ev->fd, &wset);
        if(select(ev->el->max_fd + 1, nullptr, &wset, nullptr, &tv) > 0)//3.5秒之内可以send，即socket可以写入
        {
            lenSend = send(ev->fd, html.c_str() + Total, html.length() - Total,0);
            if(lenSend == -1)
            {
                printf("send -1 closed %d\n",ev->fd);
                ev->Reset();
                break;
            }
            Total += lenSend;
            if(Total == html.length())
            {
                printf("send success %d\n", ev->fd);
                ev->el->CreateEvent(ev->fd, EventType::READ, Handle);
                break;
            }
        }

        else  //3.5秒之内socket还是不可以写入，认为发送失败
        {
            printf("send timeout closed %d\n", ev->fd);
            ev->Reset();
            break;
        }
    }
}

void Handle(Event *ev){
    int n = recv(ev->fd, ev->buff, sizeof(ev->buff), 0);
    if(n > 0){
        ev->len = n;
        ev->buff[n] = '\0';
        printf("recv from: %d: \n%s\n",ev->fd,  ev->buff);
        ev->el->CreateEvent(ev->fd, EventType::WRITE, SendHTML);
    } else {
        if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)){
            return;
        } else {
            std::cout << "closed " << ev->fd << std::endl;
            ev->Reset();
        }
    }
}



void Accept(Event *e){
    int new_fd = accept(e->fd, NULL, NULL);
    if (new_fd < 0) {
        std::cout << "[ERROR]>> accept err" << std::endl;
        return;
    }

    if(new_fd > MAX_COUNT){
        printf("limit max count \n");
        closesocket(new_fd);
        return;
    }
    std::cout << "accept a client " << new_fd << std::endl;
    setnonblocking(new_fd);
    if(new_fd > e->el->max_fd){
        e->el->max_fd = new_fd;
    }
    e->el->CreateEvent(new_fd, EventType::READ, Handle);
}



int main() {
    EventLoop *el = new EventLoop;
    el->InitApiData();
    el->InitEvents();
    el->InitTimeEvent();
    int new_fd = CreateSocket(8888);
    el->CreateEvent(new_fd, EventType::READ, Accept);

    el->Run();
    return 0;
}