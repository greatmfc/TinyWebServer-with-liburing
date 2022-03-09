#include "iorws.h"

iorws::iorws(WebServer* ser) : server(ser)
{
	registerfiles = 1;
	memset(&params, 0, sizeof(params));
	//params.flags = IORING_SETUP_SQPOLL;
	if (io_uring_queue_init_params(QUEUE_DEPTH, &ring, &params) < 0) { //初始化队列深度并根据param设置ring的参数
		if (io_uring_queue_init_params(512, &ring, &params) < 0) {
			perror("io_uring_init_failed when queue depth is 512\n");
			exit(1);
		}
		else qd = 512;
	}

	if (!(params.features & IORING_FEAT_FAST_POLL)) {
		//If this flag is set, then io_uring supports using an internal poll mechanism to drive data/space readiness.
		printf("IORING_FEAT_FAST_POLL not available in the kernel, quiting...\n");
		exit(0);
	}

	conns = (conn_info*)calloc(sizeof(conn_info), 65536);
	if (!conns) {
		fprintf(stderr, "allocate conns failed");
		exit(1);
	}

	users = new http_conn*;
	users_timer = new client_data*;
	*users = server->users;
	*users_timer = server->users_timer;
	utils = &server->utils;
}

iorws::~iorws()
{
	io_uring_queue_exit(&ring);
	free(conns);
	free(registered_files);
	close(m_pipefd[1]);
	close(m_pipefd[0]);
	delete[] users;
	delete[] users_timer;
}

void iorws::add_accept(io_uring* ring, int fd, sockaddr* client_addr, socklen_t* client_len, unsigned flags)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	conn_info *conn_i = &conns[fd]; //获取一个描述符的信息
	if (!sqe) {
		return;
	}

	//对sqe填充信息，该函数将提交队列的执行命令设置为accpet命令，相当于调用accpet系统调用
	io_uring_prep_accept(sqe, fd, client_addr, client_len, 0);
	//设置标识，当为0时设置为IOSQE_FIXED_FILE_BIT，When this flag is specified, fd is an index into the files array registered with the io_uring instance
	io_uring_sqe_set_flags(sqe, flags); 
	if (registerfiles)
		//将一组file注册到内核，最终调用io_sqe_files_register，这样内核在注册阶段就批量完成文件的一些基本操作，需检测内核是否支持该功能
		sqe->flags |= IOSQE_FIXED_FILE; 

	conn_i->fd = fd;
	conn_i->type = ACCEPT;
	io_uring_sqe_set_data(sqe, conn_i); //设置提交队列的用户数据
}

void iorws::add_socket_recv(io_uring* ring, int fd, unsigned flags)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	conn_info *conn_i = &conns[fd];
	if ((*users)[fd].m_read_idx >= http_conn::READ_BUFFER_SIZE)
	{
		return ; 
	}
	if (!sqe) {
		return;
	}

	io_uring_prep_recv(sqe, fd, (*users)[fd].m_read_buf+(*users)[fd].m_read_idx, http_conn::READ_BUFFER_SIZE-(*users)[fd].m_read_idx, 0);
	io_uring_sqe_set_flags(sqe, flags);
	if (registerfiles)
		sqe->flags |= IOSQE_FIXED_FILE;

	conn_i->fd = fd;
	conn_i->type = READ;
	io_uring_sqe_set_data(sqe, conn_i);
}

void iorws::add_socket_writev(io_uring* ring, int fd, unsigned flags)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	conn_info *conn_i = &conns[fd];

	if (sqe) {
		io_uring_prep_writev(sqe, fd, (*users)[fd].m_iv, (*users)[fd].m_iv_count, 0);
		io_uring_sqe_set_flags(sqe, flags);
		if (registerfiles)
			sqe->flags |= IOSQE_FIXED_FILE;

		conn_i->fd = fd;
		conn_i->type = WRITE;
		io_uring_sqe_set_data(sqe, conn_i);
	}
}

int iorws::init_registerfiles(void)
{
	struct rlimit r;
	int i, ret;

	ret = getrlimit(RLIMIT_NOFILE, &r); //获取进程能打开的最大文件描述符数和当前的软限制文件描述符数
	if (uf) {
		r.rlim_cur = 31440;
		setrlimit(RLIMIT_NOFILE, &r);
	}
	if (ret < 0) {
		fprintf(stderr, "getrlimit: %s\n", strerror(errno)); //stderror是通过参数errno，返回错误信息
		return ret;
	}
	else if (debug) {
		printf("RLIMIT_NOFILE: %ld %ld\n", r.rlim_cur, r.rlim_max); //输出限制数
	}

	if (r.rlim_max > 32768)
		r.rlim_max = 32768;
	if (debug) {
		printf("number of registered files: %ld\n", r.rlim_max);
		printf("queue depth is: %ld\n", qd);
	}

	registered_files = (int* )calloc(r.rlim_max, sizeof(int));
	if (!registered_files) {
		fprintf(stderr, "calloc failed\n");
		return 1;
	}

	for (i = 0; i < r.rlim_max; ++i)
		registered_files[i] = -1;

	//将文件注册到内核中，避免每次 IO 对文件做 fget/fput 操作
	//该函数先将指定数目的描述符数组注册到ring中，而后再由update函数更新内部的描述符
	ret = io_uring_register_files(&ring, registered_files, r.rlim_max);
	if (ret < 0) {
		fprintf(stderr, "%s: register %d\n", __FUNCTION__, ret);
		return ret;
	}
	return 0;
}

void iorws::deal_with_write(http_conn* user, unsigned int fd, int result)
{
	user[fd].bytes_have_send += result;
	user[fd].bytes_to_send -= result;
	if (user[fd].bytes_have_send >= user[fd].m_iv[0].iov_len) //已发送完一个缓冲区的内容
	{
		user[fd].m_iv[0].iov_len = 0; //重置第一个缓冲区长度
		user[fd].m_iv[1].iov_base = user[fd].m_file_address
			+ (user[fd].bytes_have_send - user[fd].m_write_idx); //设置第二个缓冲区地址
		user[fd].m_iv[1].iov_len = user[fd].bytes_to_send;
	}
	else //否则继续发送
	{
		user[fd].m_iv[0].iov_base = user[fd].m_write_buf + user[fd].bytes_have_send; //重新设置第一个缓冲区地址
		user[fd].m_iv[0].iov_len = user[fd].m_iv[0].iov_len - user[fd].bytes_have_send;
	}
}

void iorws::sig_handler(int signo)
{
	cout << "\rShutting down...\n";
	exit(0);
}

void iorws::IO_eventListen()
{
	debug = server->m_DebugMode;
	int ret;
	if (registerfiles) {
		ret = init_registerfiles(); //初始化文件描述符和寄存器文件描述符，正常返回0
		if (ret)
			return;
	}

	server->m_listenfd = socket(PF_INET, SOCK_STREAM, 0); 
	//对于BSD是AF，对于POSIX是PF；设置IPV4因特网域，设置有序的可靠的面向连接的字节流，设置默认的域，返回套接字描述符
	assert(server->m_listenfd >= 0); //出错时打印错误条件、文件名和出错行数

	//优雅关闭连接
	if (0 == server->m_OPT_LINGER)
	{   //设置linger结构体，可以实现不同的断开方式
		struct linger tmp = {0, 1}; //这种方式下，就是在closesocket的时候立刻返回，底层会将未发送完的数据发送完成后再释放资源，也就是优雅的退出。
		setsockopt(server->m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp)); 
		//设置套接字选项。SOL_SOCKET表示通用的套接字层次；如果还有未发报文而套接字已关闭时的延迟时间；指向的选项结构；指向的对象大小
	}
	else if (1 == server->m_OPT_LINGER)
	{
		struct linger tmp = {1, 1}; //这种方式下，在调用closesocket的时候不会立刻返回，内核会延迟一段时间，这个时间就由l_linger的值来决定。
		setsockopt(server->m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
	}

	struct sockaddr_in address; //ipv4结构
	bzero(&address, sizeof(address)); //将所指地址的前n个字节清零
	address.sin_family = AF_INET; //IPV4
	address.sin_addr.s_addr = htonl(INADDR_ANY); 
	//将主机数转换成无符号长整型的网络字节顺序，INADDR_ANY就是指定地址为0.0.0.0的地址，这个地址事实上表示不确定地址，或“所有地址”、“任意地址”
	address.sin_port = htons(server->m_port); //返回转换后的端口号

	int flag = 1;
	setsockopt(server->m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)); //SO_REUSEADDR表示如果*val非0则重用bind中的地址
	ret = bind(server->m_listenfd, (struct sockaddr *)&address, sizeof(address)); //关联地址和套接字，该函数只接受sockaddr类型的指针
	assert(ret >= 0);
	ret = listen(server->m_listenfd, LISTEN_BACKLOG); //参数backlog提供了一个提示，提示系统该进程所要入队的未完成连接请求数量，若无错误则返回0
	assert(ret >= 0);
	utils->init(TIMESLOT); //初始化计时器的时隙

	//创建一对无命名的、相互连接的UNIX域套接字，可作为全双工管道使用，该管道主要用于信号的接收和传输，全双工是为了高并发
	ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd); 
	assert(ret != -1);

	if (registerfiles) {
		ret = io_uring_register_files_update(&ring, server->m_listenfd,
				&server->m_listenfd, 1); //用新的文件描述符集合替换旧有的，参数分别是，要改变的对象，描述符偏移量，描述符集合，要更改的数量
		if (ret < 0) {
			fprintf(stderr, "lege io_uring_register_files_update "
				"failed: %d %d\n", server->m_listenfd, ret);
			exit(1);
		}
		registered_files[server->m_listenfd] = server->m_listenfd;
	}
}

void iorws::IO_eventLoop()
{
	int m_close_log = server->m_close_log;
	int ret = 0;
	struct sockaddr_in client_addr;
	socklen_t client_len = sizeof(client_addr);

	add_accept(&ring, server->m_listenfd, (struct sockaddr*)&client_addr, &client_len, 0);
	time_t saved_time = time(NULL);
	signal(SIGINT, sig_handler);
	while (1)
	{
		int cqe_count;
		struct io_uring_cqe *cqes[BACKLOG];

		ret = io_uring_submit_and_wait(&ring, 1); //提交sq的entry，阻塞等到其完成，最小完成1个时返回
		if (ret < 0) {
			printf("Returned from io is %d\n", errno);
			perror("Error io_uring_submit_and_wait\n");
			LOG_ERROR("%s", "io_uring failure");
			exit(1);
		}

		//将准备好的队列填充到cqes中，并返回已准备好的数目，收割cqe
		cqe_count = io_uring_peek_batch_cqe(&ring, cqes, sizeof(cqes) / sizeof(cqes[0]));
		assert(cqe_count > 0);
		if (debug) {
			printf("Returned from cqe_count is %d\n", cqe_count);
		}

		for (int i = 0; i < cqe_count; ++i) {
			struct io_uring_cqe* cqe = cqes[i];
			conn_info* user_data = (conn_info*)io_uring_cqe_get_data(cqe);
			int type = user_data->type;

			if (type == ACCEPT) {
				int sock_conn_fd = cqe->res;
				//cqe向前移动避免当前请求避免被二次处理，Must be called after io_uring_{peek,wait}_cqe() after the cqe has been processed by the application.
				io_uring_cqe_seen(&ring, cqe);
				if (debug) {
					printf("Returned from ACCEPT is %d\n", sock_conn_fd);
				}
				if (sock_conn_fd <= 0) {
					continue;
				}

				if (registerfiles && registered_files[sock_conn_fd] == -1) { //寄存文件中并没有注册该连接套接字
					ret = io_uring_register_files_update(&ring, sock_conn_fd, &sock_conn_fd, 1); //重新将该套接字加入到寄存器文件中，减少反复读取
					if (ret < 0) {
						fprintf(stderr, "io_uring_register_files_update "
							"failed: %d %d\n", sock_conn_fd, ret);
						exit(1);
					}
					registered_files[sock_conn_fd] = sock_conn_fd;
				}
				server->timer(sock_conn_fd, client_addr);
				add_socket_recv(&ring, sock_conn_fd, 0); //对该连接套接字添加读取
				add_accept(&ring, server->m_listenfd, (struct sockaddr*)&client_addr, &client_len, 0); 
				//再继续对监听套接字添加监听
			}
			else if (type == READ) {
				int bytes_have_read = cqe->res;
				io_uring_cqe_seen(&ring, cqe);
				if (debug) {
					printf("Returned from READ is %d\n", bytes_have_read);
				}
				util_timer* timer = (*users_timer)[user_data->fd].timer;
				(*users)[user_data->fd].m_read_idx += bytes_have_read;

				if(bytes_have_read > 0) {
					LOG_INFO("deal with the client(%s)", 
						inet_ntoa((*users)[user_data->fd].get_address()->sin_addr));

					//若监测到读事件，将该事件放入请求队列
					server->m_pool->append_p(*users + user_data->fd);

					while (!(*users)[user_data->fd].available_to_write) {}
					(*users)[user_data->fd].available_to_write = false;
					add_socket_writev(&ring, user_data->fd, 0);

					if ((*users_timer)[user_data->fd].timer_exist)
					{
						server->adjust_timer(timer);
					}
				}
				else {
					if (debug) {
						printf("closing by READ: fd%d...\n", user_data->fd);
					}
					if ((*users_timer)[user_data->fd].timer_exist) {
						server->deal_timer(timer, user_data->fd);
					}
					else close(user_data->fd);
					add_accept(&ring, server->m_listenfd, (struct sockaddr*)&client_addr, &client_len, 0);
				}
			}
			else if (type == WRITE) {
				int	ret = cqe->res;
				io_uring_cqe_seen(&ring, cqe);
				if (debug) {
					printf("Returned from WRITE is %d\n", ret);
					printf("Bytes to send is %d\n",(*users)[user_data->fd].bytes_to_send);
					printf("Bytes have send is %d\n",(*users)[user_data->fd].bytes_have_send);
				}
				util_timer* timer = (*users_timer)[user_data->fd].timer;
				if (ret < 0) {
					if(ret == -11) add_socket_writev(&ring, user_data->fd, 0);
					continue;
				}

				if ((*users)[user_data->fd].bytes_to_send <= 0) {
					LOG_INFO("send data to the client(%s)", 
						inet_ntoa((*users)[user_data->fd].get_address()->sin_addr));
					if ((*users)[user_data->fd].m_linger) {
						(*users)[user_data->fd].init();
						if ((*users_timer)[user_data->fd].timer_exist)
						{
							server->adjust_timer(timer);
						}
						add_socket_recv(&ring, user_data->fd, 0);
					}
					else
					{
						if (debug) {
							printf("closing by WRITE: fd%d...\n", user_data->fd);
						}
						if ((*users_timer)[user_data->fd].timer_exist) {
							server->deal_timer(timer, user_data->fd);
						}
						else close(user_data->fd);
					}
					(*users)[user_data->fd].unmap();
					add_accept(&ring, server->m_listenfd, (struct sockaddr*)&client_addr, &client_len, 0);
					//如果关闭后不添加accept会导致重用处在CLOSE_WAIT状态的文件描述符导致无法读写
				}
				else
				{
					deal_with_write(*users, user_data->fd, ret);
					add_socket_writev(&ring, user_data->fd, 0);
				}
			}
		}
		time_t new_time = time(NULL);
		if (new_time - saved_time == utils->m_TIMESLOT) {
			utils->timer_handler(); //处理链表内超时定时器，然后重新设置超时时间
			LOG_INFO("%s", "timer tick");
			saved_time = new_time;
		}
	}
}
