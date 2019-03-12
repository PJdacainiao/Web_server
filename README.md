# Web_server
基于epoll和线程池实现的高性能webserver,使用webbench进行web性能压力测试
## 特点
* 使用epoll边沿触发的I/O复用，非阻塞I/O，reactor模式
* 使用threadpool避免多线程工作时的线程创建和销毁
* 使用小根堆实现定时器超时删除
* 使用状态机解析报文header

reference https://github.com/linyacool/WebServer
