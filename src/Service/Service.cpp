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
        /**
         * 如果是slave关闭了，就从slavers集合中删除它
         */
        Service::Remove_slave(ev->fd);
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
            this->config->pull_data_time * 1000);
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

/*
 * 初始化eventloop
 */
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
    if (SyncSendData(e->fd, e->buff, e->len)) {
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
    auto s = GetAllServiceInfoAndSlaveInfo();
    strcpy(e->buff, s.c_str());
    e->len = s.length();
    delete (doc);
    if (!SyncSendData(e->fd, e->buff, e->len)) {
        e->RemoveAndClose();
    }
}


void Service::OnSlavePULL(Event *e, rapidjson::Document *doc) {
    std::string node_name = std::string((*doc)["NodeName"].GetString());
    std::string ip = GetRemoTeIp(e->fd);
    collect_slave(e->fd, ip, node_name); //把slave放入列表
    auto s = GetAllServiceInfoAndSlaveInfo();
    strcpy(e->buff, s.c_str());
    e->len = s.length();
    if (!SyncSendData(e->fd, e->buff, e->len)) {
        this->Remove_slave(node_name, ip);
        e->RemoveAndClose();
    }
    delete (doc);
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
    this->http_server->processHttp(ev);
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
    if(IsSocketClosed(master_fd)){
        //master宕机
        delete((int *)event->data[1]);
        delete[]((char *)event->data[0]);
        closesocket(master_fd);
        auto next_master = this->getNextMaster();
        if(next_master->name == this->config->node_name){ //本机是下一个master

        } else {

        }
        return;
    }
    int len = *(int *)event->data[1];
    char data[len];
    strcpy(data, (char *)event->data[0]);
    if(SyncSendData(master_fd, data, len)){
        char buff[MAXLINE];
        int recv_len;
        if(SyncRecvData(master_fd, buff, MAXLINE, &recv_len)){
            if(checkData(buff, recv_len)){
                rapidjson::Document doc;
                if (doc.Parse(buff + 4).HasParseError()) {
                    std::cout << "[ERROR]>> slave parse master send data err" << std::endl;
                    return;
                }
                //正确处理数据
                if(doc.HasMember("ServiceInfo")
                   && doc.HasMember("SlavesInfo")){
                    if(!doc["ServiceInfo"].IsArray() && !doc["SlavesInfo"].IsArray()) return;
                    const rapidjson::Value &serverInfo = doc["ServiceInfo"].GetArray();
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
                    const rapidjson::Value &slaveInfo = doc["SlavesInfo"].GetArray();
                    for (int k = 0; k < slaveInfo.Size(); ++k) {
                        auto s = slaveInfo[k].GetObject();
                        std::string ip = s["Ip"].GetString();
                        std::string name = s["Name"].GetString();
                        int64_t connect_time = s["ConnectTime"].GetInt64();
                        collect_slave(ip, name, connect_time);
                    }
                }
            } else {
                //master发送的数据错误
                return;
            }
        } else {
            //接收master数据失败
            return;
        }
    } else {
        //发送数据到master失败
        return;
    }

}

SlaverInfo * Service::getNextMaster() {
    SlaverInfo *next_slave = new SlaverInfo;

    if(!slavers.empty()){
        auto temp = slavers.front();
        next_slave->connect_time = temp.connect_time;
        next_slave->name = temp.name;
        next_slave->ip = temp.ip;
    } else {
        next_slave->name = this->config->node_name;
        return next_slave;
    }

    for(const auto &x : slavers){
        if(x.connect_time < next_slave->connect_time){
            next_slave->ip = x.ip;
            next_slave->name = x.name;
            next_slave->connect_time = x.connect_time;
        }
    }

    return next_slave;
}

const std::string Service::GetAllServiceInfoAndSlaveInfo() {
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
    return std::string(s.GetString());
}


/**
 * 通过fd删除slavers集合中的slave
 * @param fd
 */
void Service::Remove_slave(int fd) {
    this->slavers.remove_if([&](SlaverInfo s){
        if(s.fd == fd){
            return true;
        }
        return false;
    });
}


/*
 * 初始化http模块
 */
void Service::initHttpServer() {
    this->http_server = new HttpServer;
#ifdef _WIN64
    http_server->set_static_path("\\resource");
#else
    http_server->set_static_path("/resource");
#endif
    auto login = std::bind(&Service::HttpLogin, this, std::placeholders::_1, std::placeholders::_2);
    auto get_all = std::bind(&Service::HttpGetAllServer, this, std::placeholders::_1, std::placeholders::_2);
    auto del_server = std::bind(&Service::HttpDelServer, this, std::placeholders::_1, std::placeholders::_2);
    auto add_server = std::bind(&Service::HttpAddServer, this, std::placeholders::_1, std::placeholders::_2);
    auto change_server = std::bind(&Service::HttpChangeServer, this, std::placeholders::_1, std::placeholders::_2);
    http_server->H("POST", "/login", login);
    http_server->H("GET", "/getAll", get_all);
    http_server->H("DELETE", "/del", del_server);
    http_server->H("POST", "/add", add_server);
    http_server->H("POST", "/change", change_server);
}



bool Service::HttpAddServer(Request req, Response *resp) {
    rapidjson::Document doc;
    if (doc.Parse(req.body.c_str()).HasParseError()) {
        resp->write(200, "{\"success\":false}");
        return false;
    } else if (doc.HasMember("servername") && doc.HasMember("ip") && doc.HasMember("proportion")) {
        std::string server_name = doc["servername"].GetString();
        std::string ip = doc["ip"].GetString();
        int proportion;
        if(doc["proportion"].IsInt()){
            proportion = doc["proportion"].GetInt();
        }else if(doc["proportion"].IsString()){
            proportion = atoi(doc["proportion"].GetString());
        }

        if (server_list_map.count(server_name)) {
            if(!count(*server_list_map[server_name], ip)){
                server_list_map[server_name]->push_back(new ServerInfo(ip, proportion));
            }
        } else {
            auto li = new std::list<ServerInfo *>;
            li->push_back(new ServerInfo(ip, proportion));
            server_list_map[server_name] = li;
        }
        resp->write(200, "{\"success\":true}");
        return true;
    }
}

bool Service::HttpDelServer(Request req, Response *resp) {
    std::string server_name;
    std::string server_ip;
    if (req.params.count("server") && req.params.count("ip")) {
        server_name = req.params["server"];
        server_ip = req.params["ip"];
    } else {
        resp->write(404, "{\"success\":false}");
        return false;
    }
    if (server_list_map.count(server_name)) {
        server_list_map[server_name]->remove_if([=](ServerInfo* s) {
            if(s->ip == server_ip){
                delete s;
                return true;
            }
            return false;
        });
    }
    resp->write(200, "{\"success\":true}");
    return true;
}

bool Service::HttpGetAllServer(Request req, Response *resp) {
    rapidjson::StringBuffer s;
    rapidjson::Writer<rapidjson::StringBuffer> w(s);
    w.StartObject();
    w.Key("data");
    w.StartArray();
    for (auto l :server_list_map) {
        for (auto x : *l.second) {
            w.StartObject();
            w.Key("server");
            w.String(l.first.c_str());
            w.Key("ip");
            w.String(x->ip.c_str());
            w.Key("proportion");
            w.Int(x->proportion);
            w.EndObject();
        }
    }
    w.EndArray();
    w.EndObject();

    std::string json_data = s.GetString();
    resp->set_header("Content-Type", "application/json");
    resp->write(200, json_data);
    return true;
}


bool Service::HttpLogin(Request req, Response *resp) {
    rapidjson::Document doc;
    if (doc.Parse(req.body.c_str()).HasParseError()) {
        resp->write(200, "{\"success\":false}");
        return false;
    } else {
        if (doc.HasMember("username") && doc.HasMember("password")) {
            std::string pass = doc["username"].GetString();
            std::string pwd = doc["password"].GetString();
            if (pass == "root" && pwd == "123456") {
                resp->write(200, "{\"success\":true}");
                return true;
            } else {
                resp->write(200, "{\"success\":false}");
                return true;
            }
        } else {
            resp->write(200, "{\"success\":false}");
            return true;
        }
    }
}

bool Service::HttpChangeServer(Request req, Response *resp) {
    rapidjson::Document doc;
    if (doc.Parse(req.body.c_str()).HasParseError()) {
        resp->write(200, "{\"success\":false}");
        return false;
    } else {
        if (doc.HasMember("newServerName")
            && doc.HasMember("newIp")
            && doc.HasMember("newProportion")
            && doc.HasMember("oldServerName")
            && doc.HasMember("oldIp")
            && doc.HasMember("oldProportion")) {
            std::string newServerName = doc["newServerName"].GetString();
            std::string newIp = doc["newIp"].GetString();
            int newProportion;
            if(doc["newProportion"].IsInt()){
                newProportion = doc["newProportion"].GetInt();
            }else if(doc["newProportion"].IsString()){
                newProportion = atoi(doc["newProportion"].GetString());
            }
            int oldProportion;
            if(doc["oldProportion"].IsInt()){
                oldProportion = doc["oldProportion"].GetInt();
            }else if(doc["oldProportion"].IsString()){
                oldProportion = atoi(doc["oldProportion"].GetString());
            }
            std::string oldServerName = doc["oldServerName"].GetString();
            std::string oldIp = doc["oldIp"].GetString();
            if(server_list_map.count(oldServerName)){
                for(ServerInfo *x : *server_list_map[oldServerName]){
                    if(x->ip == oldIp && x->proportion == oldProportion){
                        x->ip = newIp;
                        x->proportion = newProportion;
                        break;
                    }
                }
            }
            resp->write(200, "{\"success\":true}");
            return true;
        }
    }
}

bool count(std::list<ServerInfo*> src, std::string ip){
    for(auto x : src){
        if(x->ip == ip){
            return true;
        }
    }
    return false;
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

