#include "iorws/iorws.h"

WebServer::WebServer()
{
    //http_conn类对象
    users = new http_conn[MAX_FD];

    //root文件夹路径
    char server_path[200]; //服务器路径
    getcwd(server_path, 200); //将当前目录复制到指定路径中
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    //定时器
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer()
{
    //close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, 
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_DebugMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

void WebServer::trig_mode()
{
    //LT + LT
    if (0 == m_DebugMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    //LT + ET
    else if (1 == m_DebugMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    //ET + LT
    else if (2 == m_DebugMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    //ET + ET
    else if (3 == m_DebugMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void WebServer::log_write() //创建当日的日志文件
{
    if (0 == m_close_log) //如果开启了日志
    {
        //初始化日志
        if (1 == m_log_write) //异步写入日志
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800); //懒汉模式
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

void WebServer::sql_pool()
{
    //初始化数据库连接池
    m_connPool = connection_pool::GetInstance(); //饿汉模式，该函数获取一个指向静态connection_pool对象的指针，即获取一个未初始化的连接池对象
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log); //调用该函数以初始化可用的mysql连接池，并获取一个相关的信号量，设置最大连接数

    //初始化数据库读取表
    users->initmysql_result(m_connPool); //在构造时已初始化该指针，通过该指针调用
}

void WebServer::thread_pool()
{
    //线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

void WebServer::eventListen()
{
    //网络编程基础步骤
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0); //对于BSD是AF，对于POSIX是PF；设置IPV4因特网域，设置有序的可靠的面向连接的字节流，设置默认的域，返回套接字描述符
    assert(m_listenfd >= 0); //出错时打印错误条件、文件名和出错行数

    //优雅关闭连接
    if (0 == m_OPT_LINGER)
    {   //设置linger结构体，可以实现不同的断开方式
        struct linger tmp = {0, 1}; //这种方式下，就是在closesocket的时候立刻返回，底层会将未发送完的数据发送完成后再释放资源，也就是优雅的退出。
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp)); //设置套接字选项。SOL_SOCKET表示通用的套接字层次；如果还有未发报文而套接字已关闭时的延迟时间；指向的选项结构；指向的对象大小
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1}; //这种方式下，在调用closesocket的时候不会立刻返回，内核会延迟一段时间，这个时间就由l_linger的值来决定。
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0; //返回值
    struct sockaddr_in address; //ipv4结构
    bzero(&address, sizeof(address)); //将所指地址的前n个字节清零
    address.sin_family = AF_INET; //IPV4
    address.sin_addr.s_addr = htonl(INADDR_ANY); //将主机数转换成无符号长整型的网络字节顺序，INADDR_ANY就是指定地址为0.0.0.0的地址，这个地址事实上表示不确定地址，或“所有地址”、“任意地址”
    address.sin_port = htons(m_port); //返回转换后的端口号

    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)); //SO_REUSEADDR表示如果*val非0则重用bind中的地址
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address)); //关联地址和套接字，该函数只接受sockaddr类型的指针
    assert(ret >= 0);
    ret = listen(m_listenfd, 5); //参数backlog提供了一个提示，提示系统该进程所要入队的未完成连接请求数量，若无错误则返回0
    assert(ret >= 0);

    utils.init(TIMESLOT); //初始化计时器的时隙

    //epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5); //size用来告诉内核这个监听的数目一共有多大，返回一个epoll的句柄
    assert(m_epollfd != -1);

    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode); //把监听的文件描述符添加到epoll事件树中
    http_conn::m_epollfd = m_epollfd; //把服务器获取的连接符赋值给连接的多路复用文件描述符，工具类的addfd()函数主要给WebServer类使用，而http_conn类中的单独使用（也有可能是重复编写）

    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd); //创建一对无命名的、相互连接的UNIX域套接字，可作为全双工管道使用，该管道主要用于信号的接收和传输，全双工是为了高并发
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]); //写端无阻塞
    utils.addfd(m_epollfd, m_pipefd[0], false, 0); //把管道的读端也添加到监听事件树中，等待来自其他进程的数据

    utils.addsig(SIGPIPE, SIG_IGN); //在管道的读进程已终止后，一个进程写此管道则产生SIGPIPE信号；SIG_IGN指忽略此信号，SIG_IGN是一个处理函数
    utils.addsig(SIGALRM, utils.sig_handler, false); //把信号传递到管道的写端
    utils.addsig(SIGTERM, utils.sig_handler, false);

    alarm(TIMESLOT); //设置超时

    //工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd; //让定时器也能够通过该管道传输信号给进程
    Utils::u_epollfd = m_epollfd; //共用一个事件符，让定时器能够从该事件树中删除超时的连接
}

void WebServer::timer(int connfd, struct sockaddr_in client_address) //定时器函数，初始化定时器并将其加入到链表中
{
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName); //初始化连接，[connfd]指指定连接符的对象进行初始化

    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func; //指明调用的函数
    time_t cur = time(NULL); //设置当前时间
    timer->expire = cur + 3 * TIMESLOT; //设置超时时间
    users_timer[connfd].timer = timer; //设置该用户的定时器
    users_timer[connfd].timer_exist = true;
    utils.m_timer_lst.add_timer(timer); //把该定时器添加到链表中
}

//若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

void WebServer::deal_timer(util_timer *timer, int sockfd) //处理计时器，将其删除
{
    timer->cb_func(&users_timer[sockfd]);
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

bool WebServer::dealclinetdata() //处理客户端数据
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    if (0 == m_LISTENTrigmode) //如果是LT模式
    {
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength); //接受连接
        if (connfd < 0) //接受连接失败
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD) //连接数超过上限
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address);
    }

    else //ET模式必须循环将数据全部读取完毕，因为后续不会再有通知
    {
        while (1) //不断接受请求直到没有请求或者请求已满
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno); //总是会触发该项以跳出循环
                break;
            }
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0); //从读端接收信号
    if (ret == -1) //出错
    {
        return false;
    }
    else if (ret == 0) //没有可用数据或发送方已按序结束
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            case SIGALRM:
            {
                timeout = true; //已超时
                break;
            }
            case SIGTERM:
            {
                stop_server = true; //终止
                break;
            }
            }
        }
    }
    return true;
}

void WebServer::dealwithread(int sockfd) //处理读事件
{
    util_timer *timer = users_timer[sockfd].timer;

    //reactor
    if (1 == m_actormodel)
    {
        if (timer) //如果该计时器有效
        {
            adjust_timer(timer);
        }

        //若监测到读事件，将该事件放入请求队列
        m_pool->append(users + sockfd, 0);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].read_once()) //如果成功读取完毕
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else //如果未成功读取
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::dealwithwrite(int sockfd) //处理写事件
{
    util_timer *timer = users_timer[sockfd].timer;
    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::eventLoop()
{
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server)
    {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        //阻塞等待事件的发生返回事件的数目，并将触发的事件写入events。-1表示无限等待，直到有文件描述符进入ready状态或者捕获到信号才返回
        if (number < 0 && errno != EINTR) //EINTR指系统调用被中断
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++) //逐个处理事件
        {
            int sockfd = events[i].data.fd;

            //处理新到的客户连接
            if (sockfd == m_listenfd) //如果事件来自监听端
            {
                bool flag = dealclinetdata();
                if (false == flag) //当前客户端接收失败，ET模式下总是为false
                    continue;
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) //如果是这类事件之一
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            //处理信号
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN)) //如果信号来自管道读端而且事件是有信号到达
            {
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                dealwithread(sockfd);
            }
            else if (events[i].events & EPOLLOUT) //如果是有数据要写
            {
                dealwithwrite(sockfd);
            }
        }
        if (timeout) //可能由于没有触发事件，所以会一直判断该条件
        {
            utils.timer_handler(); //处理链表内超时定时器，然后重新设置超时时间

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}