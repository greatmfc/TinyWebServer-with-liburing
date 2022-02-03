#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
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
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

class http_conn
{
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;
    enum METHOD //方法类
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    enum CHECK_STATE //主状态机的状态
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT //仅用于解析POST请求
    };
    enum HTTP_CODE //报文解析的结果
    {
        NO_REQUEST, //请求不完整需要继续接收
        GET_REQUEST, //获取了完整的请求
        BAD_REQUEST, //请求有误
        NO_RESOURCE, //请求资源不存在
        FORBIDDEN_REQUEST, //禁止访问
        FILE_REQUEST, //请求资源可以正常访问
        INTERNAL_ERROR, //服务器内部错误
        CLOSED_CONNECTION //连接已关闭
    };
    enum LINE_STATUS //行状态
    {
        LINE_OK = 0, //当前行已读取完毕
        LINE_BAD, //行读取出错
        LINE_OPEN //行还需要继续读取
    };

public: //public类可以让外部创建一个类对象，private类只能让类内部函数构造该类对象
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname); //外部调用以初始化套接字地址
    void close_conn(bool real_close = true);
    void process();
    bool read_once(); //循环读取客户数据，直到无数据可读或对方关闭连接；非阻塞ET工作模式下，需要一次性将数据读完
    bool write(); //服务器子线程调用process_write完成响应报文，随后注册epollout事件。服务器主线程检测写事件，并调用http_conn::write函数将响应报文发送给浏览器端。
    sockaddr_in *get_address() //ipv4套接字结构地址
    {
        return &m_address;
    }
    void initmysql_result(connection_pool *connPool);
    int timer_flag; //0代表未超时
    int improv; //1代表有新事件并已处理完毕，需要定时器对该连接进行调整
    void unmap(); //取消映射
    void init(); //对象内部调用，以初始化新接受的连接，即该对象？


private:
    HTTP_CODE process_read(); 
    bool process_write(HTTP_CODE ret);
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    HTTP_CODE do_request();
    char *get_line() { return m_read_buf + m_start_line; };
    LINE_STATUS parse_line();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd; //IO多路复用文件描述符
    static int m_user_count; //用户计数
    MYSQL *mysql;
    int m_state;  //当前连接的状态，待读为0, 待写为1
    struct iovec m_iv[2]; //缓冲区结构，包含一个缓冲区和将要写入的数据长度
    int m_iv_count; //iovec结构数组数量
    char m_read_buf[READ_BUFFER_SIZE];
    int m_read_idx;
    bool m_linger; //该连接的连接状态是keep-alive还是closed
    int bytes_to_send;
    int bytes_have_send;
    char *m_file_address;
    int m_write_idx;
    char m_write_buf[WRITE_BUFFER_SIZE];
    bool available_to_write;

private:
    struct stat m_file_stat; //文件信息结构体
    int m_sockfd; //套接字文件描述符
    sockaddr_in m_address;
    int m_checked_idx;
    int m_start_line;
    CHECK_STATE m_check_state; //主状态机处理状态
    METHOD m_method;
    char m_real_file[FILENAME_LEN];
    char *m_url;
    char *m_version;
    char *m_host;
    int m_content_length; //0是GET请求，1是POST
    int cgi;        //是否启用的POST
    char *m_string; //存储请求头数据
    char *doc_root; //文件根目录地址

    map<string, string> m_users;
    int m_TRIGMode; //触发模式
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
