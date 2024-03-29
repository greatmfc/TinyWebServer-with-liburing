# TinyWebServer-with-liburing 介绍

## 简介

本项目是基于[TinyWebServer](https://github.com/qinguoyi/TinyWebServer)使用axboe封装的[liburing](https://github.com/axboe/liburing)库对项目中的epoll IO进行了替换和重写，初步实现linux下的异步I/O服务器。

对liburing库的使用参考了该[io_uring-echo-server](https://github.com/frevib/io_uring-echo-server)的模型。

（20220313）更多详细对比见[epoll与io_uring](https://juejin.cn/post/7074212680071905311)

（20220513）该项目IO方面需要进一步改进，先前的性能测试结果有待更正。

（20220625）👴有一个有趣的想法，等有空一定填坑。

## 内容介绍

本项目重写的主要内容在iorws文件夹中，使用新增的iorws类在main.cpp文件中替换了原webserver类的event_Listen和event_Loop两个函数进行主进程的运行。

添加了-d选项，运行时参数为1可开启debug模式，会在终端上输出信息。

（20220308）添加了-u选项，参数为1时可以设置进程可打开的文件描述符上限，解决压力测试后无法继续访问的问题。

```
./server -d 1
```

取消了-m和-a选项，其他选项和原webserver保持一致。

可以使用make test命令开启AddressSanitizer进行检测，输出文件名为test。

（20220308）部分系统上使用sudo命令运行程序可以将io_uring的队列深度设置为32768，正常运行一般为512，可在debug模式下观察到。

```
make test
```

关于新增的iorws类的详细说明见iorws文件夹下的[readme](./iorws/readme.md)。

使用方式：

输入`make`后运行server文件。

（20220306）其他内容与原项目基本一致。

## 使用方式

先将上述的[liburing](https://github.com/axboe/liburing)库clone到本地，然后进入liburing进行make和make install；操作完成后回到本项目下输入make或make test。

## 已知问题

~~使用webbench进行压力测试会有大量的failed，并在测试后无法通过浏览器正常访问，需要结束进程重启，但不影响继续进行压力测试；~~

~~在wsl2上运行会出现无法分配内存的错误，需要使用sudo命令或root用户运行程序，虚拟机系统可正常运行。~~

~~（20220306）使用webbench进行压力测试会有大量的failed，并在测试后直接错误跳出，errno为16。~~

~~（20220307）通过改变io_uring队列深度和系统文件描述符上限可以解决压力测试问题，但是会大量使用系统资源。~~

（20220312）关闭寄存器文件选项可避免大量failed，但性能会大幅下降。

## 性能对比

系统环境：wsl2，内核版本5.10.60.1，发行版为Debian

硬件：I5-9400,16gDDR4

### 原webserver设置双ET+不开启日志

![image-20220202233948107](./root/image-20220202233948107.png)

### 使用io_uring的webserver

![image-20220202232755537](./root/image-20220202232755537.png)

由于优化问题，使用了`io_uring`的webserver在性能上与epoll相比略有优势，可以看得到`io_uring`在网络编程上的可行性和潜力所在。

(20220312)最新测试数据见[benchmark](./benchmark-photo/readme.md)

## 线程版webbench

在test_presure文件夹下的Webbench文件夹中存有使用线程库重写的webbench，粗略估算相较原程序性能下降50%，CPU和内存占用减少50%。需使用g++进行编译。

## TODO

~~加强封装，根据proactor模型优化代码结构。~~

~~（20220306）解决bug。~~
