//
// Created by Administrator on 2020/4/22.
//

#include "Service.h"
#include "../../deps/socket/socket_header.h"
#include <iostream>
#include <thread>
#include <inaddr.h>
#include "../../deps/rapidjson/document.h"
#include "../../deps/rapidjson/writer.h"
#include "../../deps/rapidjson/stringbuffer.h"
#include "../../utils/utils.h"

std::atomic<bool> runnning(true);

Service::Service() {

}

/**
 * recv data:[4][...]
 * 前四个字节保存数据长度，之后的为json数据
 */

void Service::handle(Event *ev) {
    int n = recv(ev->fd, ev->buff, sizeof(ev->buff), 0);
    if (n > 0) {
        ev->len = n;
        ev->buff[n] = '\0';
        printf("recv: %s\n", ev->buff + 4);
        int content_len = 0;
        if (n > 4) {
            content_len = byteCharToInt(ev->buff);
        }
        char *p = ev->buff + 4;
        if (static_cast<int>(strlen(p)) != content_len) {
            std::cout << "[ERROR]>> recv data length not match" << std::endl;
            ev->RemoveAndClose();
            return;
        }

        rapidjson::Document *doc = new rapidjson::Document;
        if (doc->Parse(p).HasParseError()) {
            std::cout << "[ERROR]>> parse data err" << std::endl;
            ev->RemoveAndClose();
            delete (doc);
            return;
        } else if (doc->HasMember("Type")
                   && std::string(((*doc)["Type"]).GetString()) == "service"
                   && doc->HasMember("Op")
                   && std::string((*doc)["Op"].GetString()) == "REG"
                   && doc->HasMember("ServiceList")
                   && doc->HasMember("ServicePort")
                   && doc->HasMember("Proportion")
                   && (this->config->node_type == NodeType::Single || this->config->node_type == NodeType::Master)) {
            this->OnServiceREG(ev, doc);
        } else if (
                doc->HasMember("Op")
                && std::string(((*doc)["Op"]).GetString()) == "PULL"
                && doc->HasMember("ServiceList")
                && doc->HasMember("Type")
                && std::string(((*doc)["Type"]).GetString()) == "client"
                ) {
            this->OnClientPULL(ev, doc);
        } else if (
                doc->HasMember("Type")
                && std::string(((*doc)["Type"]).GetString()) == "slave"
                && doc->HasMember("NodeName")
                && this->config->node_type == NodeType::Master
                ) {
            this->OnSlaveConnect(ev, doc);
        }
    } else {
        if ((n < 0) && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return;
        }
        std::cout << "[Notify]>> clinet: " << ev->fd << " closed" << std::endl;
        ev->RemoveAndClose();
    }
}


void Service::collect_server(int fd, std::string remoteIp) {
    struct ServiceClientInfo info(fd, std::move(remoteIp));
    if (!count(this->service_client, info)) {
        this->service_client.push_back(info);
    }
}

void Service::ConfigAndRun(Config *_config) {
    this->config = _config;
    if (this->config->node_type == NodeType::Master) {
        this->el->tem->CreateTimeEvent(
                std::bind(&Service::proceHeartCheck, this, std::placeholders::_1),
                nullptr,
                TimeEvemtType::CERCLE,
                {},
                this->config->heart_check_time * 1000
        );
    } else if (this->config->node_type == NodeType::Slave) {
        if (this->config->master_ip.empty()) {
            std::cout << "[ERROR]>> node_type is Slave, must config master_ip" << std::endl;
            exit(-1);
        }
        std::thread t(&Service::slaveRun, this);
        t.detach();
    }

    this->el->CreateEvent(socket_fd, EventType::READ, std::bind(&Service::AcceptTcp, this, std::placeholders::_1));
    this->el->CreateEvent(http_fd, EventType::READ, std::bind(&Service::AcceptHttp, this, std::placeholders::_1));
    this->el->Run();
}


void Service::slaveRun() {
    connect_to_master();
    int len;
    char *reqData = this->initSlaveRequestData(&len);
    char temp[len];
    strcpy(temp, reqData);
    delete[] reqData;
    char buff[MAXLINE] = {0};

    if (!Service::SyncSendData(master_fd, temp, len)) {
        std::cout << "[ERROR]>> slave send data to master err!" << std::endl;
        WSACleanup();
        return;
    }

    this->el->CreateEvent(master_fd, EventType::READ, std::bind(&Service::handleMaster, this, std::placeholders::_1));
}


void Service::removeServerInfoByIp(std::string ip) {
    std::vector<std::string> empty_list;
    for (auto k_v : server_list_map) {
        k_v.second->remove_if([=](ServerInfo *n) {
            if (n->ip == ip) {
                delete n;
                return true;
            }
            return false;
        });
        if (k_v.second->empty()) {
            empty_list.push_back(k_v.first);
        }
    }

    for (auto i : empty_list) {
        server_list_map.erase(i);
    }
}

//need delete return value
char *Service::initSlaveRequestData(int *len) {
    rapidjson::StringBuffer s;
    rapidjson::Writer<rapidjson::StringBuffer> w(s);
    w.StartObject();
    w.Key("Type");
    w.String("slave");
    w.Key("NodeName");
    w.String(this->config->node_name.c_str());
    w.EndObject();
    auto data_size = 4 + s.GetLength();
    char *temp = new char[data_size];
    auto _head = to4ByteChar(static_cast<unsigned int>(s.GetLength()));
    memcpy(temp, _head, 4);
    memcpy(temp + 4, s.GetString(), data_size - 4);
    delete[] _head;
    *len = static_cast<int>(data_size);
    return temp;
}


void Service::proceHeartCheck(TimeEvent *event) {
    std::cout << "start heart check" << std::endl;
    const char *check_data = "HEART CHECK";
    char recvline[100];
    std::list<ServiceClientInfo> err_client;

    if (!service_client.empty()) {
        for (auto item : service_client) {
            bool ok = SyncSendData(item.fd, check_data, strlen(check_data));
            printf("check send data:%s\n", (ok ? "success" : "failer"));
            int len;
            bool ok2 = SyncRecvData(item.fd, recvline, sizeof(recvline), &len);
            printf("check recv data:%s\n", (ok2 ? "success" : "failer"));
            if (!ok) {
                err_client.push_back(item);
                continue;
            }

            if (!ok2) {
                err_client.push_back(item);
            }
            memset(recvline, 0, strlen(recvline));
        }

        for (auto i : err_client) {
            Event *e = this->el->GetEventByFd(i.fd);
            if (e) {
                e->Reset();
            }
            removeServerInfoByIp(i.ip);
            service_client.remove_if([&](ServiceClientInfo n) {
                return n.fd == i.fd;
            });
        }
    }
}

void Service::connect_to_master() {
    WORD dwVersion = MAKEWORD(2, 2);
    WSAData wsaData{};
    WSAStartup(dwVersion, &wsaData);
    sockaddr_in servaddr{};
    memset(&servaddr, 0, sizeof(servaddr));
    master_fd = 0;
    servaddr.sin_family = AF_INET; //网络类型
    servaddr.sin_addr.S_un.S_addr = inet_addr(this->config->master_ip.c_str());
    servaddr.sin_port = htons(SERVICE_PORT);
    if ((master_fd = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
        printf("[ERROR]>> create socket error: %s(errno: %d)\n", strerror(errno), errno);
        WSACleanup();
        exit(-1);
    }

    setnonblocking(master_fd);
    if (connect(master_fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0) {
        printf("[ERROR]>> connect socket error: %s(errno: %d)\n", strerror(errno), errno);
        WSACleanup();
        exit(-1);
    }
}


void Service::collect_slave(int fd, std::string remoteIp, std::string name) {
    SlaverInfo slaverInfo{static_cast<int>(fd), std::move(name), remoteIp, getTimeStamp()};
    if (!count(this->slavers, slaverInfo)) {
        this->slavers.push_back(slaverInfo);
    }
}


void Service::InitEL() {
    el = new EventLoop;
    el->InitApiData();
    el->InitEvents();
    el->InitTimeEvent();
}


void Service::InitSockets() {
    this->socket_fd = CreateSocket(SERVICE_PORT);  //服务注册、发现fd
    this->http_fd = CreateSocket(HTTP_PORT);
}


void Service::OnServiceREG(Event *e, rapidjson::Document *doc) {

    std::string port_str((*doc)["ServicePort"].GetString());
    std::string remoteIp = GetRemoTeIp(e->fd);
    remoteIp.append(port_str);

    strcpy(e->buff, "OK");
    e->len = 2;
    /**
     * 发送返回信息，成功就注册信息，失败就放弃并关闭连接
     */
    if (Service::SyncSendData(e->fd, e->buff, e->len)) {
        e->RemoveNotClose();
        collect_server(e->fd, remoteIp); //把服务提供方描述符放入一个列表
        /**
         * 注册服务信息
         */
        int proportion = (*doc)["Proportion"].GetInt();
        const rapidjson::Value &serverList = (*doc)["ServiceList"];
        for (int i = 0; i < serverList.Size(); ++i) {
            std::string serName(serverList[i].GetString());
            if (server_list_map.count(serName)) {
                std::list<ServerInfo *> *temp = server_list_map[serName];
                temp->push_front(new ServerInfo(remoteIp, proportion));
            } else {
                auto *templist = new std::list<ServerInfo *>;
                templist->push_front(new ServerInfo(remoteIp, proportion));
                server_list_map[serName] = templist;
            }
        }
    } else {
        e->RemoveAndClose();
    }
    delete (doc);
}


void Service::OnClientPULL(Event *e, rapidjson::Document *doc) {
    rapidjson::StringBuffer s;
    rapidjson::Writer<rapidjson::StringBuffer> w(s);
    std::map<std::string, std::list<ServerInfo *> *> temp_map;
    auto serviceList = ((*doc)["ServiceList"]).GetArray();
    for (auto it = serviceList.Begin(); it != serviceList.End(); it++) {
        std::string ser_name(it->GetString());
        if (server_list_map.count(ser_name)) {
            temp_map[ser_name] = server_list_map[ser_name];
        } else {
            std::list<ServerInfo *> tmep_list;
            temp_map[ser_name] = &tmep_list;
        }
    }
    w.StartObject();
    w.Key("data");
    w.StartArray();
    for (auto key_val : temp_map) {
        w.StartObject();
        w.Key(key_val.first.c_str());
        w.StartArray();
        for (auto &ip : *key_val.second) {
            w.StartObject();
            w.Key("Ip");
            w.String(ip->ip.c_str());
            w.Key("Proportion");
            w.Int(ip->proportion);
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    strcpy(e->buff, s.GetString());
    e->len = s.GetLength();
    delete (doc);
    if (!Service::SyncSendData(e->fd, e->buff, e->len)) {
        e->RemoveAndClose();
    }
}


void Service::OnSlaveConnect(Event *e, rapidjson::Document *doc) {
    rapidjson::StringBuffer s;
    rapidjson::Writer<rapidjson::StringBuffer> w(s);

    w.StartObject();
    w.Key("ServiceInfo");
    w.StartObject();
    for (auto l : server_list_map) {
        w.Key(l.first.c_str());
        w.StartArray();
        for (auto m : *l.second) {
            w.StartObject();
            w.Key("Ip");
            w.String(m->ip.c_str());
            w.Key("Proportion");
            w.Int(m->proportion);
            w.EndObject();
        }
        w.EndArray();
    }
    w.EndObject();

    w.Key("SlavesInfo");
    w.StartArray();
    for (auto n : slavers) {
        w.StartObject();
        w.Key("Ip");
        w.String(n.ip.c_str());
        w.Key("Name");
        w.String(n.name.c_str());
        w.Key("ConnectTime");
        w.Int64(n.connect_time);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();

    strcpy(e->buff, s.GetString());
    e->len = s.GetLength();

    if (Service::SyncSendData(e->fd, e->buff, e->len)) {
        collect_slave(e->fd, GetRemoTeIp(e->fd), std::string((*doc)["NodeName"].GetString())); //把slave放入列表
        e->RemoveNotClose();
    } else {
        e->RemoveAndClose();
    }
    delete (doc);
}


bool Service::SyncSendData(int s, const char *buf, int len) {
    int ret = -1;
    int Total = 0;
    int lenSend = 0;
    struct timeval tv{};
    tv.tv_sec = 3;
    tv.tv_usec = 500;
    fd_set wset;
    while (true) {
        FD_ZERO(&wset);
        FD_SET(s, &wset);
        if (select(0, nullptr, &wset, nullptr, &tv) > 0)//3.5秒之内可以send，即socket可以写入
        {
            lenSend = send(s, buf + Total, len - Total, 0);
            if (lenSend == -1) {
                return false;
            }
            Total += lenSend;
            if (Total == len) {
                return true;
            }
        } else  //3.5秒之内socket还是不可以写入，认为发送失败
        {
            return false;
        }
    }
}


bool Service::SyncRecvData(int s, char *buf, int size_buf, int *len) {
    struct timeval tv{};
    tv.tv_sec = 3;
    tv.tv_usec = 500;
    fd_set rset;
    while (true) {
        FD_ZERO(&rset);
        FD_SET(s, &rset);
        if (select(0, &rset, nullptr, nullptr, &tv) > 0)//3.5秒之内可以
        {
            int n = recv(s, buf, sizeof(size_buf), 0);
            if (n > 0) {
                *len = n;
                return true;
            } else {
                if ((n < 0) && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                    continue;
                }
                return false;
            }
        } else  //3.5秒之内socket还是不可以写入，认为发送失败
        {
            return false;
        }
    }
}


void Service::handleMaster(Event *ev) {
    char buff[MAXLINE] = {0};
    int len;
    if (Service::SyncRecvData(ev->fd, buff, MAXLINE, &len)) {
        if (len < 4) {
            std::cout << "master send data to slave err" << std::endl;
            exit(-1);;
        }
        int content_len = 0;
        if (len > 4) {
            content_len = byteCharToInt(buff);
        }
        char *p = buff + 4;
        if (static_cast<int>(strlen(p)) != content_len) {
            std::cout << "[ERROR]>> recv data length not match" << std::endl;
            ev->RemoveAndClose();
            return;
        }

        rapidjson::Document *doc = new rapidjson::Document;
        if (doc->Parse(p).HasParseError()) {
            std::cout << "[ERROR]>> slave parse master send data err" << std::endl;
            exit(-1);
        } else {
            const rapidjson::Value &serverInfo = (*doc)["ServiceInfo"];
            for (int i = 0; i < serverInfo.Size(); ++i) {

            }
        }
    } else {
        std::cout << "master send data to slave err" << std::endl;
        exit(-1);
    }
}


void Service::AcceptTcp(Event *e) {
    int new_fd = accept(e->fd, NULL, NULL);
    if (new_fd < 0) {
        std::cout << "[ERROR]>> accept err" << std::endl;
        return;
    }

    if (new_fd > MAX_COUNT) {
        printf("limit max count \n");
        closesocket(new_fd);
        return;
    }

    std::cout << "accept a client " << new_fd << std::endl;
    setnonblocking(new_fd);
    if (new_fd > e->el->max_fd) {
        e->el->max_fd = new_fd;
    }
    GetRemoTeIp(new_fd);
    e->el->CreateEvent(new_fd, EventType::READ, std::bind(&Service::handle, this, std::placeholders::_1));
}


void Service::AcceptHttp(Event *e) {
    int new_fd = accept(e->fd, NULL, NULL);
    if (new_fd < 0) {
        std::cout << "[ERROR]>> accept err" << std::endl;
        return;
    }
    if (new_fd > MAX_COUNT) {
        printf("limit max count \n");
        closesocket(new_fd);
        return;
    }
    std::cout << "accept a client " << new_fd << std::endl;
    setnonblocking(new_fd);
    if (new_fd > e->el->max_fd) {
        e->el->max_fd = new_fd;
    }
    e->el->CreateEvent(new_fd, EventType::READ, std::bind(&Service::handleHttp, this, std::placeholders::_1));
}

void Service::handleHttp(Event *ev) {

}


bool count(std::list<ServiceClientInfo> src, ServiceClientInfo target) {
    for (auto x : src) {
        if (x.ip == target.ip && x.fd == target.fd) {
            return true;
        }
    }
    return false;
}

bool count(std::list<SlaverInfo> src, SlaverInfo target) {
    for (auto x : src) {
        if (x.ip == target.ip && x.name == target.name) {
            return true;
        }
    }
    return false;
}