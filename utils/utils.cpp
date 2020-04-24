//
// Created by Administrator on 2020/4/22.
//

#include "utils.h"

#include "../deps/socket/socket_header.h"
#include <dirent.h>
#include <sys/stat.h>
#include <fstream>
#include <iostream>
#include <map>
#include <ctime>
#include <vector>
#include <sstream>


#ifdef _WIN64

/**
 * keepalive 怎么通知我们异常断开的情况。这里采用 select 模式，实验环境为 XP 系统和 Win7 系统，几种情况返回值如下：
    1． 正常断开
    select 函数正常返回， recv 函数返回 0
    2． 异常断开
    a)程序异常退出，如 client 端重启，应用非正常关闭等
        select 函数正常返回， recv 函数返回 SOCKET_ERROR ， WSAGetLastError () 得到的结果为 WSAECONNRESET(10054) 。
    b)网络断开
    结果同上： select 函数正常返回， recv 函数返回 SOCKET_ERROR ， WSAGetLastError() 得到的结果为 WSAECONNRESET(10054)
 * @param socket
 * @return
 */

int socket_tcp_alive(int socket) {
    int ret = 0;

    int keep_alive = 1;
    ret = setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, (char *) &keep_alive, sizeof(keep_alive));

    if (ret == SOCKET_ERROR) {
        printf("setsockopt failed: %d \n", WSAGetLastError());
        return -1;
    }

    struct tcp_keepalive in_keep_alive = {0};
    unsigned long ul_in_len = sizeof(struct tcp_keepalive);
    struct tcp_keepalive out_keep_alive = {0};
    unsigned long ul_out_len = sizeof(struct tcp_keepalive);
    unsigned long ul_bytes_return = 0;

    in_keep_alive.onoff = 1;                    /*打开keepalive*/
    in_keep_alive.keepaliveinterval = 5000; /*发送keepalive心跳时间间隔-单位为毫秒*/
    in_keep_alive.keepalivetime = 1000;         /*多长时间没有报文开始发送keepalive心跳包-单位为毫秒*/

    ret = WSAIoctl(socket, SIO_KEEPALIVE_VALS, (LPVOID) &in_keep_alive, ul_in_len,
                   (LPVOID) &out_keep_alive, ul_out_len, &ul_bytes_return, NULL, NULL);

    if (ret == SOCKET_ERROR) {
        printf("WSAIoctl failed: %d \n", WSAGetLastError());
        return -1;
    }

    return 0;
}

//检测非阻塞socket是否关闭
bool IsSocketClosed(SOCKET clientSocket) {
    bool ret = false;
    HANDLE closeEvent = WSACreateEvent();
    WSAEventSelect(clientSocket, closeEvent, FD_CLOSE);
    DWORD dwRet = WaitForSingleObject(closeEvent, 0);
    if (dwRet == WSA_WAIT_EVENT_0)
        ret = true;
    else if (dwRet == WSA_WAIT_TIMEOUT)
        ret = false;

    WSACloseEvent(closeEvent);
    return ret;
}


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

int socket_tcp_alive(int socket) {
    int optval;
    socklen_t optlen = sizeof(optval);

    optval = 1;
    setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, &optval, optlen);

    optval = 5;
    setsockopt(socket, SOL_TCP, TCP_KEEPCNT, &optval, optlen);

    optval = 1;
    setsockopt(socket, SOL_TCP, TCP_KEEPIDLE, &optval, optlen);

    optval = 1;
    setsockopt(socket, SOL_TCP, TCP_KEEPINTVL, &optval, optlen);
}

bool IsSocketClosed(int clientSocket)
{
    char buff[32];
    int recvBytes = recv(clientSocket, buff, sizeof(buff), MSG_PEEK);
    int sockErr = errno;
    if( recvBytes > 0) //Get data
    return false;
    if((recvBytes == -1) && (sockErr == EWOULDBLOCK) ) //No receive data
    return false;
    return true;
}

int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

#endif

std::vector<std::string> split(std::string str, std::string pattern) {
    std::string::size_type pos;
    std::vector<std::string> result;
    str += pattern;//扩展字符串以方便操作
    auto size = static_cast<int>(str.size());

    for (int i = 0; i < size; i++) {
        pos = str.find(pattern, i);
        if (pos < size) {
            std::string s = str.substr(i, pos - i);
            result.push_back(s);
            i = static_cast<int>(pos + pattern.size() - 1);
        }
    }
    return result;
}

bool contain(std::string str, std::string target) {
    if (str == target)
        return true;
    if (str.empty())
        return false;
    if (target.empty())
        return true;
    std::string::size_type pos = str.find(target);
    return pos != std::string::npos;
}


bool file_exists(const std::string &name) {
    std::ifstream f(name.c_str());
    return f.good();
}


bool dir_exists(std::string path) {
    DIR *dir;
    if ((dir = opendir(path.c_str())) == NULL) {
        return false;
    }
    closedir(dir);
    return true;
}

long file_size(const char *filepath) {
    struct stat info{};
    stat(filepath, &info);
    int size = info.st_size;
    return size;
}

void trim_space(std::string &s) {
    int index = 0;
    if (!s.empty()) {
        while ((index = static_cast<int>(s.find(' ', index))) != std::string::npos) {
            s.erase(index, 1);
        }
    }
}

std::string &replace_all(std::string &str, const std::string &old_value, const std::string &new_value) {
    while (true) {
        std::string::size_type pos(0);
        if ((pos = str.find(old_value)) != std::string::npos) {
            str.replace(pos, old_value.length(), new_value);
        } else { break; }
    }
    return str;
}

std::string read_file(std::string file) {
    std::ifstream fin(file);
    std::stringstream buffer;
    buffer << fin.rdbuf();
    std::string str(buffer.str());
    return str;
}

std::map<std::string, std::string> getConf(std::string file) {
    std::map<std::string, std::string> conf;
    std::string data = read_file(file);
    auto v = split(data, "\n");
    for (auto x : v) {
        if (!contain(x, "#")) {
            auto str = replace_all(x, "\n", "");
            trim_space(str);
            auto k_v = split(str, "=");
            if (k_v.size() == 2) {
                conf[k_v[0]] = k_v[1];
            }
        }
    }
    return conf;
}

time_t getTimeStamp() {
    time_t t;
    time(&t);
    return t;
}

char *to4ByteChar(unsigned int n) {
    auto *buff = new char[4];
    buff[0] = static_cast<char>((n >> 24) & 0xFF);
    buff[1] = static_cast<char>((n >> 16) & 0xFF);
    buff[2] = static_cast<char>((n >> 8) & 0xFF);
    buff[3] = static_cast<char>(n & 0xFF);
    return buff;
}

unsigned int byteCharToInt(const char *data) {
    return (0xFFFFFF | (data[0] << 24)) & (0xFF00FFFF | (data[1] << 16)) & (0xFFFF00FF | (data[2] << 8)) &
           (0xFFFFFF00 | data[3]);
}


std::string GetRemoTeIp(SOCKET fd) {
    char ipAddr[INET_ADDRSTRLEN];
    struct sockaddr_in peerAddr;
    int peerLen = sizeof(peerAddr);
    if (getpeername(fd, (struct sockaddr *) &peerAddr, &peerLen) == -1) {
        printf("getpeername error\n");
        exit(0);
    }
    printf("connected peer address = %s:%d\n", inet_ntoa(peerAddr.sin_addr),
           ntohs(peerAddr.sin_port));
    return std::string(inet_ntoa(peerAddr.sin_addr));
}


#ifdef _WIN64


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


#else

int CreateSocket(uint16_t port) {
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

    printf("[INFO]>> socket_fd= %d\n", new_socket);
    return new_socket;
}

#endif
