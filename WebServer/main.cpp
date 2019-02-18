#include "requestData.h"
#include "util.h"
#include "threadpool.h"
#include "epoll.h"
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>
#include <queue>
#include <iostream>
#include <fcntl.h>
#include <string.h>
#include <vector>

const int THREAD_COUNT = 4;
const int QUEUE_SIZE = 65535;
const int TIMER_TIMEOUT = 500;
//const int MAX_EVENTS = 1000;
const std::string PATH = "/";

const int PORT = 8888;

extern epoll_event *events;
extern pthread_mutex_t qlock;
extern std::priority_queue<mytimer*, std::vector<mytimer*>, timerCmp> timerQueue;


int socket_bind_listen(int port)
{
	if(port < 1024 || port > 65535)
		return -1;
    
	int listenfd = socket(AF_INET, SOCK_STREAM, 0);
	if(listenfd == -1)
		return -1;

	//调试时设置端口是可复用的,运行时关闭
	//防止kill服务器进程后主动关闭处于time_wait状态，导致再次启动服务器bind到该port出错
	int on = 1;
    if(setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1)
		 return -1;

    // 绑定地址和端口
	struct sockaddr_in serv_addr;
	bzero(&serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(port);
	bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));

    // 监听
	if(listen(listenfd, LISTENQ) == -1)
		return -1;
    //printf("listenfd constructed\n");
	return listenfd;
}

int acceptConnection(int listenfd, int epoll_fd, const std::string& path)
{
	struct sockaddr_in cliaddr;
	socklen_t clilen = sizeof(cliaddr);
	int connfd = 0;

	//由于是ET触发，当大量连接到达时，accept仅能处理一个连接
	//因此需要while循环accept，直到已就绪连接队列为空
	while((connfd = accept(listenfd, (struct sockaddr*)&cliaddr, &clilen)) > 0)
	{   
        
  		if(setSocketNonBlocking(connfd) < 0)
			return -1;
		/*int flags;
		 *flags = fcntl(connfd, F_GETFL, 0);
		 *flags &= O_NONBLOCK;
		 *std::cout<< flags <<std::endl;*/ 
		
     	requestData *request = new requestData(epoll_fd, connfd, path);
        //printf("connfd= %d\n", request->getFd());

		//设置EPOLLONESHOT
		//即使是ET方式，也有可能因线程调用（过慢），直到下次边沿触发时，线程1仍在处理
		//此时，第二次ET时，线程2也同时处理线程1上的fd,导致多个线程同时处理一个fd
		//因此，线程结束后要将事件重新设置并添加到epoll中
		__uint32_t event = EPOLLIN | EPOLLET | EPOLLONESHOT;
		epoll_add(epoll_fd, connfd, (void*)request, event);
        
		mytimer *timer = new mytimer(request, TIMER_TIMEOUT);
		request->addTimer(timer);
		pthread_mutex_lock(&qlock);
		timerQueue.push(timer);
		pthread_mutex_unlock(&qlock);
				
	}
	if(connfd == -1)
		return -1;
}




void myHandle(void *arg)
{
	requestData *req = (requestData*)arg;
	req->handleRequest();
}

void handle_events(int epoll_fd, int event_num, int listenfd, epoll_event *events, threadpool_t *tp, const std::string& path)
{
	//遍历epoll_wait返回的就绪描述符数目
	for(int i = 0; i < event_num; ++i)
	{
		requestData *request = (requestData*)(events[i].data.ptr);
		int fd = request->getFd();
        //printf("fd = %d\n", fd); 

		//判断就绪事件是不是监听就绪
		//listenfd上有监听事件发生，则调用accept
		//若就绪套接字为connfd,则说明有requestData发送过来，加入线程池处理
		if(fd == listenfd){
			acceptConnection(listenfd, epoll_fd, path);
		}
		else
		{   
			if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) || (!(events[i].events & EPOLLIN)))
			{
				printf("event error!!!\n");
				delete request;
				continue;
			}
			//requestData数据已经可读，不再定时
			request->seperateTimer();
			threadpool_add(tp, myHandle, events[i].data.ptr, 0);
		}
    }
}

/*超时事件的处理：小顶堆时间队列堆顶是最接近超时的事件
 * 事件监听返回并处理完成后，定时器设置为deleted,等到该定时器到达timerQueue堆顶时再删除，可以保证
 * 1.无需遍历去删除已经完成的时间（事件）
 * 2.超时的timer不是立即删除的，在超时与真正要被删除之间，若在某一fd上有事件就绪，则不用重新创建requestData*/
void handle_expire_events()
{
	pthread_mutex_lock(&qlock);
	while(!timerQueue.empty())
	{
		mytimer *top_timer = timerQueue.top();
		if(top_timer->isDeleted()){
			timerQueue.pop();
			delete top_timer;
		}
		else if(!(top_timer->isvalid())){
			timerQueue.pop();
			delete top_timer;
		}
		else
			break;
	}
	pthread_mutex_unlock(&qlock);
}

int main()
{
	//忽略sigpipe信号，这是因为若客户端已经关闭，服务器向客户端第二次发送信息，收到sigpipe信号，sigpipe默认是teminate
    handle_for_sigpipe();
    int epoll_fd = epoll_init();
	if(epoll_fd < 0){
		perror("Epoll init error");
		return -1;
	}
    
	//线程池中的线程数目以及任务队列大小
	threadpool_t *threadpool = threadpool_create(THREAD_COUNT, QUEUE_SIZE, 0);
	if(threadpool == NULL){
		perror("Threadpool init error");
		return -1;
	}
   
	int listenfd = socket_bind_listen(PORT);
	if(listenfd < 0)
	{
		perror("socket listen and bind error");
		return -1;
	}
    //使用多路复用+accept时，常常设置listenfd为非阻塞，防止出现以下状况：
	//epoll返回连接已建立，在accept调用之前，client发送RST（服务器繁忙，accept比较慢）
	//但由于RST使得已完成的连接被清除，epoll返回1，server阻塞在accept调用上，直到有新连接到来，任务队列中其他已就绪描述符号不到处理
	if(setSocketNonBlocking(listenfd) < 0)
	{
		perror("set socket nonblocking error");
		return -1;
	}
	
	requestData* request = new requestData();
	request->setFd(listenfd);

	//listenfd不能设置为EPOLLONESHOT，否则的话一段时间内仅维持一份连接
	__uint32_t event = EPOLLIN | EPOLLET;
	epoll_add(epoll_fd, listenfd, (void*) request, event);

	while(true)
	{
        int event_num = my_epoll_wait(epoll_fd, events, MAXEVENTS, -1);
		printf("event_num= %d\n", event_num); 
        
		handle_events(epoll_fd, event_num, listenfd, events, threadpool, PATH);
		handle_expire_events();
	}
	return 0;
}

