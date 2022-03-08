#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock; //获取一个锁
map<string, string> users;

void http_conn::initmysql_result(connection_pool *connPool) //从传入的连接池中获取连接，并从mysql数据库中获取用户密码数据将其存入map中
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool); //初始化一个RAII类型的连接对象，该对象拥有指向连接池的指针和当前连接的指针，结束时通过连接池指针释放该连接

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user")) //通过该连接检索是否有该用户密码对应，mysql_query() 执行指定为一个空结尾的字符串的SQL查询
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql)); //日记写入宏，mysql_error() 返回最近被调用的MySQL函数的出错消息
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql); //mysql_store_result() 检索一个完整的结果集合给客户

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL); //改变已打开文件的属性，F_GETFL表示返回当前文件描述符标志
    int new_option = old_option | O_NONBLOCK; //文件状态标志为非阻塞模式
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    /*
    epoll_event event;
    event.data.fd = fd; //将被监听的文件描述符添加到红黑树中

    if (1 == TRIGMode) //若设置为触发ET模式
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP; //描述符处于可读状态；将epoll event通知模式设置成edge triggered（即ET模式）；对端描述符产生一个挂断事件
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot) //若设置了只通知一次
        event.events |= EPOLLONESHOT; //第一次进行通知，之后不再监测
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event); //向interest list添加一个需要监视的描述符
	//epollfd是epoll_create()的返回值，即一个epoll实例
    */
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    //epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0); //从兴趣列表删除该描述符
    close(fd);
}

//将事件重置为EPOLLONESHOT，在一个线程处理完当前事件后调用
//如果不注册EPOLLONESHOT事件会导致在多线程环境下一个线程在处理套接字上数据时，如果此时套接字有新数据传来则会再次触发使得另一个线程也进行处理，
//导致不同的线程在处理同一个套接字的数据
//如果对描述符socket注册了EPOLLONESHOT事件，那么操作系统最多触发其上注册的一个可读、可写或者异常事件，且只触发一次。
//想要下次再触发则必须使用epoll_ctl重置该描述符上注册的事件，包括EPOLLONESHOT 事件。
//为防止多线程竞争而使用
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP; //ET模式；只通知一次；产生挂断事件
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP; //ev是输入事件

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event); //重新注册该事件
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1)) //检验确保有被使用
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd); //用-1来移除一个fd的事件
        m_sockfd = -1; //已被关闭可再设置
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    //addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str()); //c_str()返回当前字符串的首字符地址
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;
    available_to_write = false;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx]; //逐个读取
        if (temp == '\r') //如果是回车
        {
            if ((m_checked_idx + 1) == m_read_idx) //如果已读取到最后一个
                return LINE_OPEN; //行被正常读取，还需要继续读取
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0'; //将'\n'替换为结束符
                m_read_buf[m_checked_idx++] = '\0'; //将最后一个字符替换
                return LINE_OK; //行被处理完毕
            }
            return LINE_BAD; //说明'\r'之后还有非特殊字符，该行有误，返回读取失败
        }
        else if (temp == '\n') //读取到了换行符
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') //如果当前是'\r\n'的组合
            {
                m_read_buf[m_checked_idx - 1] = '\0'; //替换回车符
                m_read_buf[m_checked_idx++] = '\0'; //添加结束符
                return LINE_OK; //说明行读取完毕
            }
            return LINE_BAD; //说明该行可能只存在一个换行符
        }
    }
    return LINE_OPEN; //一行被正常读取，没有遇到换行符等情况，没有遇到换行等情况，需要继续接收进行读取
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once() //将到来的数据读入读缓冲区中
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false; //越下标了
    }
    int bytes_read = 0; //已读字节

    //LT读取数据（阻塞模式）
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0); //文件描述符；存放读取数据的缓冲区指针；要读取的最大字节数
        m_read_idx += bytes_read; //recv在没有数据可用时会一直阻塞等待

        if (bytes_read <= 0) //等于0表示连接关闭，小于0表示出错
        {
            return false;
        }

        return true;
    }
    //ET读数据（非阻塞模式）
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK) //因为recv无数据可读而又处在非阻塞状况下，所以会返回EAGAIN或等同的EWOULDBLOCK
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t"); //检索是否找到第一个空格或制表符，找到则返回指向空格或制表符的指针
    if (!m_url) //如果没找到
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0'; //先让指针指向空格后的'/'，然后将原空格替换为'\0'以取出前面的数据
    char *method = text;
    if (strcasecmp(method, "GET") == 0) //忽略大小写的比较
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1; //采用cgi处理post请求
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t"); //返回第一个不匹配的下标，其当前返回值为0，即移动到了url的'/'处，为了去除多余的空格和制表符
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0'; //同上
    m_version += strspn(m_version, " \t"); //寻找是否有空格或者制表符，移动到'H'处
    if (strcasecmp(m_version, "HTTP/1.1") != 0) //不支持其他版本的HTTP协议
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0) //比较两者的前7个字符是否相等
    {
        m_url += 7;
        m_url = strchr(m_url, '/'); //返回第一次出现'/'的位置
    }

    if (strncasecmp(m_url, "https://", 8) == 0) //同上
    {
        m_url += 8; //跳过多余的http头
        m_url = strchr(m_url, '/'); //避免可能的空格
    }

    if (!m_url || m_url[0] != '/') //如果出错或格式有误
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER; //将主状态机状态设置为检查请求头
    return NO_REQUEST; 
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text) //该函数一行一行地进行数据输入
{
    if (text[0] == '\0') //该行无内容
    {
        if (m_content_length != 0) //如果是POST请求
        {
            m_check_state = CHECK_STATE_CONTENT; //将主状态机设置为检查POST消息体
            return NO_REQUEST; //表示请求不完整需要继续接收
        }
        return GET_REQUEST; //否则请求接受完毕
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true; //持久连接
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text); //转换并写入POST消息体长度
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx)) //判断是否已读取完POST消息体
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read() //将主状态机和从状态机进行封装
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK)) //循环执行处理POST消息体
    { //（当主状态机为POST消息体读取状态而且行已被完整读取）或者行已被完整读取
        text = get_line(); //获取可以开始的读缓冲区地址
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        switch (m_check_state) //判断当前工作状态
        {
        case CHECK_STATE_REQUESTLINE: //解析请求行
        {
            ret = parse_request_line(text); //此处处理完毕会跳转至解析报文头
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER: //解析报文头
        {
            ret = parse_headers(text); //此处处理完毕会跳转至解析报文内容
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST) //否则继续读取
            {
                return do_request(); //可以跳出循环
            }
            break;
        }
        case CHECK_STATE_CONTENT: //解析POST消息体部分
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN; //已读取完毕后更改状态避免继续循环
            break;
        }
        default:
            return INTERNAL_ERROR; //服务器内部逻辑，一般不触发
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request() //该函数将网站根目录和url文件拼接，然后通过stat判断该文件属性
{
    strcpy(m_real_file, doc_root); //将实际文件地址改为文档根目录
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/'); //搜索最后一次出现'/'的位置

    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2); //把标志符之后的追加写入到real中
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1); //把指定长度的m_url_real内容追加复制到m_real_file+len之后
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html"); //成功插入则跳转至登录界面
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html"); //有重名
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    if (*(p + 1) == '0') //POST请求，跳转到注册界面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1') //POST请求，跳转到登录界面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5') //POST请求，跳转到图片请求界面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6') //POST请求，跳转到视频界面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1); //如果均不符合，则直接将URL与网站目录拼接

    if (stat(m_real_file, &m_file_stat) < 0) //如果没能成功获取目标文件的信息
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH)) //如果不允许其他人读取
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode)) //如果是一个目录
        return BAD_REQUEST;

    int fd = open(m_real_file, O_RDONLY); //以只读方式打开
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0); //将该文件映射到内存，映射区可读，对该映射区进行的操作都将创建一个副本
    close(fd);
    return FILE_REQUEST;
}
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size); //取消size参数指定的从address所开始的映射内存地址数量
        m_file_address = 0;
    }
}
bool http_conn::write() //服务器子线程调用process_write完成响应报文，随后注册epollout事件。服务器主线程检测写事件，并调用http_conn::write函数将响应报文发送给浏览器端。
{
    int temp = 0;

    if (bytes_to_send == 0) //要发送的数据长度为0，表示响应报文为空
    {
        //modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode); //EPOLLIN指连接到达，有数据来临
        init();
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count); //聚集写，返回已读或已写的字节数，m_iv数组中的元素由m_iv_count指定，将缓冲区内数据输出到套接字描述符中

        if (temp < 0) //如果读，出错失败
        {
            if (errno == EAGAIN) //在非阻塞模式下调用了阻塞操作
            {
                //modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode); //EPOLLOUT指有数据要写
                return true;
            }
            unmap(); //在do_request函数中进行了内存映射
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len) //已发送完一个缓冲区的内容
        {
            m_iv[0].iov_len = 0; //重置第一个缓冲区长度
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx); //设置第二个缓冲区地址
            m_iv[1].iov_len = bytes_to_send;
        }
        else //否则继续发送
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send; //重新设置第一个缓冲区地址
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0) //数据已全部发送完
        {
            unmap();
            //modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode); //重置epoll事件树

            if (m_linger) //连接的状态类型
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
bool http_conn::add_response(const char *format, ...) //调用该函数来按格式写入内容
{
    if (m_write_idx >= WRITE_BUFFER_SIZE) //写入内容超出大小
        return false;
    va_list arg_list; //可变参数列表
    va_start(arg_list, format); //开始读取参数，将列表初始化为传入参数
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list); //返回最终写入字符串的长度
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) //如果写入内容超出空间
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len; //指针移位
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf); //写入日志

    return true;
}
bool http_conn::add_status_line(int status, const char *title) //添加状态行
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type() //添加文本类型
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_linger() //添加连接状态
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content) //添加文本内容
{
    return add_response("%s", content);
}
bool http_conn::process_write(HTTP_CODE ret) //根据do_request的返回状态，服务器子线程调用process_write向m_write_buf中写入响应报文
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0) //如果该文件长度不为0
        {
            add_headers(m_file_stat.st_size); //添加首部行
            m_iv[0].iov_base = m_write_buf; //设置响应报文的缓冲区
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address; //指向映射的文件地址
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else //如果请求的资源大小为0
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string)); //返回空白HTML文件
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
	//除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
void http_conn::process() //各子线程通过process函数对任务进行处理，调用process_read函数和process_write函数分别完成报文解析与报文响应两个任务
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        //modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode); //注册并监听读事件
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret) //如果没能成功写入报文
    {
        close_conn();
    }
    //modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode); //注册并监听写事件
}
