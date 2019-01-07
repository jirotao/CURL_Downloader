#pragma once

#ifdef CURLDOWNLOADER_EXPORTS
#define CURLDOWN_EXPORT __declspec(dllexport)
#else
#define CURLDOWN_EXPORT __declspec(dllimport)
#endif

//采用curl/pthreads库实现多线程下载

typedef void CURL;
typedef void CURLSH;
struct _tThreadInfo;
typedef struct _tThreadInfo ThreadInfo;
struct pthread_mutex_t_;
typedef struct pthread_mutex_t_ * pthread_mutex_t;

#include <string>
#include <vector>

#pragma warning(disable:4251)
//下载状态
enum CurlDownState{DOWN_PROGRESS, DOWN_PAUSE, DOWN_TERMINATE};

//日志函数
typedef void (*LPLOGFUN)(const std::string& strLog);
//下载设置
typedef struct _tCurlDownConfig{
	int iMaxThreadCount; //单个任务在支持多线程下载情况下最多开的线程数目
	int iTimeOut;        //下载过程中的超时时间(s)
	int iMaxTryTimes;    //下载线程失败最大重试次数
	int iMinBlockSize;   //多线程下载时，最小分块大小(B)
	LPLOGFUN pLogFun;
	std::string szSSLCrtName; //SSL证书名字
	std::string szSSLKeyName; //SSL证书密钥
	_tCurlDownConfig()
	{
		iMaxThreadCount = 5;
		iTimeOut = 60;
		iMaxTryTimes = 5;
		iMinBlockSize = 5*1024*1024;
		pLogFun = NULL;
		szSSLKeyName = "";
		szSSLCrtName = "";
	}
}CurlDownConfig;

class CURLDOWN_EXPORT CurlDownloader
{
public:
	CurlDownloader(void);
	~CurlDownloader(void);

	//全局初始化
	static void Init();
	static void Uninit();
	//下载设置
	static void SetCurlDownConfig(const CurlDownConfig& downCfg);
	//开始下载
	bool Start(const std::string& strUrl, const std::string& strDownloadPath, 
		int pThreadCount = 0);
	//暂停
	bool Pause();
	//恢复下载，支持断点续传
	bool Resume();
	//终止
	bool Terminate(bool bDeleteFile = true);
	//阻塞等待下载完
	bool WaitForFinish();
	//获取下载进度
	void GetProgress(unsigned long* pTotalFileSize, unsigned long* pDownSize);
	//是否是多线程下载
	bool IsMutliDownload();
	//
	bool IsRedirected();
	//检测是否完成下载
	bool CheckIsFinish(bool* pbThrdRunFinish);
	//获取当前状态
	CurlDownState GetCurlDownState();
	//设置代理
	void SetProxy(const std::string& strProxy);
	//获取错误码
	int GetLastHttpCode();
	//获取CURL错误码
	int GetLastCurlCode();
	//获取下载初始化是否成功
	bool GetDownloadSuccess();
	std::string GetRealUrl();
	int GetThreadCount();
	bool IsDownloadInitOver();
	bool IsDownloadInitInterrupt();
	void SetPause(bool bPause);
protected:
	//下载初始化
	bool DownloadInit();
	//curl初始化
	bool CurlInit(void* param);
	//下载函数
	static void* DownloadFun(void* param);
	// 头信息 
	static size_t HeaderInfo(char *ptr, size_t size, size_t nmemb, void *userdata);
	//写数据
	static size_t WriteData(char *ptr, size_t size, size_t nmemb, void *userdata);
	//解析线程参数
	static void ParseThreadParam(void* param, CurlDownloader** ppDown, int* pIndex);
	//通过临时文件进行断点续传
	bool ContinueDownloadByTmpFile();
	//最后处理
	bool FinalProc();
	//更新临时文件中指定线程块下载信息
	void UpdateDownloadInfoInTmpFile(int* pIndex);
	//清理线程信息
	void ClearThreadInfo();
	//检测SSL
	static bool CheckSSL(CURL* pCurl, const std::string& strUrl);
	//检测文件长度
	bool CheckFileLength();
	//检测是否支持分片传输
	bool CheckIsSupportRange();
	bool CheckIsSupportRangeEx();
	// 创建curl对象
	CURL *Create_Share_Curl();
	//
	void SleepEx(unsigned long ulMilliseconds);
public:
	//文件
	FILE* m_pFile;
	//用于控制写文件的互斥变量
	pthread_mutex_t m_mutexFile;
	//用于保存每个线程的信息的数组
	std::vector<ThreadInfo*> m_vecThrdInfo;
	//下载地址及本地保存路径
	std::string m_strUrl="", m_strDownloadPath="";
	//代理
	std::string m_strProxy;
	//是否暂停
	bool m_bPause;
	//是否中止下载
	bool m_bTerminate;
	//是否支持多线程下载
	bool m_bSupportMultiDown;
	//要下载的文件大小
	unsigned long m_ulFullFileSize;
	//已经下载的文件大小
	unsigned long m_ulDownFileSize;
	//HTTP错误码
	int m_iHttpCode;
	int m_curlCode;
	//是否重定向过，重定向之前的URL
	bool m_bRedirected;
	std::string m_strOriginalUrl;
	// 共享dns处理对象
	static CURLSH* sharedns_handle;
	static CurlDownConfig g_curlDownCfg;
	bool m_bInitOver;
	bool m_bInitInterrupt;
	bool init_success_;
	bool bad_net_;
};

