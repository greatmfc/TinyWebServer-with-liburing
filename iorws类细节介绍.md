# `iorws`类细节介绍

## `io_uring`工作原理简介

原生的`io_uring`只提供了三个用户态的接口，`io_uring_setup`, `io_uring_enter` 和 `io_uring_register`。前者是设置`io_uring`结构的基本信息，初始化一个新的`io_uring`上下文；中者是负责将`io_uring`上下文里的sqe提交到内核中进行处理并对已完成任务进行收割，后者是注册内核与用户共享的缓冲区。在实际运行的过程中，需要将先前初始化的`io_uring`中的complete queue(cq)、submission queue(sq)和submission queue entries(sqes)三个部分映射到内存中，内核和用户将在内存中共享同一块映射区。用户负责从其初始化的`io_uring`实例中获取一个空闲的SQE并设置任务信息等，然后把该SQEs的索引放到sq队列中，待任务完成后对cq队列进行收割，而内核则从SQEs数组获取任务并将完成的任务提交到cq队列中。

## `liburing`库函数简介

类中所使用到的库函数有`io_uring_queue_init_params`、`io_uring_get_sqe`、`io_uring_prep_`、`io_uring_sqe_set_data`、`io_uring_submit_and_wait`、`io_uring_peek_batch_cqe`等几个。

### `io_uring_queue_init_params`

该函数将对`io_uring`实例的初始化以及内存区域的映射一并进行了封装，避免了用户方额外手动对内存进行映射。

### `io_uring_get_sqe`

该函数从已初始化的`io_uring`实例中获取一个内容为空的sqe指针。

### `io_uring_prep_`

该系列函数根据不同的功能对sqe进行任务设置，支持不同系统调用的多种参数。

### `io_uring_sqe_set_data`

将设置好的`user_data`信息设置到对应的sqe中。

### `io_uring_submit_and_wait`

该函数底层使用`io_uring_enter`系统调用，将sq队列中的任务提交给内核，并阻塞等待完成最小指定个数任务时返回。

### `io_uring_peek_batch_cqe`

该函数收割cq中指定数目的完成任务，填充信息到`io_uring_cqe`结构的指针中，并返回完成个数。

## 类声明

```c++
class iorws
{
public:
	iorws(WebServer* ser);
	~iorws();
	void add_accept(struct io_uring *ring, int fd,struct sockaddr *client_addr, socklen_t *client_len,unsigned flags); 
	void add_socket_recv(struct io_uring* ring, int fd, unsigned flags);
	void add_socket_writev(struct io_uring* ring, int fd, unsigned flags);
	void add_pipe_read(struct io_uring* ring, int fd, char* signals, unsigned flags);
	void IO_eventListen();
	void IO_eventLoop();
	int init_registerfiles(void);

public:
	WebServer* server;
	http_conn** users;
	client_data** users_timer;
	Utils* utils;
	int m_pipefd[2]; //未实际使用

private:
	struct io_uring_params params;
	struct io_uring ring; //与内核进行提交与收割交互的对象
	int registerfiles; //是否开启寄存文件功能
	int *registered_files; //寄存文件数组

	enum {
		ACCEPT,
		POLL_LISTEN,
		POLL_NEW_CONNECTION,
		READ,
		WRITE,
		PIPE, //未使用
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

```

在公开成员变量中，由于类对象需要使用到其他类对象的成员，而直接使用其他类对象在成员函数编写的过程中多有不便，因此在类中定义了多个所需对象的指针，方便直接在类中直接进行操作。

枚举的若干类型将会在`conn_info`结构中的`type`中使用到，而`conn_info`是进行`io_uring`操作所需要的参数，这会在后文进行说明。

## 类实现

### 构造函数

在构造函数中，首先对`ring`对象进行队列初始化和参数设置，而后检查内核是否支持`IORING_FEAT_FAST_POLL`标志和初始化寄存器文件数组、分配`conn_info`数组空间等操作，该标志表示内核将支持使用对数据或者空间进行轮询读取的内部机制。

```c++
	users = new http_conn*;
	users_timer = new client_data*;
	*users = server->users;
	*users_timer = server->users_timer;
	utils = &server->utils;

```

在对`users`以及`users_timer`成员进行赋值时，务必注意需要先在堆上分配空间，否则会导致段错误。

### 析构函数

在销毁对象时，需要调用`io_uring_queue_exit`函数来释放`io_uring`实例所使用的资源。

### `void iorws::add_accept`函数

```c++
void iorws::add_accept(io_uring* ring, int fd, sockaddr* client_addr, socklen_t* client_len, unsigned flags)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	conn_info *conn_i = &conns[fd]; //获取一个描述符的信息
	if (!sqe) {
		return;
	}

	//对sqe填充信息，该函数将提交队列的执行命令设置为accpet命令，相当于调用accpet系统调用
	io_uring_prep_accept(sqe, fd, client_addr, client_len, 0);
	//设置标识，当为0时设置为IOSQE_FIXED_FILE_BIT
    //When this flag is specified, fd is an index into the files array registered with the io_uring instance
	io_uring_sqe_set_flags(sqe, flags); 
	if (registerfiles)
		//将一组file注册到内核，最终调用io_sqe_files_register，这样内核在注册阶段就批量完成文件的一些基本操作，需检测内核是否支持该功能
		sqe->flags |= IOSQE_FIXED_FILE; 

	conn_i->fd = fd;
	conn_i->type = ACCEPT;
	io_uring_sqe_set_data(sqe, conn_i); //设置提交队列的用户数据
}

```

该函数中以`io_uring_`开头的函数都是`liburing`库函数。`get_sqe`函数是从`ring`对象中获取一个sqe以将任务提交到队列中。在`liburing`有多个封装好的`io_uring_prep_`函数，通过这些接口来将特定的任务提交到任务队列中。而后将所设置的文件描述符及其类型添加到sqe中，在收割时将根据这些类型和文件描述符进行具体的操作。

### `int iorws::init_registerfiles(void)`函数

该函数中有两段被注释掉的语句：

```c++
	r.rlim_cur = 31440;
	setrlimit(RLIMIT_NOFILE, &r);
```

这两行代码可以设置当前进程打开的文件描述符软上限数，可以解决压力测试后无法使用的问题，但会导致系统的文件描述符被大量占用，需要进程结束后才会被释放。

### `void iorws::IO_eventListen()`函数

该函数与webserver类中的该函数逻辑基本一致，都是网络编程和内部功能设置的基本步骤，只有最后的注册寄存文件一步不同。

### `void iorws::IO_eventLoop()`函数

在该函数中，由于使用的是异步IO，获取的是已完成事件的状态，因此与webserver类中使用的epoll模型获取的是待处理事件的模式不同。首先添加一个对监听套接字的accept任务，然后进入事件循环，不断提交sq中的事件并调用`io_uring_peek_batch_cqe`函数对已完成的事件集合cq进行收割；如果cq中的数量为0，则提交一个accpet任务，避免因为提交时队列内没有任务导致系统调用中断。

```c++
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
if (!cqe_count) {
	add_accept(&ring, server->m_listenfd, (struct sockaddr*)&client_addr, &client_len, 0);
}
```

接下来是一个对cqes数组中的cqe进行遍历处理的过程，函数返回的结果存放在`io_uring_cqe`结构中的`res`字段中。在获取了cqe中的user_data信息后（即在`add_`系列函数中设置的`fd`与`type`字段），根据其`type`字段的内容执行下一步的动作。

```c++
struct io_uring_cqe* cqe = cqes[i];
//获取一个cqe中的user_data数据
conn_info* user_data = (conn_info*)io_uring_cqe_get_data(cqe);
int type = user_data->type;
```

#### ACCEPT类型

如果完成的事件是accpet，则首先调用`io_uring_cqe_seen`函数将cq队列向前移动避免对请求进行二次处理；然后将返回的连接套接字寄存器文件数组中，避免对其反复读取，并添加一个对其的计时器和对该套接字的读取任务到sqe队列中。如果返回值小于等于0则表示出错，则判断当前cqes数组中还有多少个ACCEPT类型的完成任务，如果完成任务较少则添加一个accept任务，否则直接跳过多余的出错完成任务。

```c++
if (type == ACCEPT) {
	int sock_conn_fd = cqe->res;
	//cqe向前移动避免当前请求避免被二次处理
	io_uring_cqe_seen(&ring, cqe);
	if (server->m_DebugMode) {
		printf("Returned from ACCEPT is %d\n", sock_conn_fd);
	}
    if (sock_conn_fd <= 0) {
        if (cqe_count - i <= 1) {
            goto newone;
        }
        else continue;
    }

    if (registerfiles && registered_files[sock_conn_fd] == -1) { //寄存文件中并没有注册该连接套接字
        //重新将该套接字加入到寄存器文件中，减少反复读取
        ret = io_uring_register_files_update(&ring, sock_conn_fd, &sock_conn_fd, 1); 
        if (ret < 0) {
            fprintf(stderr, "io_uring_register_files_update "
                "failed: %d %d\n", sock_conn_fd, ret);
            exit(1);
        }
        registered_files[sock_conn_fd] = sock_conn_fd;
    }
    server->timer(sock_conn_fd, client_addr);
    add_socket_recv(&ring, sock_conn_fd, 0); //对该连接套接字添加读取
    //再继续对监听套接字添加监听
newone:add_accept(&ring, server->m_listenfd, (struct sockaddr*)&client_addr, &client_len, 0); 
}
```

#### READ类型

如果完成的事件是read，则将已读取的字节数量与该连接读取下标相加。如果返回的结果小于等于0则关闭连接并添加对监听套接字的accpet任务；否则将该连接放入事件请求队列，并通过循环阻塞等待事件的完成，然后根据是否有可发送的数据注册写回任务或读取任务。

#### WRITE类型

根据返回值的不同，当返回值为-32时，表示该套接字无法写回，则重新添加accept任务并删除该套接字的计时器；当返回值为-11时，表示需要重试写入，则添加写回任务；当返回值为-14时，表示套接字地址有误，则添加读取任务重新对该套接字进行读取。如果成员变量`bytes_to_send`小于等于0表示已全部将该连接的数据写回完毕，则根据该连接是否为持久连接或已关闭进行相应的操作。如果成员变量`bytes_to_send`大于0，则重新调整该连接的聚集写缓冲区地址和相应变量数据，然后再次注册写回任务。

