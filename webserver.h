#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"

const int MAX_FD = 65536;           //最大文件描述符
const int MAX_EVENT_NUMBER = 10000; //最大事件数
const int TIMESLOT = 5;             //最小超时单位

class WebServer
{
public:
    WebServer();
    ~WebServer();

    void init(int port , string user, string passWord, string databaseName,
              int log_write , int opt_linger, int trigmode, int sql_num,
              int thread_num, int close_log, int actor_model);

    void thread_pool();
    void sql_pool();
    void log_write();
    void trig_mode();
    void eventListen();
    void eventLoop();
    void timer(int connfd, struct sockaddr_in client_address); //定时器函数，初始化定时器并将其加入到链表中
    void adjust_timer(util_timer *timer);
    void deal_timer(util_timer *timer, int sockfd); //处理计时器，将其删除
    bool dealclinetdata(); //用于在监听套接字有响应后建立连接
    bool dealwithsignal(bool& timeout, bool& stop_server); //由管道事件调用，从管道的读端读取信号
    void dealwithread(int sockfd); //将到来的读事件加入到请求队列中并调整该套接字的计时器
    void dealwithwrite(int sockfd);

public:
    //基础
    int m_port; //端口
    char *m_root; //root文件夹路径
    int m_log_write; //1是使用异步
    int m_close_log; //0是开启日志
    int m_actormodel; //模型切换，1是选择非阻塞同步网络模式即reactor，默认0是选择异步网络模式即proactor

    int m_pipefd[2];
    int m_epollfd;
    http_conn *users;

    //数据库相关
    connection_pool *m_connPool;
    string m_user;         //登陆数据库用户名
    string m_passWord;     //登陆数据库密码
    string m_databaseName; //使用数据库名
    int m_sql_num;

    //线程池相关
    threadpool<http_conn> *m_pool;
    int m_thread_num;

    //epoll_event相关
    epoll_event events[MAX_EVENT_NUMBER];

    int m_listenfd;
    int m_OPT_LINGER; //0是优雅地关闭连接，1是等待一段时间后关闭
    int m_DebugMode; //设置监听套接字和建立连接是LT还是ET模式
    int m_LISTENTrigmode; //1是设置监听套接字的工作方式为ET模式
    int m_CONNTrigmode; //1是设置已建立的连接的工作方式为ET模式

    //定时器相关
    client_data *users_timer;
    Utils utils;
};
#endif
