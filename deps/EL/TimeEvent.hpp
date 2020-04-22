

#ifndef TIME_EVENT_HPP
#define TIME_EVENT_HPP
#include <functional>
#include <sys/time.h>
#include <list>
#include <vector>
#ifndef pvoid
#define pvoid void *
#endif

enum TimeEvemtType{
    ONCE,     //定时类型
    CERCLE    //循环类型
};

class TimeEvent{
public:
    typedef std::function<void(TimeEvent *)> TimeEventCallBack;
    typedef std::function<void(TimeEvent *)> TimeEventDestroyCallBack;

    int id;
    TimeEventCallBack callBack;
    TimeEventDestroyCallBack destroyCallBack;
    // 事件的到达时间
    long when_sec; /* seconds */
    long when_ms; /* milliseconds */
    TimeEvemtType type;
    std::vector<pvoid> data;
    long long cercleTime;


    TimeEvent(int id, const TimeEventCallBack &callBack, const TimeEventDestroyCallBack &destroyCallBack, long when_sec,
              long when_ms, TimeEvemtType type, const std::vector<void *> &data, long long int cercleTime)
            : id(id), callBack(callBack), destroyCallBack(destroyCallBack), when_sec(when_sec), when_ms(when_ms),
              type(type), data(data), cercleTime(cercleTime) {}


    void Call(){
        callBack(this);
    }

    void Destroy(){
        destroyCallBack(this);
    }
};





static void GetTime(long *seconds, long *milliseconds);
static void AddMillisecondsToNow(long long milliseconds, long *sec, long *ms);

class EventNode{
public:
    TimeEvent *event;
    EventNode *next;

    EventNode(){
        event = nullptr;
        next = nullptr;
    }

    void Add(TimeEvent *item){
        auto temp = this;
        while(temp->next != nullptr){
            temp = temp->next;
        }
        auto node = new EventNode;
        node->event = item;
        temp->next = node;
    }

    void Del(int id){
        auto temp = this;
        while(temp->next != nullptr){
            if(temp->next->event->id == id){
                auto t = temp->next;
                temp->next = temp->next->next;
                if(t->event->destroyCallBack){
                    t->event->Destroy();
                }
                delete(t);
                return;
            }
            temp = temp->next;
        }
    }
};





class TimeEventManeger{
public:
    long max_time_id = 0;
    EventNode *timers;  //时间事件队列
    time_t last_run_time;
    TimeEventManeger(){
        timers = new EventNode;
    }
    int CreateTimeEvent(TimeEvent::TimeEventCallBack timeEventCallBack,
                         TimeEvent::TimeEventDestroyCallBack timeEventDestroyCallBack,
                          TimeEvemtType type,
                          std::vector<pvoid> data,
                          long long milliseconds
    ){
        long sec, ms;
        AddMillisecondsToNow(milliseconds, &sec, &ms);
        timers->Add(new TimeEvent(
                max_time_id,
                timeEventCallBack,
                timeEventDestroyCallBack,
                sec,
                ms,
                type,
                data,
                milliseconds
        ));
        return max_time_id++;
    }

    TimeEvent * GetTimerById(int timer_id){
        auto temp = this->timers;
        while(temp->next != nullptr){
            if(temp->next->event->id == timer_id){
                return temp->next->event;
            }
            temp = temp->next;
        }

        return nullptr;
    }

    void UnLinkTimeEvent(int timer_id){
        timers->Del(timer_id);
    }


    TimeEvent * GetNearestEvent(){
        auto te = timers;
        TimeEvent *nearest = nullptr;
        while(te->next != nullptr){
            if(!nearest
               ||
               (te->next->event->when_sec < nearest->when_sec)
               ||
               ((te->next->event->when_sec == nearest->when_sec) && (te->next->event->when_ms < nearest->when_ms))){
                nearest = te->next->event;
            }
            te = te->next;
        }
        return nearest;
    }


    void ProcTimeEvent(){
        auto head = this->timers;
        auto temp = this->timers;
        while(temp->next != nullptr){
            long sec, ms;
            GetTime(&sec, &ms);
            if(temp->next->event->when_sec <= sec || (temp->next->event->when_sec ==sec && temp->next->event->when_ms < ms)){
                temp->next->event->Call();
                if(temp->next == nullptr) return; //如果在回调中自己删除自己，则temp->next为null，需要直接返回
                if(temp->next->event->type == TimeEvemtType::ONCE){
                    head->Del(temp->next->event->id);
                } else if(temp->next->event->type == TimeEvemtType::CERCLE) {
                    long sec1, ms1;
                    AddMillisecondsToNow(temp->next->event->cercleTime, &sec1, &ms1);
                    temp->next->event->when_sec = sec1;
                    temp->next->event->when_ms = ms1;
                }
            }
            temp = temp->next;
            if(temp == nullptr) return;
        }
    }
};

/*
 * 在当前时间上加上 milliseconds 毫秒，
 * 并且将加上之后的秒数和毫秒数分别保存在 sec 和 ms 指针中。
 */
static void AddMillisecondsToNow(long long milliseconds, long *sec, long *ms) {
    long cur_sec, cur_ms, when_sec, when_ms;
    // 获取当前时间
    GetTime(&cur_sec, &cur_ms);

    // 计算增加 milliseconds 之后的秒数和毫秒数
    when_sec = cur_sec + milliseconds/1000;
    when_ms = cur_ms + milliseconds%1000;

    // 进位：
    // 如果 when_ms 大于等于 1000
    // 那么将 when_sec 增大一秒
    if (when_ms >= 1000) {
        when_sec ++;
        when_ms -= 1000;
    }

    // 保存到指针中
    *sec = when_sec;
    *ms = when_ms;
}

static void GetTime(long *seconds, long *milliseconds)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *seconds = tv.tv_sec;
    *milliseconds = tv.tv_usec/1000;
}

#endif //TIME_EVENT_HPP