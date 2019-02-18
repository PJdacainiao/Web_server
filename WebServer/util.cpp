#include "util.h"
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <iostream>

ssize_t readn(int fd, void *buff, size_t n)
{
	size_t nleft = n;
	ssize_t nread = 0;
	ssize_t readsum = 0;
	char *ptr = (char*) buff;
	while(nleft > 0){
		if((nread = read(fd, ptr, nleft)) < 0){
			if(errno == EINTR) //发生中断时再次读取
				nread = 0;
			else if(errno == EAGAIN) //非阻塞资源没准备好时，直接返回
				return readsum;
			else
				return -1;
		}
		else if(nread == 0){ //EOF
			break;
		}
	
		readsum += nread;
		nleft -= nread;
		ptr += nread;
	}
		return readsum;
}

ssize_t writen(int fd, void *buff, size_t n)
{
	size_t nleft = n;
	ssize_t nwritten = 0;
	ssize_t writesum = 0;
	char *ptr = (char*) buff;
	while(nleft > 0){
		if((nwritten = write(fd, ptr, n)) <= 0){
		    if(nwritten < 0)
			{
				if(errno == EINTR || errno == EAGAIN)
				{
					nwritten = 0;
					continue;
				}
		    	else
			        return -1;
			}
		}
		nleft -= nwritten;
		writesum += nwritten;
		ptr += nwritten;
	}
	return writesum;
}

/*忽略SIGPIPE信号，往一个已经关闭的socket写信号会产生SIGPIPE信号，收到该信号，发送套接字也会关闭*/
void  handle_for_sigpipe()
{
	struct sigaction sa;
	memset(&sa, '\0', sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;
	if(sigaction(SIGPIPE, &sa, NULL))
		return;
        //return -1;
    //std::cout<<"ignore sigpipe success";
	//return;
}

int setSocketNonBlocking(int fd)
{
	int flag = fcntl(fd, F_GETFL, 0);
	if(flag == -1)
		return -1;

	flag |= O_NONBLOCK;
	if(fcntl(fd, F_SETFL, flag) == -1)
		return -1;
	//printf("nonblocking set\n");
	return 0;
}




















