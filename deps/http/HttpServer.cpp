//
// Created by Administrator on 2020/4/1.
//

#include "HttpServer.h"
#include "../../utils/utils.h"
#include <stdlib.h>
#include <iostream>
#include "Request.h"
#include "Response.h"
#include "../socket/socket_header.h"

#ifndef MAXLINE
#define MAXLINE 4096
#endif

HttpServer::HttpServer() {

}

void HttpServer::processHttp(Event *ev) {
    Request request;
    char buf[MAXLINE] = {0};
    int n;
    n = recv(ev->fd, buf, MAXLINE, 0);
    if (n == SOCKET_ERROR || n == 0) {
        ev->RemoveAndClose();
        return;
    }
    buf[n] = '\0';
    try {
        request.Paser(std::string(buf));
    } catch (std::string err) {
        std::cout << err << std::endl;
        ev->RemoveAndClose();
        return;
    }

    std::cout << "DEBUG [" << request.method << "-----" << request.url << "]" << std::endl;
    if (request.header.count("Content-Length")) {
        long content_length = atol(request.header["Content-Length"].c_str());
        while (request.body.size() < content_length) {
            char buff[MAXLINE];
            int len;
            len = recv(ev->fd, buff, MAXLINE, 0);
            if (len < 0) {
                ev->RemoveAndClose();
                return;
            }
            buff[len] = '\0';
            request.body += std::string(buff);
        }
    }

    Response response(ev->fd);
    if (request.method == "GET" && excute_pwd != "" && this->static_path != "") {
        std::string dir = excute_pwd + static_path + request.path;
#ifdef _WIN64
        dir = replace_all(dir, "/", "\\");
        std::string index = dir + "\\index.html";
#else
        auto v = split(dir, "/");
        std::string Dir;
        for (int i = 0; i < v.size(); ++i) {
            if (v[i] != "") {
                Dir += "/" + v[i];
            }
        }
        std::string index = Dir + "/index.html";
#endif
        if (file_exists(index)) {
            response.send_file(index);
            return;
        } else if (file_exists(dir)) {
            response.send_file(dir);
            return;
        }
    }

    bool ok = false;
    std::function<bool(Request, Response *)> temp_handle;
    if (methods.count(request.method)) {
        auto l = methods[request.method];
        if (l->size() != 0) {
            for (auto h : *l) {
                if (h->url == request.path) {
                    temp_handle = h->method;
                    ok = true;
                    break;
                }
            }
        }
    }

    if (ok) {
        bool done = temp_handle(request, &response);
        if(!done){
            ev->RemoveAndClose();
        }
    } else {
        response.set_header("Content-Type", "text/html");
        response.write(404, "Not found!");
        ev->RemoveAndClose();
    }
}

void HttpServer::H(std::string method, std::string url, std::function<bool(Request, Response *)> func) {
    auto h = new struct handle(url, func);
    if (methods.count(method)) {
        methods[method]->push_back(h);
    } else {
        auto temp = new std::list<handle *>;
        temp->push_back(h);
        methods[method] = temp;
    }
}


#ifdef _WIN64
void HttpServer::set_static_path(std::string path) {
    if (path[0] != '\\') {
        throw std::string("STATIC PATH MUST START WITH '/'");
    }
    this->static_path = path;
}



#else
void HttpServer::set_static_path(std::string path) {
    if (path[0] != '/') {
        throw std::string("STATIC PATH MUST START WITH '/'");
    }
    this->static_path = path;
}
#endif