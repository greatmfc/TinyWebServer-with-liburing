#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}
sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

void sort_timer_lst::add_timer(util_timer *timer)
{
    if (!timer) //如果输入计时器为空
    {
        return;
    }
    if (!head) //如果是空链表
    {
        head = tail = timer;
        return;
    }
    if (timer->expire < head->expire) //如果新的定时器超时时间小于当前头部节点，就直接将当前头部节点设置为新的计时器
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    add_timer(timer, head); //私有成员函数调整内部节点
}
void sort_timer_lst::adjust_timer(util_timer *timer) //当任务发生变化时调整定时器在链表中的位置
{
    if (!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;
    if (!tmp || (timer->expire < tmp->expire)) //被调整的定时器位于链表尾部或者定时器小于下一个定时器超时值则不调整
    {
        return;
    }
    if (timer == head) //如果给定定时器为头定时器
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head); //将该节点调整到原头节点的下一节点后的合适位置
    }
    else //否则被调整定时器在链表内部
    {
        timer->prev->next = timer->next; //将定时器取出进行重新调整
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}
void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if ((timer == head) && (timer == tail)) //链表内只有一个元素
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if (timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}
void sort_timer_lst::tick() //每次系统收到信号则调用该函数以处理链表内的超时定时器
{
    if (!head)
    {
        return;
    }
    
    time_t cur = time(NULL); //得到当前日历时间
    util_timer *tmp = head;
    while (tmp)
    {
        if (cur < tmp->expire) //如果没超时
        {
            break;
        }
        tmp->cb_func(tmp->user_data); //当前定时器到期，则调用回调函数，执行定时事件，删除该描述符
        head = tmp->next; //然后搜素下一个定时器
        if (head) //如果定时器非空
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}

void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head) //作用是在给定节点后插入定时器，其为内部私有函数，只能由其他函数调用，
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    while (tmp)
    {
        if (timer->expire < tmp->expire) //如果找到了添加的定时器超时时间小于指定节点之后位置的时间，即可以插入
        {
            prev->next = timer; //将指定节点的下一节点更改为要添加的定时器即timer
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next; //向后遍历
    }
    if (!tmp) //如果没能找到合适的插入位置，即tmp为空
    {
        prev->next = timer; //则把要添加的定时器添加到链表的末尾
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

void Utils::init(int timeslot) //初始化时隙
{
    m_TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //setnonblocking(fd);
}

//信号处理函数，作用是把信号传输到管道的写端
void Utils::sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0); //把信号传送到管道的写端
    errno = save_errno;
}

//设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    struct sigaction sa; //参阅APUE中10.14节
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler; //设置信号处理程序，让信号集中的信号被触发时调用该函数
    if (restart)
        sa.sa_flags |= SA_RESTART; //由此信号中断的系统调用自动重启动
    sigfillset(&sa.sa_mask); //设置默认信号屏蔽字
    //sigaction函数的功能是检查或修改（或检查并修改）与指定信号相关联的处理动作。此函数取代了UNIX早期版本使用的signal函数。
    assert(sigaction(sig, &sa, NULL) != -1); //检查或修改与指定信号相关联的处理动作，把给定sig添加到sa中
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    m_timer_lst.tick();
    //alarm(m_TIMESLOT); //设置时隙为超时时间
}

void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;
void cb_func(client_data *user_data)
{
    //epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0); //从列表中删除该描述符
    assert(user_data); //检验该连接是否有效，无效则终止程序执行
    user_data->timer_exist = false;
    close(user_data->sockfd);
    http_conn::m_user_count--;
}
