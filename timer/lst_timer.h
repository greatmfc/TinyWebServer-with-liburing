#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "../log/log.h"

class util_timer; //定时器类

struct client_data //客户端数据
{
    sockaddr_in address; //客户端套接字地址
    int sockfd; //套接字文件描述符
    util_timer *timer; //定时器
    bool timer_exist;
};

class util_timer //定时器类
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    time_t expire; //超时时间
    
    void (* cb_func)(client_data *); //回调函数，删除输入指针的对应事件和关闭对应文件描述符
    client_data *user_data;
    util_timer *prev; //前向定时器
    util_timer *next; //后向定时器
};

class sort_timer_lst //定时器容器类，将多个定时器串联起来
{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    void add_timer(util_timer *timer);
    void adjust_timer(util_timer *timer);
    void del_timer(util_timer *timer);
    void tick(); //使用统一事件源，SIGALRM信号每次被触发，主循环中调用一次定时任务处理函数，处理链表容器中到期的定时器

private:
    void add_timer(util_timer *timer, util_timer *lst_head);

    util_timer *head;
    util_timer *tail;
};

class Utils //该类负责设置将文件描述符或信号添加到集中，以及其他工具类函数
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    //对文件描述符设置非阻塞
    int setnonblocking(int fd);

    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    //信号处理函数，作用是把信号传输到管道的写端
    static void sig_handler(int sig);

    //设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;
    sort_timer_lst m_timer_lst;
    static int u_epollfd;
    int m_TIMESLOT; //时隙
};

void cb_func(client_data *user_data); //删除输入指针的对应事件和关闭对应文件描述符

#endif
