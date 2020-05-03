//
// Created by Administrator on 2020/4/1.
//

#ifndef HTTP_WINDOWS_HTTPSERVER_H
#define HTTP_WINDOWS_HTTPSERVER_H
#include <functional>
#include <map>
#include <list>
#include "../socket/socket_header.h"
#include "Request.h"
#include "Response.h"
#include "../EL/EventLoop.hpp"

#ifndef _WIN64
#include  <sys/epoll.h>
#ifndef _MAX_COUNT_
#define MAX_COUNT 1024
#endif
#endif
struct handle{
    handle(std::string u, std::function<bool(Request, Response *)> m):url(u), method(m){}
    std::string url;
    std::function<bool(Request, Response *)> method;
};

class HttpServer {
public:
    HttpServer();

    void set_static_path(std::string);

    void H(std::string method, std::string url, std::function<bool(Request, Response*)> func);

    void processHttp(Event *ev);

private:
    std::string excute_pwd; //程序启动路径
    std::string static_path;
    std::map<std::string, std::list<handle*>*> methods;
};


#endif //HTTP_WINDOWS_HTTPSERVER_H
