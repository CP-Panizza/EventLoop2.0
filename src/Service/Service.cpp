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

void Service::handleTcp(Event *ev) {
    int n = recv(ev->fd, ev->buff, sizeof(ev->buff), 0);
    if (n > 0) {
        ev->len = n;
        ev->buff[n] = '\0';
        printf("recv: %s\n", ev->buff + 4);
        if(!checkData(ev->buff, ev->len)){
            std::cout << "[ERROR]>> recv data length not match" << std::endl;
            ev->RemoveAndClose();
            return;
        }

        rapidjson::Document *doc = new rapidjson::Document;
        if (doc->Parse(ev->buff + 4).HasParseError()) {
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
            this->OnSlavePULL(ev, doc);
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
                {this->el},
                this->config->heart_check_time * 1000
        );
    } else if (this->config->node_type == NodeType::Slave) {
        if (this->config->master_ip.empty()) {
            std::cout << "[ERROR]>> node_type is Slave, must config master_ip" << std::endl;
            exit(-1);
        }
        slaveRun();
    }

    this->el->CreateEvent(socket_fd, EventType::READ, std::bind(&Service::AcceptTcp, this, std::placeholders::_1));
    this->el->CreateEvent(http_fd, EventType::READ, std::bind(&Service::AcceptHttp, this, std::placeholders::_1));

    /**
     * 事件循环入口
     */
    this->el->Run();
}


/**
 * 每隔一段时间向master跟新一下数据
 */
void Service::slaveRun() {
    connect_to_master();
    int *len = new int;
    char *reqData = this->initSlaveRequestData(len);
    this->el->tem->CreateTimeEvent(
            std::bind(&Service::SlavePullDataFromMaster, this, std::placeholders::_1),
            nullptr,
            TimeEvemtType::CERCLE,
            {reqData, len},
            3000 * 10);
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


/**
 *每一次心跳检测远端服务提供者时，把当前节点的所有slave节点信息发送过去
 * 如果检测到服务提供者连接断开，则把此服务提供者的服务与服务信息删除并广播到所有slave进行数据同步
 */
void Service::proceHeartCheck(TimeEvent *event) {
    EventLoop *el = (EventLoop *)(event->data[0]);
    std::cout << "start heart check" << std::endl;
    std::string check_data = GetAllSlaveInfo();
    char recvline[100];
    std::list<ServiceClientInfo> err_client;

    if (!service_client.empty()) {
        for (auto item : service_client) {
            if(IsSocketClosed(item.fd)){
                printf("service %d closed!\n", item.fd);
                err_client.push_back(item);
            }else {
                bool ok = SyncSendData(item.fd, check_data.c_str(), strlen(check_data.c_str()));
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
        }

        for (auto i : err_client) {
            closesocket(i.fd);
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
    SlaverInfo slaverInfo(fd, std::move(name), remoteIp, getTimeStamp());
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
    char *row_data = new char[MAXLINE];
    int *row_data_len = new int;
    *row_data_len = e->len;
    memcpy(row_data, e->buff, e->len);
    std::string port_str((*doc)["ServicePort"].GetString());
    std::string remoteIp = GetRemoTeIp(e->fd);
    remoteIp.append(port_str);
    std::string slaveInfo = GetAllSlaveInfo();
    strcpy(e->buff, slaveInfo.c_str());
    e->len = strlen(slaveInfo.c_str());
    /**
     * 发送返回信息，成功就注册信息，失败就放弃并关闭连接
     */
    if (Service::SyncSendData(e->fd, e->buff, e->len)) {
        collect_server(e->fd, remoteIp); //把服务提供方描述符放入一个列表
        e->RemoveNotClose();

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
    delete(doc);
    delete[](row_data);
    delete(row_data_len);
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


void Service::OnSlavePULL(Event *e, rapidjson::Document *doc) {
    rapidjson::StringBuffer s;
    rapidjson::Writer<rapidjson::StringBuffer> w(s);

    w.StartObject();
    w.Key("ServiceInfo");
    w.StartArray();
    for (auto l : server_list_map) {
        w.StartObject();
        w.Key("ServerName");
        w.String(l.first.c_str());
        w.Key("List");
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
        w.EndObject();
    }
    w.EndArray();

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

    std::string node_name = std::string((*doc)["NodeName"].GetString());
    std::string ip = GetRemoTeIp(e->fd);

    if (Service::SyncSendData(e->fd, e->buff, e->len)) {
        collect_slave(e->fd, ip, node_name); //把slave放入列表
    } else {
        this->Remove_slave(node_name, ip);
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
            exit(-1);
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
            SyncSendData(master_fd, "OK", 2);
            return;
        } else {
            if(doc->HasMember("ServiceInfo")
            && doc->HasMember("SlavesInfo")){
                const rapidjson::Value &serverInfo = (*doc)["ServiceInfo"].GetArray();
                for (int i = 0; i < serverInfo.Size(); ++i) {
                    auto item = serverInfo[i].GetObject();
                    if(item.HasMember("ServerName") && item.HasMember("List")){
                        const std::string server_name = item["ServerName"].GetString();
                        auto server_list = item["List"].GetArray();
                        for (int j = 0; j < server_list.Size(); ++j) {
                            auto info = server_list[j].GetObject();
                            const std::string ip = info["Ip"].GetString();
                            int proportion = info["Proportion"].GetInt();
                            if (server_list_map.count(server_name)) {
                                std::list<ServerInfo *> *temp = server_list_map[server_name];
                                temp->push_front(new ServerInfo(ip, proportion));
                            } else {
                                auto *templist = new std::list<ServerInfo *>;
                                templist->push_front(new ServerInfo(ip, proportion));
                                server_list_map[ip] = templist;
                            }
                        }
                    }
                }

                const rapidjson::Value &slaveInfo = (*doc)["SlavesInfo"].GetArray();
                for (int k = 0; k < slaveInfo.Size(); ++k) {
                    auto s = slaveInfo[k].GetObject();
                    std::string ip = s["Ip"].GetString();
                    std::string name = s["Name"].GetString();
                    int64_t connect_time = s["ConnectTime"].GetInt64();
                    collect_slave(ip, name, connect_time);
                }

                SyncSendData(master_fd, "OK", 2);
            } else if(doc->HasMember("Op")
            && std::string((*doc)["Op"].GetString()) == "Del"
            && doc->HasMember("ServiceInfo")){

                SyncSendData(master_fd, "OK", 2);
            }
        }
    } else {
        std::cout << "master send data to slave err" << std::endl;
        //选举master
        //重启
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

    std::cout << "accept a tcp client " << new_fd << std::endl;
    setnonblocking(new_fd);
    if (new_fd > e->el->max_fd) {
        e->el->max_fd = new_fd;
    }
    e->el->CreateEvent(new_fd, EventType::READ, std::bind(&Service::handleTcp, this, std::placeholders::_1));
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
    std::cout << "accept a http client " << new_fd << std::endl;
    setnonblocking(new_fd);
    if (new_fd > e->el->max_fd) {
        e->el->max_fd = new_fd;
    }
    e->el->CreateEvent(new_fd, EventType::READ, std::bind(&Service::handleHttp, this, std::placeholders::_1));
}

void Service::handleHttp(Event *ev) {

}

const std::string Service::GetAllSlaveInfo() {
    rapidjson::StringBuffer s;
    rapidjson::Writer<rapidjson::StringBuffer> w(s);
    w.StartObject();
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
    return std::string(s.GetString());
}

void Service::collect_slave(std::string remoteIp, std::string name, int64_t connect_time) {
    for(auto i : this->slavers){
        if(i.name == name && i.ip == remoteIp){
            return;
        }
    }
    slavers.push_back(SlaverInfo(name, remoteIp, connect_time));
}



/**
 *
 * @param data
 * @param data_len
 * @return bool
 * 检查接收的数据是否正确,正确返回true
 */
bool Service::checkData(char *data, int data_len) {
    int content_len = 0;
    if (data_len > 4) {
        content_len = byteCharToInt(data);
    } else {
        return false;
    }
    char *p = data + 4;
    if (static_cast<int>(strlen(p)) != content_len) {
        return false;
    }
    return true;
}

void Service::Remove_slave(std::string name, std::string ip) {
    this->slavers.remove_if([&](SlaverInfo s){
        if(s.ip == ip && s.name == name){
            return true;
        }
        return false;
    });
}


void Service::SlavePullDataFromMaster(TimeEvent *event) {

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

