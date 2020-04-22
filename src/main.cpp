#include <iostream>

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "../deps/EL/EventLoop.hpp"
#include "../deps/socket/socket_header.h"
#include "../utils/utils.h"
#include "Service/Service.h"



int main() {

    auto conf = getConf("conf.txt");
    std::string node_type, master_ip, node_name;
    node_type = conf.count("node_type") ? conf["node_type"] : "single";
    NodeType type = node_type == "master" ? NodeType::Master : (node_type == "slave" ? NodeType::Slave : NodeType::Single);
    master_ip = conf.count("master_ip") ? conf["master_ip"] : "";
    node_name = conf.count("node_name") ? conf["node_name"] : "";
    int64_t heart_check_time = conf.count("heart_check") ? stringToNum<int64_t >(conf["heart_check"]) : DEFAULT_HEART_CHECK_TIME;

    auto server = new Service;
    server->InitEL();
    server->InitSockets();
    server->ConfigAndRun(new Config(node_name, type, master_ip, heart_check_time));
    return 0;
}