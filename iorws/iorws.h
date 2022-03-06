#ifndef IORWS_H
#define IORWS_H

#include "../webserver.h"
#include <liburing.h>
#include <sys/resource.h>
#include <cstddef>

#define BACKLOG 8192
#define IORING_FEAT_FAST_POLL (1U << 5)
constexpr auto WAIT_TIME = 0xffffff;
constexpr auto QUEUE_DEPTH = 512;

class iorws
{
public:
	iorws(WebServer* ser);
	~iorws();
	void add_accept(struct io_uring *ring, int fd,struct sockaddr *client_addr, socklen_t *client_len,unsigned flags); 
	void add_socket_recv(struct io_uring* ring, int fd, unsigned flags);
	void add_socket_writev(struct io_uring* ring, int fd, unsigned flags);
	void IO_eventListen();
	void IO_eventLoop();
	int init_registerfiles(void);
	void deal_with_write(http_conn* user, unsigned int fd, int result);

public:
	WebServer* server;
	http_conn** users;
	client_data** users_timer;
	Utils* utils;
	int m_pipefd[2];

private:
	struct io_uring_params params;
	struct io_uring ring;
	int registerfiles; //是否开启寄存文件功能
	int *registered_files; //寄存文件数组

	enum {
		ACCEPT,
		POLL_LISTEN,
		POLL_NEW_CONNECTION,
		READ,
		WRITE,
		PIPE,
	};
	typedef struct conn_info //存放文件描述符和文件类型
	{
		unsigned fd;
		unsigned type;
	} conn_info;

	conn_info* conns;
	int max_connections = 65536;
	int msg_len = 128;

};

#endif // !IORWS_H
