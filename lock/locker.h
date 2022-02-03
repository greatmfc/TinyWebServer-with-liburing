#ifndef LOCKER_H
#define LOCKER_H

#include <exception> //异常头文件
#include <pthread.h>
#include <semaphore.h> //posix信号量头文件

class sem //IPC信号量类，若信号量为正则可以使用该资源
{
public:
    sem()
    {
        if (sem_init(&m_sem, 0, 0) != 0) //创建一个未命名的信号量
        {
            throw std::exception(); //抛出一个异常并终止
        }
    }
    sem(int num)
    {
        if (sem_init(&m_sem, 0, num) != 0) //num为信号量的初始值，第二个参数为是否在多个进程中使用信号量，num为信号量的数量即允许同时使用该资源的进程数量
        {
            throw std::exception();
        }
    }
    ~sem() //析构函数，销毁掉该信号量
    {
        sem_destroy(&m_sem);
    }
    bool wait() //判断是否成功将信号量减1，说明已开始使用
    {
        return sem_wait(&m_sem) == 0;
    }
    bool post() //判断是否成功将信号量增1，说明已使用完成
    {
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem; //信号量变量
};
class locker //锁类
{
public:
    locker()
    {
        if (pthread_mutex_init(&m_mutex, NULL) != 0) //用默认属性初始化线程互斥量
        {
            throw std::exception();
        }
    }
    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);
    }
    bool lock() //判断是否成功对该锁上锁
    {
        return pthread_mutex_lock(&m_mutex) == 0;
    }
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }
    pthread_mutex_t *get() //获取一个锁
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};
class cond //条件变量类
{
public:
    cond()
    {
        if (pthread_cond_init(&m_cond, NULL) != 0) //默认初始化条件变量（动态分配，需手动销毁）
        {
            //pthread_mutex_destroy(&m_mutex);
            throw std::exception();
        }
    }
    ~cond()
    {
        pthread_cond_destroy(&m_cond);
    }
    bool wait(pthread_mutex_t *m_mutex)
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_wait(&m_cond, m_mutex); //使用输入互斥量对该条件变量进行保护,阻塞等待条件变量
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0; //判断是否满足了条件，若false则说明出现了错误
    }
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t)
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t); //在限时t内等待条件变量
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool signal()
    {
        return pthread_cond_signal(&m_cond) == 0; //唤醒至少一个阻塞在条件变量上的线程
    }
    bool broadcast()
    {
        return pthread_cond_broadcast(&m_cond) == 0; //唤醒全部阻塞在条件变量上的线程
    }

private:
    //static pthread_mutex_t m_mutex;
    pthread_cond_t m_cond;
};
#endif
