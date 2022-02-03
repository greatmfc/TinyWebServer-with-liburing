#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request, int state); //追加
    bool append_p(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //是否有任务需要处理
    connection_pool *m_connPool;  //数据库
    int m_actor_model;          //模型切换，1是选择非阻塞同步网络模式，其他是选择异步网络模式
};
template <typename T>
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0) //无效数量
        throw std::exception();
    m_threads = new pthread_t[m_thread_number]; //创建一个线程池，表现形式为数组
    if (!m_threads) //若为空指针
        throw std::exception();
    for (int i = 0; i < thread_number; ++i)
    {
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) //第二个参数指线程属性，将本对象指针即所创建的进程作为worker函数的参数传入，并将本对象指针转换为threadpool指针，通过其调用run()函数
        {
            delete[] m_threads; //如果有任一进程创建失败都会直接删除整个线程池
            throw std::exception();
        }
        if (pthread_detach(m_threads[i])) //创建进程后进行线程分离，以实现结束时系统对其进行自动的资源回收，成功则返回0否则返回错误编号
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}
template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}
template <typename T>
bool threadpool<T>::append(T *request, int state) //向请求队列添加任务并更新该任务状态
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state; //更新状态
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post(); //解除使用该资源
    return true;
}
template <typename T>
bool threadpool<T>::append_p(T *request) //向请求队列添加任务
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post(); //信号量提示有任务要执行
    return true;
}
template <typename T>
void *threadpool<T>::worker(void *arg) //任意类型指针
{
    threadpool *pool = (threadpool *)arg; //将传入指针转换为线程池指针
    pool->run();
    return pool;
}
template <typename T>
void threadpool<T>::run() //因为该函数会由多线程，为保证线程安全和竞争，需使用信号量和锁
{
    while (true)
    {
        m_queuestat.wait(); //开始使用资源，信号量IPC
        m_queuelocker.lock();
        if (m_workqueue.empty()) //该工作队列中没有任务
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front(); //T为http_conn类型，在.h头文件中声明
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request) //如果请求无效
            continue;
        //同步I/O指内核向应用程序通知的是就绪事件，比如只通知有客户端连接，要求用户代码自行执行I/O操作；
        //异步I/O是指内核向应用程序通知的是完成事件，比如读取客户端的数据后才通知应用程序，由内核完成I/O操作。
        if (1 == m_actor_model)
        {
            if (0 == request->m_state) //读为0，写为1，以下部分需结合http_conn部分阅读
            {
                if (request->read_once()) //因为是非阻塞模式需一次性读取，若成功读取完毕
                {
                    request->improv = 1; //让定时器对该连接进行处理
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    request->process(); //调用process()函数对缓冲区中数据进行处理，在该函数中调用两个读写函数对客户进行响应
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1; //1代表超时需进行处理
                }
            }
            else
            {
                if (request->write()) //写回失败或是连接已关闭
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        else //因为是异步模式，所以直接采用RAII模式进行
        {
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
            request->available_to_write = true;
        }
    }
}
#endif
