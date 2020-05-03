#include <iostream>

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "../deps/EL/EventLoop.hpp"
#include "../deps/socket/socket_header.h"
#include "../utils/utils.h"
#include "Service/Service.h"


#define DEFAULT_HEART_CHECK_TIME 30
#define DEFAULT_PULL_DATA_TIME 20

int main() {

    /**
     * 读取并解析配置文件
     */
    auto conf = getConf("EL_SERVICE.conf");
    std::string node_type, master_ip, node_name;
    node_type = conf.count("node_type") ? conf["node_type"] : "single";
    NodeType type = node_type == "master" ? NodeType::Master : (node_type == "slave" ? NodeType::Slave : NodeType::Single);
    master_ip = conf.count("master_ip") ? conf["master_ip"] : "";
    node_name = conf.count("node_name") ? conf["node_name"] : "";
    int64_t heart_check_time = conf.count("heart_check") ? stringToNum<int64_t >(conf["heart_check"]) : DEFAULT_HEART_CHECK_TIME;
    int64_t pull_data_time = conf.count("pull_data_time") ? stringToNum<int64_t>(conf["pull_data_time"]) : DEFAULT_PULL_DATA_TIME;

    auto server = new Service;
    /**
     * 初始化eventloop，包括io事件，定时器事件
     */
    server->InitEL();

    /**
     * 初始化socket，tcp，http
     */
    server->InitSockets();

    /**
     * 初始化http接口
     */
    server->initHttpServer();
    /**
     * 构造配置项，依据配置启动相应的服务
     */
    server->ConfigAndRun(new Config(node_name, type, master_ip, heart_check_time, pull_data_time));
    return 0;
}