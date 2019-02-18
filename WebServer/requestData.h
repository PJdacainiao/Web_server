 #ifndef REQUESTDATA
 #define REQUESTDATA
 #include <string.h>
 #include <unordered_map>
 //state
 const int STATE_PARSE_URI = 1;
 const int STATE_PARSE_HEADERS = 2;
 const int STATE_RECV_BODY = 3;
 const int STATE_ANALYSIS = 4;
 const int STATE_FINISH = 5;
 //
 const int PARSE_URI_AGAIN = -1;
 const int PARSE_URI_ERR = -2;
 const int PARSE_URI_SUCCESS = 2;
 const int PARSE_HEADERS_AGAIN = -1;
 const int PARSE_HEADERS_ERR = -2;
 const int PARSE_HEADERS_SUCCESS = 2;

 const int ANALYSE_SUCCESS = 1;
 const int ANALYSE_ERROR = -1;
 

 const int MAX_BUFF = 4096;
 //重传
 const int AGAIN_MAX_TIMES = 200;

 const int METHOD_POST = 1;
 const int METHOD_GET = 2;
 const int HTTP_10 = 1;
 const int HTTP_11 = 2;

 const int EPOLL_WAIT_TIME = 500;

 /*资源类型，content-type，采用单例模式避免过多对象的构造和析构*/
 class MimeType
 {
 private:
     static pthread_mutex_t lock;
     static std::unordered_map<std::string, std::string> mime;
     MimeType(); //单例模式，构造函数私有
     MimeType(const MimeType &m);
 public:
     static std::string getMime(const std::string &suffix);//获取实例的接口
 };
/*状态机解析header*/
 enum HeadersState
 {
     h_start = 0, //每一行header的开头
     h_key,       //每一行的key
     h_colon,     //key后的冒号
     h_spaces_after_colon,  //冒号与value间的空格
     h_value,     //value
     h_CR,        //<\r>
     h_LF,        //<\n>
     h_end_CR,    //header和body之间的<\r>
     h_end_LF     //header和body之间的<\n>
 };

struct mytimer;
struct requestData
{
private:
	int againTimes;
	std::string path; //url位置前面的路径
        int fd;
	int epollfd;
        std::string content; //读到的内容
	int method; //post or get
	int HTTPversion; 
	std::string resource; //URL资源名称
	int now_read_pos; //记录解析的位置
	int state; 
	int h_state; //header的不同状态 HeadersState
	bool isfinish;
	bool keep_alive;
	std::unordered_map<std::string, std::string> headers; //存放header内容
	mytimer *timer;

private:
	int parse_URI();
	int parse_Headers();
	int analysisRequest();

public:
	requestData();
	requestData(int _epollfd, int _fd, std::string _path);
	~requestData();
	void addTimer(mytimer *mtimer);
	int getFd();
	void setFd(int _fd);
	void reset();
	void seperateTimer();
	void handleRequest();
	void handleError(int fd, int state_num, const char* message);

};

struct mytimer
{
	bool deleted;
	size_t expired_time;
	requestData *request_data;
	
	mytimer(requestData *_requestdata, int timeout);
	~mytimer();
	void update(int timeout);
	bool isvalid();
	void clearReq();
	void setDeleted();
	bool isDeleted() const;
	size_t getExpTime() const;

};

struct timerCmp
{
	bool operator() (const mytimer *a, const mytimer *b) const;
};

#endif

