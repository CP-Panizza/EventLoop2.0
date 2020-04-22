//
// Created by Administrator on 2020/4/22.
//

#ifndef EL_UTILS_H
#define EL_UTILS_H



#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <map>
#include <ctime>
#include "../deps/socket/socket_header.h"

std::vector<std::string> split(std::string str, std::string pattern);

bool contain(std::string str, std::string target);

bool file_exists(const std::string &name);

bool dir_exists(std::string path);

long file_size(const char *filepath);

void trim_space(std::string &s);

std::string &replace_all(std::string &str, const std::string &old_value, const std::string &new_value);

std::string read_file(std::string file);

std::map<std::string, std::string> getConf(std::string file);

time_t getTimeStamp();

char *to4ByteChar(unsigned int n); //需要delete返回值
unsigned int byteCharToInt(const char *data);

template<class Type>
Type stringToNum(const std::string &in) {
    std::istringstream iss(in);
    Type out;
    iss >> out;
    return out;
}


std::string GetRemoTeIp(SOCKET fd);


#ifdef _WIN64

SOCKET CreateSocket(uint16_t port);
int setnonblocking(SOCKET s);
#else

int CreateSocket(uint16_t port);
int setnonblocking(int fd);
#endif


#endif //EL_UTILS_H
