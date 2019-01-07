#pragma once

#ifdef CURLDOWNLOADER_EXPORTS
#define CURLDOWN_EXPORT __declspec(dllexport)
#else
#define CURLDOWN_EXPORT __declspec(dllimport)
#endif

//����curl/pthreads��ʵ�ֶ��߳�����

typedef void CURL;
typedef void CURLSH;
struct _tThreadInfo;
typedef struct _tThreadInfo ThreadInfo;
struct pthread_mutex_t_;
typedef struct pthread_mutex_t_ * pthread_mutex_t;

#include <string>
#include <vector>

#pragma warning(disable:4251)
//����״̬
enum CurlDownState{DOWN_PROGRESS, DOWN_PAUSE, DOWN_TERMINATE};

//��־����
typedef void (*LPLOGFUN)(const std::string& strLog);
//��������
typedef struct _tCurlDownConfig{
	int iMaxThreadCount; //����������֧�ֶ��߳������������࿪���߳���Ŀ
	int iTimeOut;        //���ع����еĳ�ʱʱ��(s)
	int iMaxTryTimes;    //�����߳�ʧ��������Դ���
	int iMinBlockSize;   //���߳�����ʱ����С�ֿ��С(B)
	LPLOGFUN pLogFun;
	std::string szSSLCrtName; //SSL֤������
	std::string szSSLKeyName; //SSL֤����Կ
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

	//ȫ�ֳ�ʼ��
	static void Init();
	static void Uninit();
	//��������
	static void SetCurlDownConfig(const CurlDownConfig& downCfg);
	//��ʼ����
	bool Start(const std::string& strUrl, const std::string& strDownloadPath, 
		int pThreadCount = 0);
	//��ͣ
	bool Pause();
	//�ָ����أ�֧�ֶϵ�����
	bool Resume();
	//��ֹ
	bool Terminate(bool bDeleteFile = true);
	//�����ȴ�������
	bool WaitForFinish();
	//��ȡ���ؽ���
	void GetProgress(unsigned long* pTotalFileSize, unsigned long* pDownSize);
	//�Ƿ��Ƕ��߳�����
	bool IsMutliDownload();
	//
	bool IsRedirected();
	//����Ƿ��������
	bool CheckIsFinish(bool* pbThrdRunFinish);
	//��ȡ��ǰ״̬
	CurlDownState GetCurlDownState();
	//���ô���
	void SetProxy(const std::string& strProxy);
	//��ȡ������
	int GetLastHttpCode();
	//��ȡCURL������
	int GetLastCurlCode();
	//��ȡ���س�ʼ���Ƿ�ɹ�
	bool GetDownloadSuccess();
	std::string GetRealUrl();
	int GetThreadCount();
	bool IsDownloadInitOver();
	bool IsDownloadInitInterrupt();
	void SetPause(bool bPause);
protected:
	//���س�ʼ��
	bool DownloadInit();
	//curl��ʼ��
	bool CurlInit(void* param);
	//���غ���
	static void* DownloadFun(void* param);
	// ͷ��Ϣ 
	static size_t HeaderInfo(char *ptr, size_t size, size_t nmemb, void *userdata);
	//д����
	static size_t WriteData(char *ptr, size_t size, size_t nmemb, void *userdata);
	//�����̲߳���
	static void ParseThreadParam(void* param, CurlDownloader** ppDown, int* pIndex);
	//ͨ����ʱ�ļ����жϵ�����
	bool ContinueDownloadByTmpFile();
	//�����
	bool FinalProc();
	//������ʱ�ļ���ָ���߳̿�������Ϣ
	void UpdateDownloadInfoInTmpFile(int* pIndex);
	//�����߳���Ϣ
	void ClearThreadInfo();
	//���SSL
	static bool CheckSSL(CURL* pCurl, const std::string& strUrl);
	//����ļ�����
	bool CheckFileLength();
	//����Ƿ�֧�ַ�Ƭ����
	bool CheckIsSupportRange();
	bool CheckIsSupportRangeEx();
	// ����curl����
	CURL *Create_Share_Curl();
	//
	void SleepEx(unsigned long ulMilliseconds);
public:
	//�ļ�
	FILE* m_pFile;
	//���ڿ���д�ļ��Ļ������
	pthread_mutex_t m_mutexFile;
	//���ڱ���ÿ���̵߳���Ϣ������
	std::vector<ThreadInfo*> m_vecThrdInfo;
	//���ص�ַ�����ر���·��
	std::string m_strUrl="", m_strDownloadPath="";
	//����
	std::string m_strProxy;
	//�Ƿ���ͣ
	bool m_bPause;
	//�Ƿ���ֹ����
	bool m_bTerminate;
	//�Ƿ�֧�ֶ��߳�����
	bool m_bSupportMultiDown;
	//Ҫ���ص��ļ���С
	unsigned long m_ulFullFileSize;
	//�Ѿ����ص��ļ���С
	unsigned long m_ulDownFileSize;
	//HTTP������
	int m_iHttpCode;
	int m_curlCode;
	//�Ƿ��ض�������ض���֮ǰ��URL
	bool m_bRedirected;
	std::string m_strOriginalUrl;
	// ����dns�������
	static CURLSH* sharedns_handle;
	static CurlDownConfig g_curlDownCfg;
	bool m_bInitOver;
	bool m_bInitInterrupt;
	bool init_success_;
	bool bad_net_;
};

