#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>
using namespace std;

Log::Log()
{
    m_count = 0; //初始化日志行数记录
    m_is_async = false; //设置同步标志位为否
}

Log::~Log()
{
    if (m_fp != NULL)
    {
        fclose(m_fp); //关闭文件描述符
    }
}
//异步需要设置阻塞队列的长度，同步不需要设置
bool Log::init(const char *file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size) //创建日志文件，初始化其文件名，并打开该日志文件设置文件描述符
{
    //如果设置了max_queue_size，则设置为异步
    //同步是指立即写入，而异步是指先将内容写入阻塞队列，而后创建一个写进程从队列中取出内容写入日志
    if (max_queue_size >= 1) 
    {
        m_is_async = true;
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;
        //flush_log_thread为回调函数,这里表示创建线程异步写日志
        pthread_create(&tid, NULL, flush_log_thread, NULL); //新线程从flush函数处开始执行，并将线程ID返回到tid处
    }
    
    m_close_log = close_log;
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size); //在mlog位置之前用\0填充
    m_split_lines = split_lines; //单个日志文件最大行数

    time_t t = time(NULL); //返回1970-01-01 00:00:00至今的秒数
    struct tm *sys_tm = localtime(&t); //返回当前时间的结构指针
    struct tm my_tm = *sys_tm; //储存并填充

 
    const char *p = strrchr(file_name, '/'); //搜索字符最后一次出现的位置，输入名"./ServerLog"后p为"/ServerLog"
    char log_full_name[256] = {0}; //日志文件全面

    if (p == NULL) //未找到，说明只有文件名存在而没有路径名，则无需处理"./"两个字符
    {
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name); //格式化将文件名写入到log_full_name中
    }
    else
    {
        strcpy(log_name, p + 1); //+1是为了去掉'/'符而只保留文件名，仅留下"ServerLog"
        strncpy(dir_name, file_name, p - file_name + 1); //复制去除掉文件名后的路径名到dirname，即将例如"/etc/init"中"/etc/"的部分复制到路径名中
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name); //从前往后写入，逐个对应
    }

    m_today = my_tm.tm_mday; //储存当天
    
    m_fp = fopen(log_full_name, "a"); //创建该文件并以“a”——追加方式写入
    if (m_fp == NULL)
    {
        return false; //打开文件失败则返回false
    }

    return true;
}

void Log::write_log(int level, const char *format, ...) //写入日记文件
{
    struct timeval now = {0, 0}; //该结构体保存秒数及微秒数
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16] = {0};
    switch (level)
    {
    case 0:
        strcpy(s, "[debug]:");
        break;
    case 1:
        strcpy(s, "[info]:");
        break;
    case 2:
        strcpy(s, "[warn]:");
        break;
    case 3:
        strcpy(s, "[erro]:");
        break;
    default:
        strcpy(s, "[info]:");
        break;
    }
    //写入一个log，对m_count++, m_split_lines最大行数
    m_mutex.lock();
    m_count++; //增加一个日记行数记录

    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) //如果日期非今天或者日志行数记录已达到最大行数
    {
        
        char new_log[256] = {0};
        fflush(m_fp); //把原文件待写入的写入
        fclose(m_fp); //关闭日志文件指针避免更多修改
        char tail[16] = {0};
       
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
       
        if (m_today != my_tm.tm_mday)
        {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday; //如果记录日期不是今天则重设并写入
            m_count = 0;
        }
        else //否则说明日志行数记录为0
        {
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        m_fp = fopen(new_log, "a"); //重新以追加模式打开新的文件
    }
 
    m_mutex.unlock();

    va_list valst; //将随后的参数存入到该结构变量中
    va_start(valst, format); //valst指向format之后第一个参数的地址,开始获取变量

    string log_str;
    m_mutex.lock();

    //写入的具体时间内容格式
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    
    int m = vsnprintf(m_buf + n, m_log_buf_size - 1, format, valst); //该函数返回最终写入的字符串长度，将format以后的参数全部写入，包括逗号
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    if (m_is_async && !m_log_queue->full()) //如果开启了同步设置而且阻塞队列非空
    {
        m_log_queue->push(log_str);
    }
    else //否则直接写入
    {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst); //结束变量获取
}

void Log::flush(void)
{
    m_mutex.lock();
    //强制刷新写入流缓冲区
    fflush(m_fp); //刷新该流
    m_mutex.unlock();
}
