#include "config.h"

int main(int argc, char *argv[])
{
    //需要修改的数据库信息,登录名,密码,库名
    string user = "root";
    string passwd = "667788";
    string databasename = "yourdb";

    //命令行解析
    Config config;
    config.parse_arg(argc, argv); //设置类函数，执行完毕则结束

    static WebServer server;
    iorws iorws(&server);

    //初始化
    server.init(config.PORT, user, passwd, databasename, config.LOGWrite, 
                config.OPT_LINGER, config.debugmode,  config.sql_num,  config.thread_num, 
                config.close_log, config.actor_model); //设置类函数，执行完毕则结束
    

    //日志
    server.log_write(); //设置类函数，执行完毕则结束，创建了当天的日志文件，并初始化日志缓冲区等

    //数据库
    server.sql_pool(); //设置类函数，执行完毕则结束，主要目的是创建与数据库进行连接的连接池，并通过连接取出数据库内数据

    //线程池
    server.thread_pool(); //循环执行函数，该函数在new完一个对象后，该对象内的构造函数会创建一个线程池并分离线程让子线程都循环执行从消息队列获取信息并处理的任务，该循环不会结束

    //触发模式
    server.trig_mode(); //设置类函数，执行完毕则结束

    //监听
    //server.eventListen(); //设置类函数，执行完毕则结束，主要目的是设置监听套接字并创建epoll事件表进行复用
    iorws.IO_eventListen();

    //运行
    //server.eventLoop(); //循环执行函数，该函数通过epoll树返回的文件描述符进行相应的处理，如果监听到了自管道写端的信号会进行处理
    iorws.IO_eventLoop();

    return 0;
}