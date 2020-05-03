#ifndef EVENT_LOOP_HPP
#define EVENT_LOOP_HPP

#include "../socket/socket_header.h"
#include <functional>
#include "TimeEvent.hpp"


#ifndef MAX_COUNT
#define MAX_COUNT 1024
#endif

class Event;
class EventLoop;

#ifdef _WIN64
typedef struct {
    fd_set read_fd;
    fd_set write_fd;
    fd_set _read_fd;
    fd_set _write_fd;
} ApiData;

#else
typedef struct{
    int elfd;
    struct epoll_event *ep_events;
} ApiData;

#endif

typedef std::function<void(Event *)> CallBack;

enum EventType{
    READ,
    WRITE
};

enum EventStatu{
    FREE,
    USING
};

class FireEvent{
public:
    int fd;
};


class Event{
public:
    int fd;
    ApiData *src_fd;
    EventType type;
    EventStatu statu;
    CallBack callBack;
    EventLoop *el;
    char buff[4096];
    int len;
#ifndef _WIN64
    void RemoveAndClose(){
        close(fd);
        epoll_ctl(src_fd->elfd, EPOLL_CTL_DEL, fd, nullptr);
        fd = -1;
        statu = EventStatu::FREE;
        callBack = nullptr;
        memset(buff, 0, 4096);
        len = 0;
    }

     void RemoveNotClose(){
        epoll_ctl(src_fd->elfd, EPOLL_CTL_DEL, fd, nullptr);
        fd = -1;
        statu = EventStatu::FREE;
        callBack = nullptr;
        memset(buff, 0, 4096);
        len = 0;
    }
#else

//关闭fd，从监听列表删除
    void RemoveAndClose(){
        closesocket(fd);
        if(type == EventType::WRITE){
            FD_CLR(fd, &this->src_fd->write_fd);
        }
        if(type == EventType::READ){
            FD_CLR(fd, &this->src_fd->read_fd);
        }
        fd = -1;
        statu = EventStatu::FREE;
        callBack = nullptr;
        memset(buff, 0, 4096);
        len = 0;
    }

    void RemoveNotClose(){
        if(type == EventType::WRITE){
            FD_CLR(fd, &this->src_fd->write_fd);
        }
        if(type == EventType::READ){
            FD_CLR(fd, &this->src_fd->read_fd);
        }
        fd = -1;
        statu = EventStatu::FREE;
        callBack = nullptr;
        memset(buff, 0, 4096);
        len = 0;
    }

#endif

    void Call(){
        this->callBack(this);
    }
};





class EventLoop{
public:
    int max_fd;
    Event *events;
    FireEvent *fireEvents;
    ApiData *apiData;
    TimeEventManeger *tem;
    bool running;
    EventLoop(){
        max_fd = -1;
    }

    Event *GetEventByFd(int fd){
        for (int i = 0; i < MAX_COUNT; ++i) {
            if((&events[i])->fd == fd){
                return &events[i];
            }
        }
        return nullptr;
    }

    void InitApiData(){
        apiData = new ApiData;
#ifndef _WIN64
        apiData->elfd = epoll_create(MAX_COUNT);
        apiData->ep_events = new struct epoll_event[MAX_COUNT];
#else
        FD_ZERO(&apiData->read_fd);
        FD_ZERO(&apiData->write_fd);
#endif
    }

    void InitTimeEvent(){
        tem = new TimeEventManeger;
    }

    void InitEvents(){
        events = new Event[MAX_COUNT];
        for (int i = 0; i < MAX_COUNT; ++i) {
            (&events[i])->el = this;
            (&events[i])->src_fd = apiData;
            (&events[i])->statu = EventStatu::FREE;
        }
        fireEvents = new FireEvent[MAX_COUNT];
    }

#ifndef _WIN64
    void CreateEvent(int fd, EventType type, CallBack callBack){
        Event *event = &this->events[fd];
        event->fd = fd;
        event->callBack = callBack;
        struct epoll_event ev;
        ev.data.fd = fd;
        int op;
        if(event->statu == EventStatu::FREE){
            op = EPOLL_CTL_ADD;
            event->statu = EventStatu::USING;
        } else if(event->statu == EventStatu::USING){
            op = EPOLL_CTL_MOD;
        }
        if(type == EventType::READ) {
            ev.events = EPOLLIN;
        }
        if(type == EventType::WRITE) {
            ev.events = EPOLLOUT;
        }
        epoll_ctl(this->apiData->elfd, op, fd, &ev);
    }


    int ApiPoll(timeval *tv){
        int retval = epoll_wait(this->apiData->elfd, this->apiData->ep_events, MAX_COUNT, (tv->tv_sec*1000 + tv->tv_usec/1000));
        for (int i = 0; i < retval; ++i) {
            this->fireEvents[i].fd = this->apiData->ep_events[i].data.fd;
        }
        return retval;
    }

#else

    void CreateEvent(int fd, EventType type, CallBack callBack){
        if(fd > this->max_fd){
            this->max_fd = fd;
        }
        Event *event = &this->events[fd];
        event->type = type;
        event->fd = fd;
        event->callBack = callBack;

        if(type == EventType::READ) {
            FD_CLR(fd, &this->apiData->write_fd);
            FD_SET(fd, &this->apiData->read_fd);
        }
        if(type == EventType::WRITE) {
            FD_CLR(fd, &this->apiData->read_fd);
            FD_SET(fd, &this->apiData->write_fd);
        }
        event->statu = EventStatu::USING;
    }

    int ApiPoll(timeval *tv){
        FD_ZERO(&this->apiData->_write_fd);
        FD_ZERO(&this->apiData->_read_fd);
        memcpy(&this->apiData->_write_fd, &this->apiData->write_fd, sizeof(fd_set));
        memcpy(&this->apiData->_read_fd, &this->apiData->read_fd, sizeof(fd_set));
        int retval = select(this->max_fd+1, &this->apiData->_read_fd, &this->apiData->_write_fd, nullptr, tv);

        int num = 0;
        if(retval > 0){
            for (int i = 0; i <= max_fd; ++i) {
                if((&this->events[i])->statu == EventStatu::FREE) continue;
                if((&this->events[i])->type == EventType::READ && FD_ISSET(i, &this->apiData->_read_fd)){
                    this->fireEvents[num++].fd = i;
                }

                if((&this->events[i])->type == EventType::WRITE && FD_ISSET(i, &this->apiData->_write_fd)){
                    this->fireEvents[num++].fd = i;
                }

            }
        }
        return num;
    }

#endif
    void Run(){
        running = true;
        while (running) {
            Proc();
        }
    }

    void Proc(){
        struct timeval tv;
        long now_sec, now_ms;
        TimeEvent *te = this->tem->GetNearestEvent();
        if(te){
            GetTime(&now_sec, &now_ms);
            tv.tv_sec = te->when_sec - now_sec;
            if(te->when_ms < now_ms){
                tv.tv_usec = (te->when_ms + 1000 - now_ms)*1000;
                tv.tv_sec--;
            } else {
                tv.tv_usec = (te->when_ms - now_ms)*1000;
            }
            if(tv.tv_sec < 0) tv.tv_sec = 0;
            if(tv.tv_usec < 0) tv.tv_usec = 0;
        } else {
            tv.tv_sec = 0;
            tv.tv_usec = 0;
        }

        //处理网络i/o事件
        int nfd = ApiPoll(&tv);
        int i;
        for (i = 0; i < nfd; ++i) {
            this->events[this->fireEvents[i].fd].Call();
        }
        //处理时间事件
        this->tem->ProcTimeEvent();
    }

};

#endif