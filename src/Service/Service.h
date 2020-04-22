//
// Created by Administrator on 2020/4/22.
//

#ifndef EL_SERVICE_H
#define EL_SERVICE_H


#include "../../deps/socket/socket_header.h"
#include "RWLock.hpp"
#include <string>
#include <map>
#include <list>
#include <atomic>
#include "../../deps/EL/EventLoop.hpp"
#include "../../deps/rapidjson/document.h"

#define MAXLINE 4096
#define SERVICE_PORT 8527
#define HTTP_PORT 8528
enum NodeType {
    Master,
    Slave,
    Single
};

struct Config {
    Config(const std::string &node_name, NodeType node_type, const std::string &master_ip, int64_t heart_check_time)
            : node_name(node_name), node_type(node_type), master_ip(master_ip), heart_check_time(heart_check_time) {}

    std::string node_name;
    NodeType node_type;
    std::string master_ip;
    int64_t heart_check_time; //单位s
};

//保存服务提供者socket_fd和远端服务ip
struct ServiceClientInfo {
    ServiceClientInfo(int _fd, std::string _ip):fd(_fd), ip(_ip){}
    int fd;
    std::string ip; //exp:127.0.0.1:8520
};

//保存服务注册信息
struct ServerInfo {
    ServerInfo(std::string _ip, int _proportion) : ip(_ip), proportion(_proportion) {}

    std::string ip;  //exp: 127.0.0.1:8888
    int proportion;
};

struct SlaverInfo {
    int fd;
    std::string name; //slave名字
    std::string ip; //slave ip
    time_t connect_time; //slave连接上master的时间
    SlaverInfo(int fd, const std::string &name, const std::string &ip, time_t connect_time) : fd(fd), name(name),
                                                                                              ip(ip), connect_time(
                    connect_time) {}

    SlaverInfo(const std::string &name, const std::string &ip, time_t connect_time) : name(name), ip(ip),
                                                                                      connect_time(
                                                                                              connect_time) {}
};



class Service {
public:
    Service();

    void InitSockets();

    void InitEL();

    void ConfigAndRun(Config *);

    void AcceptTcp(Event *e);

    void AcceptHttp(Event *e);

    void handleHttp(Event *ev);

    void removeServerInfoByIp(std::string ip);

    void slaveRun();

    void handleMaster(Event *ev);

    char *initSlaveRequestData(int *len);

    void proceHeartCheck(TimeEvent *event);

    void connect_to_master();

    void handleTcp(Event *ev);

    bool SyncSendData(int s,const char *buf,int len);

    bool SyncRecvData(int s, char *buf, int size_buf, int *len);

    void collect_server(int fd, std::string remoteIp);

    void collect_slave(int fd, std::string remoteIp, std::string name);

    void collect_slave(std::string remoteIp, std::string name, int64_t connect_time);

    void OnServiceREG(Event *e, rapidjson::Document *doc);

    void OnClientPULL(Event *e, rapidjson::Document *doc);

    void OnSlaveConnect(Event *e, rapidjson::Document *doc);

    const std::string GetAllSlaveInfo();

    void BroadCastToAllSlave(TimeEvent *);
private:
    typedef std::map<std::string, std::list<struct ServerInfo *> *> Server_map;
    RWLock lock;
    Server_map server_list_map;
    std::list<struct ServiceClientInfo> service_client; //服务提供者信息
    std::list<SlaverInfo> slavers;
    Config *config;
    EventLoop *el;
    int socket_fd;
    int http_fd;
    int master_fd;
};

extern std::atomic<bool> runnning;

bool count(std::list<ServiceClientInfo> src, ServiceClientInfo target);

bool count(std::list<SlaverInfo> src, SlaverInfo target);


#endif //EL_SERVICE_H
