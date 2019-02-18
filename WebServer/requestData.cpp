#include "requestData.h"
#include "epoll.h"
#include "util.h"
#include "pthread.h"
#include <sys/epoll.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <iostream>
#include <queue>
#include <vector>

pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER; //时钟队列lock
pthread_mutex_t MimeType::lock = PTHREAD_MUTEX_INITIALIZER; //mimelock
std::unordered_map<std::string, std::string> MimeType::mime;

//单例模式，只有一个对象被创建
std::string MimeType::getMime(const std::string &suffix)
{
	if(mime.size() == 0) // 双重锁定，懒汉式，多线程安全，只有size()=0的线程阻塞在锁调用上
	{
		pthread_mutex_lock(&lock);
		if(mime.size() == 0) //对象没有创建则开始创建
		{
			mime[".html"] = "text/html";
			mime[".avi"] = "video/x-msvideo";
            mime[".bmp"] = "image/bmp";
            mime[".c"] = "text/plain";
            mime[".doc"] = "application/msword";
            mime[".gif"] = "image/gif";
            mime[".gz"] = "application/x-gzip";
            mime[".htm"] = "text/html";
            mime[".ico"] = "application/x-ico";
            mime[".jpg"] = "image/jpeg";
            mime[".png"] = "image/png";
            mime[".txt"] = "text/plain";
            mime[".mp3"] = "audio/mp3";
            mime["default"] = "text/html";
		}
		pthread_mutex_unlock(&lock);
	}
	if(mime.find("suffix") == mime.end())
		return mime["default"];
	return mime["suffix"];
}

requestData::requestData():
	againTimes(0), now_read_pos(0), state(STATE_PARSE_URI), h_state(h_start), isfinish(0), keep_alive(false), timer(NULL)
{
    //printf("RequestData constructed!\n");
}

requestData :: requestData(int _epollfd, int _fd, std::string _path):
	againTimes(0), path(_path), fd(_fd), epollfd(_epollfd), now_read_pos(0), state(STATE_PARSE_URI), 
	h_state(h_start), isfinish(0), keep_alive(0), timer(NULL)
{
	//printf("RequestData constructed!");
}

requestData::~requestData()
{
	//printf("~requestData\n");
	__uint32_t events = EPOLLIN | EPOLLET | EPOLLONESHOT;
	epoll_del(epollfd, fd,(void*)this, events);
	if(timer != NULL){
		timer->clearReq();
		timer = NULL;
	}
    close(fd); //关闭该描述符
}

void requestData::addTimer(mytimer *mtimer)
{
	if(timer == NULL)
		timer = mtimer;
}
	
void requestData::reset()
{
	againTimes = 0;
	path.clear();
	content.clear();
    resource.clear();
	now_read_pos = 0;
	state = STATE_PARSE_URI;
	h_state = h_start;
	isfinish = false;
	keep_alive = false;
	headers.clear();
}

void requestData::seperateTimer()
{
       if(timer){
	       timer->clearReq();
	       timer = NULL;
       }
}

//时间小顶堆
std::priority_queue<mytimer*,std::vector<mytimer*>, timerCmp> timerQueue;


/*读数据-->解析数据-->keepalive-->reset重新加入等待*/
void requestData::handleRequest()
{
	bool isError = 0;
	char buff[MAX_BUFF];
	while(true)
	{
		int readnum = readn(fd, buff, MAX_BUFF);
		
		if(readnum < 0){
			isError = 1;
			break;
		}
		/*由于网络等因素导致没有读到，若重读次数小于最大重读次数，则继续重读*/
		else if(readnum == 0){
			//perror("readnum == 0");
			if(errno == EAGAIN){  //非阻塞的accept
				perror("EAGIN");
				if(againTimes <= AGAIN_MAX_TIMES)
					againTimes += 1;
				else
					isError = 1;
			}
			else if(errno != 0){
				isError = 1;
				perror("read wrong");}
			break;
		}
        
		//printf("%s\n", content.c_str());
        content = std::string(buff, buff+readnum);
        
		//requestline
		if(state == STATE_PARSE_URI){
			int flag = parse_URI();
			//printf("flag-->:%d\n",flag);
			if(flag == PARSE_URI_AGAIN)
				break;
			else if(flag == PARSE_URI_ERR){
				isError = 1;
				break;
			}
		}

		//header
		if(state == STATE_PARSE_HEADERS){
			int flag = parse_Headers();
			if(flag == PARSE_HEADERS_AGAIN)
				break;
			else if(flag == PARSE_HEADERS_ERR){
				isError = 1;
				break;
			}
			if(method == METHOD_POST)
				state = STATE_RECV_BODY;
			else if(method == METHOD_GET)
				state = STATE_ANALYSIS;
			//printf("parse header success\n");
		}

		//body
		if(state == STATE_RECV_BODY){
			if(headers.find("Content-length") == headers.end()){
				isError = 1;
				break;
			}
			int content_length = stoi(headers["content-length"]);
			if(content.size() < (size_t)content_length)
				continue; //没有读完body,回到read部分
			else 
				state = STATE_ANALYSIS;

		}	

		if(state == STATE_ANALYSIS){
			int flag = analysisRequest();
		        if(flag == ANALYSE_SUCCESS){
				    state = STATE_FINISH;
					//printf("state finishd--%d\n", isError);
			    	break;
			    }
			else{
				isError = 1;
				break;
			}
		}
	}

		if(isError){
			delete this;
			return;
		}
        
        if(state == STATE_FINISH){
			//printf("requst Done!!!\n");
			if(keep_alive)
				this->reset(); //避免每次请求应答重新建立连接
			else
			{
				//printf("delete this\n");
				delete this;
				return;
			}
		}
	
        
		//若为keep-alive模式，重置并添加新定时器等待套接字可读
		//并将该事件重新设为EPOLLONESHOT添加进epoll池
		pthread_mutex_lock(&qlock);
        mytimer *newtimer = new mytimer(this, 500);
		timer = newtimer;
        timerQueue.push(timer);
		pthread_mutex_unlock(&qlock);

        __uint32_t _epo_event = EPOLLIN | EPOLLET | EPOLLONESHOT;
        if(epoll_mod(epollfd, fd, (void*)this, _epo_event) < 0){
			delete this;
			return;
		}	
}

int requestData::parse_URI()
{       
	int pos = content.find('\r', 0);
	if(pos < 0)
	    return PARSE_URI_ERR;

   	std::string request_line = content.substr(0, pos);
	content = content.substr(pos+1); //content中去掉request_line,方便解析报头 
	
	//GET or POST
	pos = request_line.find("GET");
	if(pos < 0){
		pos = request_line.find("POST");
		if(pos < 0)
		    return PARSE_URI_ERR;
		else
	        method = METHOD_POST;
	}
	else
		method = METHOD_GET;
	
	// URL
	pos = request_line.find('/');
	int _pos = request_line.find(' ', pos);
	if(pos < 0 || _pos < 0)
		return PARSE_URI_ERR;

	std::string url = request_line.substr(pos + 1, _pos - pos - 1);
	if(url.size() == 0)
		resource = "index.html"; // 如果只有/符号，则默认为主页
	else if((_pos = url.find('?')) < 0)  //查找是否带有参数
	    resource = url;
	else 
		resource = url.substr(0, _pos); //有参数的话取问号前的地址

	//HTTP VERSION
	_pos = request_line.find('/', pos + 1);
	if(_pos < 0)
		return PARSE_URI_ERR;
	else if(request_line.size() -  _pos <= 3) //保证是1.0或1.1
		return PARSE_URI_ERR;
	std::string httpversion = request_line.substr(_pos+1);
	if(httpversion == "1.0")
		HTTPversion = HTTP_10;
    else if (httpversion == "1.1")
		HTTPversion = HTTP_11;
	else
		return PARSE_URI_ERR;
    //成功，状态更新
    state = STATE_PARSE_HEADERS;
    return PARSE_URI_SUCCESS;
}

int requestData::parse_Headers()
{
    //content的不包含request_line,但包含request_line的\r\n
    int pos = 0;
    int key_start = 0; int key_end = 0;
    int value_start =0; int value_end = 0;
    bool notfinish = 1;
   
    for(int i = 0; i < (int)content.size() && notfinish; ++i)
       {
           switch(h_state)
    	   {
	           case h_start:
	           {
		           if(content[i] == '\r' || content[i] == '\n'){
		               break;
				   }
		           else{
		               key_start = i;
					   //printf("-->key_start:%d\n", key_start);
		               h_state = h_key;
		               break;
	               }
	           }
	           case h_key:
	           {  
		           if(content[i] == ':'){
		               key_end = i;
			           //printf("key_end =:%d\n", key_end);		  
		               if(key_end - key_start <= 0)
			               return PARSE_HEADERS_ERR;
		               h_state = h_colon;
		               break;
		           }
		           else if(content[i] == '\n' || content[i] == '\r')
		               return PARSE_HEADERS_ERR;	   
				   break;
	           }
	           case h_colon:
	           {
		           if(content[i] == ' '){
		               h_state = h_spaces_after_colon;
		               break;
		           }
		           else 
		               return PARSE_HEADERS_ERR;
		           break;
	           }
	           case h_spaces_after_colon:
			   {
		           h_state = h_value;
		           value_start = i;
				   //printf("value_start =%d\n", value_start);
		           break;
	           }
	           case h_value:
	           {   
		           if(content[i] == '\r'){
		               value_end = i;
					   h_state = h_CR;
					   //printf("value_end = %d\n", value_end);
		               if(value_end - value_start >= 255)
			               return PARSE_HEADERS_ERR;
		               std::string key = content.substr(key_start, key_end - key_start);
					   //printf("key: %s\n", key.c_str());
		               std::string value = content.substr(value_start, value_end - value_start);
					   //printf("value: %s\n", value.c_str());
		               headers[key] = value;
		               break;
		           }
		           break;
	           }
	           case h_CR:
	           {
		           if(content[i] == '\n'){
					   pos = i;    //没有body的报文没有最后一行空行
		               h_state = h_LF;
		               break;
		           }
		           else
		               return PARSE_HEADERS_ERR;
		           break;
               }
	           case h_LF:
	           {
		           if(content[i] == '\r'){
		               h_state = h_end_CR;
		               break;
		           }
		           else{
		               key_start = i;
		               h_state = h_key;
                       break;
		           }
	            }
                case h_end_CR:
	            {
		           if(content[i] == '\n'){
		               h_state = h_end_LF;
		               break;
		           }
		           else
		               return PARSE_HEADERS_ERR;
		           break;
	            }
	            case h_end_LF:
	            {
		           notfinish = 0;
		           pos = i;
		           break;
	             }
	       }
	   }
	   //printf("pos-->%d\n",pos);
	   content = content.substr(pos); //去掉header和中间的空行
	   if(h_state == h_end_LF)
	       return PARSE_HEADERS_SUCCESS;
	   
	   return PARSE_HEADERS_ERR;
       
}

int requestData::analysisRequest()
{
	if(method == METHOD_GET)
	{
		//写header信息
		char header[MAX_BUFF];
		const char *http_version = (HTTPversion == HTTP_10)? "1.0" : "1.1";
		sprintf(header, "HTTP/%s 200 OK\r\n",http_version);
		if(headers.find("Connection") != headers.end() && headers["Connection"] == "keep-alive")
		{
			keep_alive = true; //解析header,设定keep-alive参数
			sprintf(header, "%sConnection: keep-alive\r\n", header);
			sprintf(header, "%sKeep-alive: timeout=%d\r\n", header, EPOLL_WAIT_TIME);
		}
		int pos = resource.find('.');
		const char *filetype = (pos < 0)? MimeType::getMime("default").c_str()
			: MimeType::getMime(resource.substr(pos)).c_str();
		sprintf(header, "%sContent-Type: %s\r\n", header, filetype);
		//printf("filetype: %s\n", const_cast<char*> (filetype));
     
		//获取资源信息
		struct stat sbuf;
		if(stat(resource.c_str(), &sbuf) < 0)
		{
			handleError(fd, 404, "Not Found!");
			return ANALYSE_ERROR;
		}
		sprintf(header, "%sContent-Length: %zu\r\n", header, sbuf.st_size);
		sprintf(header, "%s\r\n", header);
        //printf("%s\n", header);
		
		//发送header信息
		size_t send_len = writen(fd, header, strlen(header));
		if(send_len != strlen(header)){
			perror("Send header failed");
			return ANALYSE_ERROR;
		}

		// 发送资源文件,将文件印射到内存空间写
		// 映射到进程
		int src_fd = open(resource.c_str(), O_RDONLY, 0);
		char *src_addr = static_cast<char*>(mmap(NULL, sbuf.st_size, PROT_READ, MAP_PRIVATE, src_fd, 0));
        close(src_fd);
		send_len = writen(fd, src_addr, sbuf.st_size);
		if((ssize_t)send_len != sbuf.st_size)
		{
            perror("Send reources failed");
			return ANALYSE_ERROR;
		}
        
		munmap(src_addr, sbuf.st_size);
		//printf("Analyse success%d\n", ANALYSE_SUCCESS);
		return ANALYSE_SUCCESS;
	}
	else if(method == METHOD_POST)
	{
		char header[MAX_BUFF];
		const char* http_version = (HTTPversion == HTTP_10)? "1.0":"1.1";
		sprintf(header, "HTTP%s 200 OK", http_version);
		if(headers.find("Connection") != headers.end() && headers["Connection"] == "keep-alive"){
			keep_alive = true;
			sprintf(header, "%sConnection: keep-alive\r\n", header);
			sprintf(header, "%sKeep-alive: timeout=%d\r\n", header, EPOLL_WAIT_TIME);
		}
		char *send_content = "Receive your post massage";
		sprintf(header, "%sContent-Length%zu", header, strlen(send_content));
		size_t send_len = writen(fd, header, strlen(header));
		if(send_len != strlen(header)){
			perror("Send header failed");
			return ANALYSE_ERROR;
		}

		send_len = writen(fd, send_content, strlen(send_content));
		if(send_len != strlen(send_content)){
			perror("Send header failed");
			return ANALYSE_ERROR;
		}
		return ANALYSE_SUCCESS;

	}

}

void requestData::handleError(int fd, int err_num, const char* message)
{
	char header[MAX_BUFF];
    //const char* http_version = (HTTPversion == HTTP_10)? "1.0":"1.1";
	sprintf(header, "<html><title>ERROR</title>");
	sprintf(header, "%s<body>%d %s",header, err_num, message); 
	sprintf(header,"%s<hr>PJ Server</body></html>", header);
    writen(fd, header, strlen(header));
}

int requestData::getFd()
{
	return fd;
}

void requestData::setFd(int _fd)
{
	fd = _fd;
}

mytimer::mytimer(requestData *_requestdata, int timeout):deleted(false), request_data(_requestdata)
{
	struct timeval now;
	gettimeofday(&now, NULL);
	expired_time = (now.tv_sec * 1000) + (now.tv_usec / 1000) + timeout;

}
mytimer::~mytimer()
{
	if(request_data){
		delete request_data;
		request_data = NULL;
	}
}

void mytimer::update(int timeout)
{
	struct timeval now;
	gettimeofday(&now, NULL);
	expired_time = (now.tv_sec * 1000) + (now.tv_usec / 1000) + timeout;
}

bool mytimer::isvalid()
{
	struct timeval now;
	gettimeofday(&now, NULL);
	size_t tmp = (now.tv_sec * 1000) + (now.tv_usec / 1000);
	if(tmp < expired_time)
		return true;
	else{
		this->setDeleted();
		return false;
	}
}


void mytimer::clearReq()
{
    request_data = NULL;
	this->setDeleted();
}

void mytimer::setDeleted()
{
	deleted = true;
}

bool mytimer::isDeleted() const
{
	return deleted; 
}

size_t mytimer::getExpTime() const
{
	return expired_time;
}

bool timerCmp::operator() (const mytimer *a, const mytimer *b) const
{
	return a->expired_time > b->expired_time;
}
















