#include "stdafx.h"
#include <io.h>
#include <iostream>
#include <assert.h>
#include <algorithm>
#include <Sensapi.h>
#include "curl/curl.h"
#include "pthread.h"
#include "CurlDownload.h"

//������ʱ�ļ���׺
#define DOWNLOAD_TMP_FILE_EXT ".dltmp"
//����Ƿ�֧�ֶ��̷߳�Ƭ������ַ���
#define RANGE_TEST_FLAG "RangeTest"
//���ڼ���Ƿ�֧�ֶ��߳����ؽ��մ�С
#define RANGE_TEST_RECV_SIZE 1024

CURLSH* CurlDownloader::sharedns_handle = NULL; 
CurlDownConfig CurlDownloader::g_curlDownCfg;

//�߳���Ϣ
typedef struct _tThreadInfo{
	unsigned long ulBegPos;  //������ʼλ��
	unsigned long ulBlockSize; //���̸߳������ص����ݴ�С
	unsigned long ulRecvSize; //���߳��Ѿ����յ������ݴ�С
	CURL* pCurl;
	pthread_t thrdId;
	int iTryTimes;  //ʧ���Ѿ����Դ���
	_tThreadInfo()
	{
		ulBegPos = 0;
		ulBlockSize = 0;
		ulRecvSize = 0;
		pCurl = NULL;
		thrdId.p = NULL;
		thrdId.x = 0;
		iTryTimes = 0;
	}
}ThreadInfo;

void FFlushEx(FILE* stream)
{
	int duphandle;
	fflush(stream);
	duphandle=dup(fileno(stream));
	close(duphandle);
}

void ReplaceStr(std::string& src, const std::string& target, const std::string& replacement)
{
	if (target == replacement) return;
	std::string::size_type startpos = 0;  
	while (startpos != std::string::npos)  
	{  
		startpos = src.find(target, startpos);      
		if( startpos != std::string::npos )  
		{  
			src.replace(startpos,target.length(),replacement); 
			startpos += replacement.length();
		}  
	}  
}

void SplitStr(const std::string& s, const std::string& delim, std::vector<std::string>* ret) 
{  
	if (!ret) return;
	ret->clear();
	size_t last = 0;  
	size_t index=s.find_first_of(delim,last);  
	while (index!=std::string::npos)  
	{  
		ret->push_back(s.substr(last,index-last));  
		last=index+delim.length();  
		index=s.find_first_of(delim,last);  
	}  
	if (index-last>0)  
	{  
		ret->push_back(s.substr(last,index-last));  
	}  
}  

std::string Log_Format(LPCSTR lpszFormat, ...)
{
	char szLog[1024]={0};
	va_list ap;
	va_start(ap, lpszFormat);
	vsprintf(szLog, lpszFormat, ap);
	va_end(ap);
	return szLog;
}

class MyLog{
public:
	MyLog(LPLOGFUN pLogFun, const char* szFileName, 
		const char* szFuncName, long lLine)
		: m_pLogFun(pLogFun) 
	{
		std::string strFileName(szFileName ? szFileName : "");
		int pos = strFileName.rfind('\\');
		if (pos != -1) strFileName = strFileName.substr(pos + 1, strFileName.size() - pos - 1);
		m_szFileName = strFileName;
		m_szFuncName = std::string(szFuncName ? szFuncName : "");
		m_lLine = lLine;
	};
	~MyLog(){};
	void operator<<(const std::string& strLog)
	{
		if (m_pLogFun) 
		{
			//SYSTEMTIME curTime;
			//GetLocalTime(&curTime);
			char ch[512]={0};
			//sprintf(ch, "%02d:%02d:%02d.%6d %s:%d %s(): ",
			//	curTime.wHour, curTime.wMinute, curTime.wSecond,
			//	curTime.wMilliseconds, m_szFileName.c_str(), m_lLine,
			//	m_szFuncName.c_str());
			sprintf(ch, "%s:%d %s(): ", m_szFileName.c_str(), m_lLine,
				m_szFuncName.c_str());
			std::string strInfo = std::string(ch) + strLog;
			m_pLogFun(strInfo);
		}
	}
private:
	LPLOGFUN m_pLogFun;
	std::string m_szFileName, m_szFuncName;
	long m_lLine;
};

#define RICH_FORMAT_LOG MyLog(g_curlDownCfg.pLogFun, __FILE__, __FUNCTION__, __LINE__)<<Log_Format

CurlDownloader::CurlDownloader(void)
{
	m_pFile = NULL;
	m_bPause = false;
	m_bSupportMultiDown = false;
	m_bTerminate = false;
	m_ulFullFileSize = 0;
	m_ulDownFileSize = 0;
	m_strUrl = m_strDownloadPath = "";
	pthread_mutex_init(&m_mutexFile,NULL);
	m_strProxy = "";
	m_iHttpCode = 200;
	m_bRedirected = false;
	m_strOriginalUrl = "";
	m_curlCode = CURLE_OK;
	m_bInitOver = false;
	m_bInitInterrupt = false;
	init_success_ = false;
}

CurlDownloader::~CurlDownloader(void)
{
	Pause();
	pthread_mutex_lock(&m_mutexFile);
	if (m_pFile)
	{
		fclose(m_pFile);
		m_pFile = NULL;
	}
	pthread_mutex_unlock(&m_mutexFile);
    ClearThreadInfo();
	//�ļ����ʱ����ͷ�
	pthread_mutex_destroy(&m_mutexFile);
}

void CurlDownloader::ClearThreadInfo()
{
	std::vector<ThreadInfo*>::const_iterator ita = m_vecThrdInfo.begin();
	while (ita != m_vecThrdInfo.end())
	{
		ThreadInfo* pInfo = *ita++;
		if (pInfo)
		{
			if (pInfo->pCurl)
			{
				curl_easy_cleanup(pInfo->pCurl);
				pInfo->pCurl = NULL;
			}
			delete pInfo;
			pInfo = NULL;
		}
	}
	m_vecThrdInfo.clear();
}

bool CurlDownloader::Start(const std::string& strUrl, const std::string& strDownloadPath, 
	int pThreadCount)
{
	OutputDebugString(strUrl.c_str());
	int strUrlC = strUrl.size();
	int iThreadCountX = 0;
	if (pThreadCount)
	{
	    assert(pThreadCount);
		iThreadCountX = pThreadCount;
	}
	else
	{
		iThreadCountX = g_curlDownCfg.iMaxThreadCount;
	}
	m_bPause = false;
	m_bTerminate = false;
	m_strUrl = strUrl.c_str();
	//����ԭʼURL
	m_strOriginalUrl = strUrl.c_str();
	m_strDownloadPath = strDownloadPath.c_str();
	//��ֹ���¿�ʼ������Ҫ���³�ʼ����������
	m_ulFullFileSize = 0;
	m_ulDownFileSize = 0;
	m_bSupportMultiDown = false;
    m_bRedirected = false;
	m_curlCode = CURLE_OK;
	m_iHttpCode = 200;
	//
	ClearThreadInfo();
	//ɾ�������ļ�
	DeleteFileA(strDownloadPath.c_str());
	std::string strTmpFile = strDownloadPath.c_str();
	strTmpFile += DOWNLOAD_TMP_FILE_EXT;
	//ͨ����ʱ�ļ��ж��Ƿ��Ѿ����ع�
	if (_access(strTmpFile.c_str(), 0) == -1)
	{
		//�ļ������ڣ��µ����� 
        if (!DownloadInit())
        {
			return false;
        }
		//���ż��̸߳���
		int iCount = (int)ceil(1.0*m_ulFullFileSize/(g_curlDownCfg.iMinBlockSize));
		// �̸߳������ܳ���iThreadCountX 
		const int iThreadCount = (m_bSupportMultiDown ? min(iCount, iThreadCountX) : 1);
		m_ulDownFileSize = 0;
		unsigned long ulFileSize = m_ulFullFileSize;
		//��ʱ��Ϣ����
		const int iTmpInfoLen = iThreadCount*3*sizeof(unsigned long) + sizeof(unsigned long);
		//д����ʱ����
		const unsigned long ulTmpFileLen = ulFileSize + iTmpInfoLen;
		unsigned long ulHasWritten = 0;
		// ���ڴ�ӳ�䴴����ʱ�ļ� 
		bool bFlag = false;
		HANDLE hFile = CreateFileA(strTmpFile.c_str(), GENERIC_READ|GENERIC_WRITE,
			0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile != INVALID_HANDLE_VALUE)
		{
			HANDLE hFileMapping = CreateFileMapping(hFile,NULL,PAGE_READWRITE,0,
				ulTmpFileLen,NULL);
			if (hFileMapping != INVALID_HANDLE_VALUE)
			{
				CloseHandle(hFileMapping);
				bFlag = true;
			}
			CloseHandle(hFile);
		}
		if (!bFlag)
		{
			RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, ��ʱ�ļ�����ʧ��, lastErrorCode=%d"
				, this, GetLastError());
			return false;
		}
		//������ʱ�ļ�	
		m_pFile = fopen(strTmpFile.c_str(), "rb+");
		if (!m_pFile) 
		{
			RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, ��ʱ�ļ���ʧ��, lastErrorCode=%d"
				, this, GetLastError());
			return false;
		}
		//�ļ���λ��δβ�������ֽ���ʼλ��, ���ļ���Сд�����4�ֽ���
		if (_fseeki64(m_pFile, (int)-sizeof(unsigned long), SEEK_END)
			|| 0 == fwrite(&ulFileSize, sizeof(unsigned long), 1, m_pFile)) 
		{
			RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, ��ʱ��Ϣ��ʼ��д��ʧ��, lastErrorCode=%d"
				, this, GetLastError());
			goto Loop;
		}
		//ÿ���̸߳������ص����ݿ��С
		const unsigned long ulBlockSize = ulFileSize/iThreadCount;
		int off = - iTmpInfoLen;
		_fseeki64(m_pFile, off, SEEK_END);
		//�����ļ���С���̸߳���������ÿ���̸߳������ص���������
		for (int i = 0; i < iThreadCount; ++i)
		{
			ThreadInfo* pInfo = new ThreadInfo;
			pInfo->ulBegPos = i * ulBlockSize;
			if (i == iThreadCount - 1)
			{
				pInfo->ulBlockSize = ulFileSize - i * ulBlockSize;
			}
			else
			{
				pInfo->ulBlockSize = ulBlockSize;
			}
			m_vecThrdInfo.push_back(pInfo);
			//���ļ�β��д��ÿ���̵߳�����������Ϣ����ʼλ��(4B)���������ص����ݴ�С(4B)��
			//�Ѿ����صĴ�С(4B)
			fwrite(&pInfo->ulBegPos, sizeof(unsigned long), 1, m_pFile);
			fwrite(&pInfo->ulBlockSize, sizeof(unsigned long), 1, m_pFile);
			fwrite(&pInfo->ulRecvSize, sizeof(unsigned long), 1, m_pFile);
		}
		FFlushEx(m_pFile);

		for (int j = 0; j < iThreadCount; ++j)
		{
			ThreadInfo* pInfo = m_vecThrdInfo[j];
			//���ﴫ���̵߳Ĳ�������ҪpInfo�ĵ�ַ������Ҫm_pFile��m_mutexFile��������Ҫ����
			//thisָ�뼰pInfo��m_vecThrdInfo�е���������ʽ����Ϊ:this+i�ַ�����ʽ
			char* param = new char[32];
			memset(param, 0, sizeof(param));
			sprintf(param, "%X+%X", this, j);
			if (!CurlInit(param)
				|| pthread_create(&pInfo->thrdId, NULL, DownloadFun, param))
			{
				RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, �߳�%d����ʧ��", this, j);
				goto Loop;
			}
		}
		return true;
	}
	else
	{
		//�ļ����ڣ�������ʱ�ļ�����ȡ��һ��δ��ɵ���Ϣ����������
		RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, �����ϴε���ʱ�ļ������жϵ�����", this);
		return ContinueDownloadByTmpFile();
	}

Loop:
	//��ͣ�Ѿ���ʼ���߳�
	Pause();
	FinalProc();
	return false;
}

bool CurlDownloader::Pause()
{
	m_bPause = true;
	bool bRet = true;
	std::vector<ThreadInfo*>::const_iterator ita = m_vecThrdInfo.begin();
	while (ita != m_vecThrdInfo.end())
	{
		ThreadInfo* pInfo = *ita++;
		if (pInfo && pInfo->ulRecvSize < pInfo->ulBlockSize)
		{
			//�߳��Ѿ�ִ�����������ͣ
			if (pInfo->pCurl && pInfo->thrdId.p && pthread_kill(pInfo->thrdId, 0) != ESRCH
				&& pthread_join(pInfo->thrdId, NULL))
			{
				RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, �߳�%d��ͣʧ��", this, 
					(int)(ita - m_vecThrdInfo.begin()) - 1);
				bRet = false;
				//��������ͣʧ�ܾ�������
			}
		}
	}
	return bRet;
}

bool CurlDownloader::Resume()
{
	m_bPause = false;
	bool bRunFinish = true;
	//���߳�����ִ�л��ļ��Ѿ�������ϣ���ֱ���˳�
	if (CheckIsFinish(&bRunFinish) || !bRunFinish) return false;
	//δ��ɵ��߳��ٴο���
	std::vector<ThreadInfo*>::const_iterator ita = m_vecThrdInfo.begin();
	//
	while (ita != m_vecThrdInfo.end())
	{
		ThreadInfo* pInfo = *ita++;
		if (pInfo && pInfo->ulBlockSize > pInfo->ulRecvSize)
		{
			char* param = new char[32];
			memset(param, 0, sizeof(param));
			sprintf(param, "%X+%X", this, int(ita - m_vecThrdInfo.begin()) - 1);
			//û�����꣬��������Ϊ��ͣ��Ҳ��������Ϊ����ʧ��
			if ((!pInfo->pCurl && !CurlInit(param)) 
				|| pthread_create(&pInfo->thrdId, NULL, DownloadFun, param))
			{
				RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, �߳�%d��ԭʧ��", this, 
					(int)(ita - m_vecThrdInfo.begin()) - 1);
				//�ָ�ԭ��״̬
				Pause();
				m_bPause = true;
				return false;
			}
			else
			{
				//�̻߳�ԭ�ɹ�
			}
		}
	}
	return true;
}

bool CurlDownloader::Terminate(bool bDeleteFile/* = true*/)
{
	m_bTerminate = true;
	if (Pause())
	{
		pthread_mutex_lock(&m_mutexFile);
		if (m_pFile)
		{
			fclose(m_pFile);
			m_pFile = NULL;
		}
		pthread_mutex_unlock(&m_mutexFile);
		if (bDeleteFile)
		{
			//ɾ����ʱ�ļ��������ļ�
			DeleteFileA(m_strDownloadPath.c_str());
			std::string strTmpPath = m_strDownloadPath + DOWNLOAD_TMP_FILE_EXT;
			DeleteFileA(strTmpPath.c_str());
		}
		//
		m_ulDownFileSize = 0;
		ClearThreadInfo();
		return true;
	}
	return false;
}

bool CurlDownloader::CurlInit(void* param)
{
	CurlDownloader* pDown = NULL;
	ThreadInfo* pInfo = NULL;
	int index = -1;
	ParseThreadParam(param, &pDown, &index);
	if (pDown && index != -1) pInfo = pDown->m_vecThrdInfo[index];
	if (pInfo)
	{
		//CURL* _curl = curl_easy_init(); 
		CURL* _curl = Create_Share_Curl();
		if (!_curl) return false;
		// ���Ӵ������� 
		CURLcode code;
        if (!pDown->m_strProxy.empty())
        {
			code = (CURLcode)curl_easy_setopt(_curl, CURLOPT_PROXY, pDown->m_strProxy.c_str());
			if (code != CURLE_OK) goto Loop;
        }
		code = (CURLcode)curl_easy_setopt(_curl, CURLOPT_URL, pDown->m_strUrl.c_str());
		if (code != CURLE_OK) goto Loop;
		// ֧��SSL 
		if (!CheckSSL(_curl, pDown->m_strUrl)) goto Loop;

		code = (CURLcode)curl_easy_setopt(_curl, CURLOPT_WRITEFUNCTION, 
			&CurlDownloader::WriteData);  
		if (code != CURLE_OK) goto Loop;
		code = (CURLcode)curl_easy_setopt(_curl, CURLOPT_WRITEDATA, param); 
		if (code != CURLE_OK) goto Loop;
		//�ض���
		code = (CURLcode)curl_easy_setopt(_curl, CURLOPT_FOLLOWLOCATION, 1L); 
		if (code != CURLE_OK) goto Loop;
		// �Ż����ܣ���ֹ��ʱ���� 
		code = (CURLcode)curl_easy_setopt(_curl, CURLOPT_NOSIGNAL, 1L);  
		if (code != CURLE_OK) goto Loop;
		// �������ٶ�< 1 �ֽ�/�� ���� 5 ��ʱ,�����ӻ���ֹ����������ƽ��ճ�ʱ 
		code = (CURLcode)curl_easy_setopt(_curl, CURLOPT_LOW_SPEED_LIMIT, 1L);  
		if (code != CURLE_OK) goto Loop;
		code = (CURLcode)curl_easy_setopt(_curl, CURLOPT_LOW_SPEED_TIME, 30L); 
		if (code != CURLE_OK) goto Loop;
		// ֻ���������ӳ�ʱ����Ϊ����ʱ�䲻�ɿ�
		code = (CURLcode)curl_easy_setopt(_curl, CURLOPT_CONNECTTIMEOUT, 
			g_curlDownCfg.iTimeOut);
		if (code != CURLE_OK) goto Loop;
		pInfo->pCurl = _curl;
		pInfo->iTryTimes = 0;
		//�̳߳�ʼ�����!
		return true;
Loop:
		curl_easy_cleanup(_curl);
		return false;
	}
	return false;
}

void* CurlDownloader::DownloadFun(void* param)
{
	CurlDownloader* pDown = NULL;
	ThreadInfo* pInfo = NULL;
	int index = -1;
	long lCode = 0;
	ParseThreadParam(param, &pDown, &index);
	if (pDown && index != -1) pInfo = pDown->m_vecThrdInfo[index];
	if (pInfo)
	{
		unsigned long ulBegPos, ulUnRecvSize, ulCurRecvSize;
		CURLcode code;
		bool bRet;
		const int iOnceMaxRecvSize = g_curlDownCfg.iMinBlockSize;
		while (!pDown->m_bPause)
		{
			// ���Ի��� 
	    ReTry:
			//�ֶ�����
			if (pInfo->ulRecvSize < pInfo->ulBlockSize)
			{
				//δ���յ����ݴ�С
				ulUnRecvSize = pInfo->ulBlockSize - pInfo->ulRecvSize;
				//����Ҫ���յ����ݴ�С
				ulCurRecvSize = ulUnRecvSize;				
                //ȷ����ʼλ��
				ulBegPos = pInfo->ulBegPos + pInfo->ulRecvSize;
				bRet = false;
				// ���ڲ�֧�ֶ��߳����صģ�������Range����ʱ����ղ������� 
				if (pDown->m_bSupportMultiDown)
				{
					char* range = new char[32];
					memset(range, 0, sizeof(range));
					// ������һ�ֽڣ���ֹ����ʱ�����һ�ֽ����� 
					sprintf(range, "%lu-%lu", ulBegPos, ulBegPos + ulCurRecvSize/* - 1*/);
					code = (CURLcode)curl_easy_setopt(pInfo->pCurl, CURLOPT_RANGE, range);
				}
				else
				{
					code = (CURLcode)CURLE_OK;
				}
				if (code == CURLE_OK)
				{
					code = (CURLcode)curl_easy_perform(pInfo->pCurl);
					if (code == CURLE_OK)
					{
						// ��ʱ����CURLE_OK����ȴû���յ�����
						bRet = (ulUnRecvSize > pInfo->ulBlockSize - pInfo->ulRecvSize);
					}
					// ����CURLE_OK��û���յ����ݣ�Ҳ��Ҫ����
					//else
					if (!bRet)
					{
						DWORD dwFlags;
						// ������ʧ����������ͣ����ֹ��������򲻱����� 
						if (!pDown->m_bPause && !pDown->m_bTerminate
							&& IsNetworkAlive(&dwFlags))
						{
							// ��ȡ������ 
							curl_easy_getinfo(pInfo->pCurl, CURLINFO_RESPONSE_CODE, &lCode);
							pDown->m_iHttpCode = lCode;
							pDown->m_curlCode = code;
							if (pInfo->iTryTimes < g_curlDownCfg.iMaxTryTimes)
							{
								// ���³�ʼ�� ���Դ���Ҫ����
								int tryTimes = pInfo->iTryTimes;
								curl_easy_cleanup(pInfo->pCurl);
								pInfo->pCurl = NULL;
								if (pDown->CurlInit(param)/*1*/)
								{
									// �����Ѿ����ԵĴ��� 
									pInfo->iTryTimes = tryTimes;
									// ����ʱ�䣺һ�룬���룬���� 
									float iVal = pow(2.0, pInfo->iTryTimes);
									pDown->SleepEx(iVal*1000);
									pInfo->iTryTimes++;
									// ���ڵ��߳����أ�����Ҫ��0��ʼ 
									if (!pDown->m_bSupportMultiDown)
									{
										pInfo->ulRecvSize = 0;
										pDown->m_ulDownFileSize = 0;
									}
									//����ʱ�����ؽ���
									float fPercent = 
										100.0*(pDown->m_ulDownFileSize)/(pDown->m_ulFullFileSize);
										RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, ���ؽ���:%f%%, �߳�%d��������ʧ�ܣ�lastHttpCode: %d, CURLcode: %d, ���Ե�%d������", 
											pDown, fPercent, index, lCode, code, 
											pInfo->iTryTimes);
									goto ReTry;
								}
								else
								{
									RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, �߳�%d��ʼ��ʧ�ܣ��޷���������", pDown, index);
								}
							}
							else
							{
								RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, �߳�%d��������ʧ��",
									pDown, index);
							}
						}
					}
				}   
				//
				if (!bRet)
				{
					RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, �߳�%d��������ʧ�ܣ������룺CURLCode=%d", pDown, index, code);
					break;
				}
			}
			else
			{
				//�����м䷢�����ִ���ֻҪ���ճɹ���������͸�λ
				pDown->m_iHttpCode = 200;
				pDown->m_curlCode = CURLE_OK;
				//���߳������Ѿ��������
				if (pInfo->iTryTimes)
				{
					RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, �߳�%d����������ϣ����Դ�����%d", pDown, index, pInfo->iTryTimes);
				}
				break;
			}
		}

		if (1)
		{
			curl_easy_cleanup(pInfo->pCurl);
			pInfo->pCurl = NULL;
		}
	}
	return NULL;
}


bool CurlDownloader::ContinueDownloadByTmpFile()
{
	std::string strTmpPath = m_strDownloadPath + DOWNLOAD_TMP_FILE_EXT;
	//�Զ�д�����Ʒ�ʽ���ļ�
	m_pFile = fopen(strTmpPath.c_str(), "rb+");
    //���س�ʼ��������������ļ��Ƿ��и���
	if (m_pFile && DownloadInit())
	{
		//��λ���ļ�δβ���ȶ�ȡ��ʱ�ļ��ܳ���
		_fseeki64(m_pFile, 0, SEEK_END);
		long long ulTmpFileSize = _ftelli64(m_pFile);
		//�ٻ�ȡҪ���ص��ļ����ȣ�����ʼ����ȡ���ĳ��ȱȽϣ�����ͬ����������Դ�и��£���Ҫ��������
		unsigned long ulFileSize = 0;
		if (_fseeki64(m_pFile, (int)-sizeof(unsigned long), SEEK_END) == 0
			&& fread(&ulFileSize, sizeof(unsigned long), 1, m_pFile)
			&& ulFileSize == m_ulFullFileSize
			&& ulFileSize < ulTmpFileSize)
		{
			__int64 ulTmp = ulTmpFileSize - ulFileSize - sizeof(unsigned long);
			//������һ�ε��̸߳���
			int iThreadCount = ulTmp/(sizeof(unsigned long)*3);
			m_ulDownFileSize = 0;
			//��λ��������Ϣ������ȡ��һ�ε�������Ϣ
			if (0 == _fseeki64(m_pFile, ulFileSize, SEEK_SET))
			{
				unsigned long ulBegPos, ulBlockSize, ulRecvSize;
				// ��ǰ�ֿ���Ϣ�Ƿ���Ч 
				bool bInfoValid;
				for (int i = 0; i < iThreadCount; ++i)
				{
					bInfoValid = true;
					if (m_bSupportMultiDown)
					{
						//֧�ֶ��߳����أ��ſ��ܶϵ�����
						fread(&ulBegPos, sizeof(unsigned long), 1, m_pFile);
						fread(&ulBlockSize, sizeof(unsigned long), 1, m_pFile);
						fread(&ulRecvSize, sizeof(unsigned long), 1, m_pFile);
						if (ulBegPos == -1 || ulBlockSize == -1 || ulRecvSize == -1 
							|| ulBegPos >= ulFileSize || ulBlockSize > ulFileSize 
							|| ulRecvSize > ulFileSize || ulRecvSize > ulBlockSize
							|| ulBlockSize == 0)
						{
							bInfoValid = false;
						}

						if (i == 0 && ulBegPos != 0)
						{
							bInfoValid = false;
						}
						
					}
					else
					{
						//���̲߳�֧�ֶϵ�������ֱ�����¿�ʼ
						RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, ���̲߳�֧�ֶϵ�������ֱ�����¿�ʼ", this);
						ulBegPos = 0;
						ulBlockSize = ulFileSize;
						ulRecvSize = 0;
					}
					//
					// ���ϵ���Ϣ�Ƿ���Ч
					if (i)
					{
						ThreadInfo* pLastInfo = m_vecThrdInfo[i - 1];
						if (pLastInfo->ulBegPos + pLastInfo->ulBlockSize != ulBegPos)
						{
							bInfoValid = false;
						}
					}

					ThreadInfo* pInfo = new ThreadInfo;
					if (bInfoValid)
					{
						//�˷ֿ���Ϣ��Ч
						pInfo->ulBegPos = ulBegPos;
						pInfo->ulBlockSize = ulBlockSize;
						pInfo->ulRecvSize = ulRecvSize;
						m_ulDownFileSize += ulRecvSize;
					}
					else
					{
						//�˷ֿ���Ϣ��Ч����֮���������ش˿�
						RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, �ϵ�������%d����Ϣ���󣬴������ش˿�����", this, i);
						pInfo->ulBegPos = i * (ulFileSize/iThreadCount);
						if (i == iThreadCount - 1)
						{
							pInfo->ulBlockSize = ulFileSize - i * (ulFileSize/iThreadCount);
						}
						else
						{
							pInfo->ulBlockSize = ulFileSize/iThreadCount;
						}
					}
					m_vecThrdInfo.push_back(pInfo);
				}
				
				RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, �ϵ�������ʼ����һ�������ذٷֱ�%f%%", this, m_ulDownFileSize*100.0/m_ulFullFileSize);
				//
				for (int j = 0; j < iThreadCount; ++j)
				{
					ThreadInfo* pInfo = m_vecThrdInfo[j];
					if (pInfo->ulRecvSize < pInfo->ulBlockSize)
					{
						//�����߳���һ��δ�����꣬���¿�ʼ����
						//���ﴫ���̵߳Ĳ�������ҪpInfo�ĵ�ַ������Ҫm_pFile��m_mutexFile��������Ҫ����
						//thisָ�뼰pInfo��m_vecThrdInfo�е���������ʽ����Ϊ:this+i�ַ�����ʽ
						char* param = new char[32];
						memset(param, 0, sizeof(param));
						sprintf(param, "%X+%X", this, j);
						if (!CurlInit(param) 
							|| pthread_create(&pInfo->thrdId, NULL, DownloadFun, param))
						{
							//��ͣ�Ѿ���ʼ������
							Pause();
							fclose(m_pFile);
							m_pFile = NULL;
							return false;
						}
					}		     
				}
				//
				return true;
			}
		}
	}

	RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, ��ʱ�ļ������⣬���¿�ʼ����", this);
	//��ʱ�ļ������⣬ɾ�������¿�ʼ����
	pthread_mutex_lock(&m_mutexFile);
	if (m_pFile) 
	{
		fclose(m_pFile);
		m_pFile = NULL;
	}
	pthread_mutex_unlock(&m_mutexFile);
	//��ɾ������������ ���ɾ��������ʱ�ļ������
	if (_access(strTmpPath.c_str(), 0) != -1 && !DeleteFileA(strTmpPath.c_str()))
	{
		char ch[16]={0};
		int flag = 0;
		std::string strPath;
		do 
		{
			memset(ch, 0, sizeof(ch));
			sprintf(ch, "~bk%d", flag++);
			strPath = m_strDownloadPath + std::string(ch);
		} while (_access(strPath.c_str(), 0) != -1);
		m_strDownloadPath = strPath;
		RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, ��ʱ�ļ���ռ���޷�ɾ������������Ŀ���ļ���Ϊ%s", this, strPath.c_str());
	}
	return Start(m_strUrl, m_strDownloadPath);
}

CURL *CurlDownloader::Create_Share_Curl()
{  
	CURL *curl_handle = curl_easy_init();
	curl_easy_setopt(curl_handle, CURLOPT_SHARE, sharedns_handle);  
	curl_easy_setopt(curl_handle, CURLOPT_DNS_CACHE_TIMEOUT, 60 * 5);
	return curl_handle;
}

bool CurlDownloader::DownloadInit()
{
	m_bInitOver = false;
	m_bInitInterrupt = false;
	DWORD dwTick = GetTickCount();
	RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p DownloadInit begin", this);
    bool bInitSuccess = false;
	//ȷ���ļ�����
	if (CheckFileLength())
	{
		//ȷ���Ƿ�֧�ַ�Ƭ����
		if (CheckIsSupportRange())
		{
			bInitSuccess = true;
			if (m_bSupportMultiDown)
			{
				//��һ��ȷ���Ƿ�֧��
				if (!CheckIsSupportRangeEx())
				{
					bInitSuccess = false;
				}
			}
		}
	}
	m_bInitOver = true;
	init_success_ = true;
	if (!bInitSuccess)
	{
		init_success_ = false;
		RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p DownloadInit Fail,Take time=%d", this, GetTickCount() - dwTick);
		return false;
	}
	//
	RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p DownloadInit end, IsMutilDonwload: %d, FileSize: %dB, Take time=%d", this, (m_bSupportMultiDown ? 1 : 0), m_ulFullFileSize, GetTickCount() - dwTick);
	//
	// 0KB�ļ�����֧�����أ���404�� 
	if (m_ulFullFileSize == 0)
	{
		m_iHttpCode = 404;
	}
	else
	{
		//�����м䷢�����ִ���ֻҪ���ճɹ���������͸�λ
		m_iHttpCode = 200;
		m_curlCode = CURLE_OK;
	}

	return m_ulFullFileSize != 0;
}

//��������
void CurlDownloader::ParseThreadParam(void* param, CurlDownloader** ppDown, int* pIndex)
{
	if (param)
	{
		std::string strTmp((const char*)param);
		//��������
		std::string strParam1, strParam2;
		std::string::size_type pos = strTmp.find('+');
		if (pos != std::string::npos)
		{
			strParam1 = strTmp.substr(0, pos);
			strParam2 = strTmp.substr(pos + 1, strTmp.length() - pos + 1);
			CurlDownloader* pDown = (CurlDownloader*)strtoul(strParam1.c_str(), NULL, 16);
			if (ppDown) *ppDown = pDown;
			int index = strtoul(strParam2.c_str(), NULL, 16);
			if (pDown && index < pDown->m_vecThrdInfo.size())
			{
				if (pIndex) *pIndex = index;
			}
		}
	}
}

size_t CurlDownloader::HeaderInfo(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	std::string* pHead = (std::string*)userdata;
	if (pHead)
	{
		pHead->append(std::string((char*)ptr, nmemb));
	}
	return size*nmemb;
}

size_t CurlDownloader::WriteData(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	//�ж��Ƿ�֧�ֶ��̷߳�Ƭ�ϴ�
	if (userdata)
	{
		std::string strData((const char*)userdata);
		if (strData.find(RANGE_TEST_FLAG) == 0)
		{
			std::vector<std::string> vecstr;
			SplitStr(strData, "##", &vecstr);
			int iRangeTestSize = strtoul(vecstr[2].c_str(), NULL, 10);
			int* pRecvSize = (int*)strtoul(vecstr[1].c_str(), NULL, 16);
			if (pRecvSize)
			{
				*pRecvSize += size*nmemb;
				if (*pRecvSize > iRangeTestSize)
				{
					//��ʱ�Ѿ�ȷ����֧�ֶ��߳������ˣ�ֱ�ӷ���
					return size*nmemb - 1;
				}
			}
			return size*nmemb;
		}
	}
	//
	CurlDownloader* pDown = NULL;
	ThreadInfo* pInfo = NULL;
	int index = -1;
	ParseThreadParam(userdata, &pDown, &index);
	if (pDown && index != -1) pInfo = pDown->m_vecThrdInfo[index];
	//
	size_t writeLen = 0;  
	if (pInfo)
	{
		//����ֹ����ֱ�ӷ���
		if (pDown->m_bTerminate)
		{
			return size*nmemb - 1;
		}
		pthread_mutex_lock(&pDown->m_mutexFile);
		if (pInfo->ulRecvSize < pInfo->ulBlockSize)
		{
			if (0 == _fseeki64(pDown->m_pFile, pInfo->ulBegPos + pInfo->ulRecvSize, SEEK_SET))
			{
				unsigned long ulUnRecvSize = pInfo->ulBlockSize - pInfo->ulRecvSize;
				size_t toWriteCount = (ulUnRecvSize > size * nmemb ? nmemb : ulUnRecvSize/size);
				writeLen = fwrite(ptr, size, toWriteCount, pDown->m_pFile);
				pInfo->ulRecvSize += writeLen*size;
			} 
			else
			{
				RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, _fseeki64 error! GetLastError:%d",
					pDown, GetLastError());
			}
		}
		else
		{
			//�߳����ݽ�����
		}
		pthread_mutex_unlock(&pDown->m_mutexFile);
		//
		pDown->m_ulDownFileSize += writeLen;

		// Ϊ�Ż��ϵ�������Ϊ��ʱ������ʱ��Ϣ 
		pDown->UpdateDownloadInfoInTmpFile(&index);

		//������߳���ͣ���������أ����̲߳�������ͣ
		if (pDown->m_bPause/* && pDown->m_bSupportMultiDown*/)
		{
			// ���߳̾�������ͣ���ָ�ʱ�����¿�ʼ���� 
			if (!pDown->m_bSupportMultiDown)
			{
				pInfo->ulRecvSize = 0;
				pDown->m_ulDownFileSize = 0;
			}
			//ͨ������ֵʹcurl_easy_peform����
			return size*nmemb - 1;
		}
	}  
	return /*writeLen*/size*nmemb; 
}

bool CurlDownloader::CheckIsFinish(bool* pbThrdRunFinish)
{
	//�������߳��Ƿ�ִ����
	bool bRunFinish = true;
	//�ļ��Ƿ�������
	bool bDownFinish = true;
	pthread_mutex_lock(&m_mutexFile);
	std::vector<ThreadInfo*>::const_iterator ita = m_vecThrdInfo.begin();
	while (ita != m_vecThrdInfo.end())
	{
		ThreadInfo* pInfo = *ita++;
		if (pInfo)
		{
			if (bDownFinish)		
				bDownFinish = (pInfo->ulBlockSize == pInfo->ulRecvSize);
			// ��ʱpCurl�Ѿ�Ϊ�գ���pthread_killȴ����0 
			if (bRunFinish)
                bRunFinish = (!pInfo->pCurl || !pInfo->thrdId.p 
				|| pthread_kill(pInfo->thrdId, 0) == ESRCH);
			if (!bDownFinish && !bRunFinish) break;
		}
	}
	pthread_mutex_unlock(&m_mutexFile);
	// ��������͵�������Ҳ�� 
	if (pbThrdRunFinish) *pbThrdRunFinish = (bDownFinish ? true : bRunFinish);
	return bDownFinish && !m_vecThrdInfo.empty();
}

void CurlDownloader::UpdateDownloadInfoInTmpFile(int* pIndex)
{
	pthread_mutex_lock(&m_mutexFile);
	if (m_pFile)
	{
		int off = - sizeof(unsigned long) - m_vecThrdInfo.size()*sizeof(unsigned long)*3;
		int iBeg, iEnd;
		if (pIndex)
		{
			//ֻ���´�ָ���߳̿����Ϣ
			iBeg = *pIndex;
			iEnd = iBeg + 1;
			off += (*pIndex)*sizeof(unsigned long)*3;
		}
		else
		{
			//��������
			iBeg = 0;
			iEnd = m_vecThrdInfo.size();
		}
		if (0 == _fseeki64(m_pFile, off, SEEK_END))
		{
			ThreadInfo* pInfo = NULL;
			for (int i = iBeg; i < iEnd; ++i)
			{
				pInfo = m_vecThrdInfo[i];
				fwrite(&pInfo->ulBegPos, sizeof(unsigned long), 1, m_pFile);
				fwrite(&pInfo->ulBlockSize, sizeof(unsigned long), 1, m_pFile);
				fwrite(&pInfo->ulRecvSize, sizeof(unsigned long), 1, m_pFile);
			}
		}
	}
	FFlushEx(m_pFile);
	pthread_mutex_unlock(&m_mutexFile);
}

bool CurlDownloader::FinalProc()
{
	bool bRet = false;
	//�ж��Ƿ��������
	bool bRunFinish = false;
	bool bFileDownFinish = CheckIsFinish(&bRunFinish);
	if (bRunFinish)
	{
		//���߳�ִ���꣬��ر���ʱ�ļ�
		pthread_mutex_lock(&m_mutexFile);
		if (m_pFile)
		{
			int iTryTimes = 0;
			//��û�йرճɹ���������ļ���ռ�ã��ȴ�ռ�����
			while (iTryTimes++ < 3)
			{
				if (fclose(m_pFile) == 0)
				{
					m_pFile = NULL;
					break;
				}
				else
				{
					if (iTryTimes == 3)
					{
						RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, fclose Fail, GetLastError:%d",
							this, GetLastError());
						FFlushEx(m_pFile);
						break;
					}
					Sleep(500);
				}
			}
		}
		pthread_mutex_unlock(&m_mutexFile);
		//���ļ�������ɣ����޸���ʱ�ļ�����
		if (bFileDownFinish)
		{
			std::string strTmpPath = m_strDownloadPath + DOWNLOAD_TMP_FILE_EXT;
			HANDLE hFile = CreateFileA(strTmpPath.c_str(), GENERIC_READ|GENERIC_WRITE, 
				0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			// ���ļ���ռ�ã�����һ�ݣ��ñ��ݵ��ļ������� 
			if (hFile == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION)
			{
				RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, ��ʱ�ļ���ռ�ã�����֮!",
					this);
				std::string strTmpPathX = strTmpPath + ".bk";
				if (CopyFileA(strTmpPath.c_str(), strTmpPathX.c_str(), FALSE))
				{
					RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, ��ʱ�ļ������ɹ�!",
						this);
					strTmpPath = strTmpPathX;
					hFile = CreateFileA(strTmpPath.c_str(), GENERIC_READ|GENERIC_WRITE, 
						0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
				}
				else
				{
					RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, ��ʱ�ļ���ռ�ã�����ʧ��, GetLastError:%d", this, GetLastError());
				}
			}
			//
			if (hFile != INVALID_HANDLE_VALUE)
			{
				int off = - sizeof(unsigned long) - m_vecThrdInfo.size()*sizeof(unsigned long)*3;
				DWORD dwPtr = SetFilePointer(hFile, off, NULL, FILE_END);
				if (dwPtr != INVALID_SET_FILE_POINTER)
				{
					bRet = (SetEndOfFile(hFile) == TRUE);
					if (!bRet)
					{
						RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, SetEndOfFile Fail, GetLastError:%d", this, GetLastError());
					}
				}
				else
				{
					RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, SetFilePointer Fail, GetLastError:%d", this, GetLastError());
				}
				CloseHandle(hFile);
				if (bRet)
				{
					int pos = m_strDownloadPath.rfind("~bk");
					if (pos != std::string::npos)
					{
						m_strDownloadPath = m_strDownloadPath.substr(0, pos);
						RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, ����Ŀ���ļ�������ϵ�����ʱ��ʱ�ļ���ռ�ö����Ĺ������ڻ�ԭΪ%s", this, m_strDownloadPath.c_str());
					}
					//���ļ�������ɣ���������
					bRet = (0 == rename(strTmpPath.c_str(), m_strDownloadPath.c_str()));
					if (!bRet)
					{
						RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, rename Fail, GetLastError:%d",
							this, GetLastError());
					}
				}
			}
			else
			{
				RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, CreateFileA Fail, GetLastError:%d",
					this, GetLastError());
			}
		}
	}
	return bRet;
}

void CurlDownloader::GetProgress(unsigned long* pTotalFileSize, unsigned long* pDownSize)
{
	if (pTotalFileSize) *pTotalFileSize = m_ulFullFileSize;
	if (pDownSize) *pDownSize = m_ulDownFileSize;
}

bool CurlDownloader::IsMutliDownload()
{
	return m_bSupportMultiDown;
}

bool CurlDownloader::IsRedirected()
{
	return m_bRedirected;
}

bool CurlDownloader::WaitForFinish()
{
	std::vector<ThreadInfo*>::const_iterator ita = m_vecThrdInfo.begin();
	while (ita != m_vecThrdInfo.end())
	{
		ThreadInfo* pInfo = *ita++;
		if (pInfo && pInfo->pCurl && pInfo->thrdId.p) pthread_join(pInfo->thrdId, NULL);
	}
	bool bRet = FinalProc();
	return bRet;
}

CurlDownState CurlDownloader::GetCurlDownState()
{
	if (m_bPause)
	{
		return DOWN_PAUSE;
	}
	else if (m_bTerminate)
	{
		return DOWN_TERMINATE;
	}
	return DOWN_PROGRESS;
}

void CurlDownloader::SetProxy(const std::string& strProxy)
{
	m_strProxy = strProxy;
}

int CurlDownloader::GetLastHttpCode()
{
	return m_iHttpCode;
}

bool CurlDownloader::GetDownloadSuccess(){
	return init_success_;
}

int CurlDownloader::GetLastCurlCode()
{
	return m_curlCode;
}

void CurlDownloader::Init()
{
	CURLcode code = (CURLcode)curl_global_init(CURL_GLOBAL_ALL);
	if (!sharedns_handle)  
	{  
		sharedns_handle = curl_share_init();  
		curl_share_setopt(sharedns_handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);  
	}
}

void CurlDownloader::Uninit()
{
	if (sharedns_handle)
	{
		curl_share_cleanup(sharedns_handle);
		sharedns_handle = NULL;
	}
	curl_global_cleanup();
}

void CurlDownloader::SetCurlDownConfig(const CurlDownConfig& downCfg)
{
	g_curlDownCfg = downCfg;
}

bool CurlDownloader::CheckSSL(CURL* pCurl, const std::string& strUrl)
{
	if (!pCurl || strUrl.empty()) return false;
	//
	std::string strTmp = strUrl.substr(0, 5);
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), tolower);
	if (strTmp == "https")
	{
		static std::string strRootDir;
		if (strRootDir.empty())
		{
			char path[MAX_PATH]={0};
			GetModuleFileNameA(NULL, path, MAX_PATH);
		    std::string strPath = std::string(path);
		    ReplaceStr(strPath, "/", "\\");
		    int pos = strPath.rfind('\\');
		    strRootDir = strPath.substr(0, pos);
		}
		std::string strCertPath = strRootDir + std::string("\\") 
			+ std::string(g_curlDownCfg.szSSLCrtName);
		std::string strKeyPath = strRootDir + std::string("\\") 
			+ std::string(g_curlDownCfg.szSSLKeyName);
		if (_access(strCertPath.c_str(), 0) == -1)
		{
			RICH_FORMAT_LOG("%s is not exist!", g_curlDownCfg.szSSLCrtName);
			return false;
		}
		if (_access(strKeyPath.c_str(), 0) == -1)
		{
			RICH_FORMAT_LOG("%s is not exist!", g_curlDownCfg.szSSLKeyName);
			return false;
		}
		curl_easy_setopt(pCurl, CURLOPT_USE_SSL, CURLUSESSL_TRY);
		curl_easy_setopt(pCurl, CURLOPT_SSL_VERIFYPEER, 0);
		curl_easy_setopt(pCurl, CURLOPT_SSL_VERIFYHOST, 1);
		curl_easy_setopt(pCurl, CURLOPT_SSLCERT, strCertPath.c_str());
		curl_easy_setopt(pCurl, CURLOPT_SSLKEY, strKeyPath.c_str());
	}
	return true;
}

bool CurlDownloader::CheckFileLength()
{
	DWORD dwTick = GetTickCount();
	double downloadFileLenth = -1; 
	///////////////////////////////////////////
	// ��ȡ�����ļ�����Ϊ0Ҫ���� 
	int iTryTimes = 0;
	std::string strTmp;
ReTry0:
	std::string strUrl = m_strUrl;
	CURL *curl = Create_Share_Curl(); 
ReTry1:
	if (m_bPause)
	{
		RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, �⵽��ͣ", this);
		m_bInitInterrupt = true;
		return false;
	}
	if (!m_strProxy.empty())
	{
		curl_easy_setopt(curl, CURLOPT_PROXY, m_strProxy.c_str());
	}
	// ֧��SSL 
	if (!CheckSSL(curl, strUrl)) 
	{
		return false;
	}
	//����ض���-ǿ����GET����
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "GET");		
	curl_easy_setopt(curl, CURLOPT_URL, strUrl.c_str());
	curl_easy_setopt(curl, CURLOPT_HEADER, 1); 
	curl_easy_setopt(curl, CURLOPT_NOBODY, 1);
	// ��������ʱ����
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, g_curlDownCfg.iTimeOut);
	// �Ż����ܣ���ֹ��ʱ���� 
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	long lCode = 0;
	if (m_bPause)
	{
		RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, �⵽��ͣ", this);
		m_bInitInterrupt = true;
		return false;
	}
	CURLcode code = (CURLcode)curl_easy_perform(curl);
	if (code == CURLE_OK)  
	{  	
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &lCode);
		if (lCode == 301 || lCode == 302)
		{
			//�����ض��򣬻�ȡ�ض�����url
			char *redirecturl = {0};
			curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redirecturl);
			//myLog(coutx<<"�����ض���RedirectUrl: "<<redicturl<<"\n";)
			RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, �����ض���RedirectUrl: %s",
				this, redirecturl);
			strUrl = redirecturl;
			m_bRedirected = true;
			goto ReTry1;
		}
		else if (lCode >= 200 && lCode < 300)
		{
			if (lCode != 200)
			{
				RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, HttpCode: %d", this, lCode);
			}
			curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &downloadFileLenth); 
		}
	}
	else
	{
		RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, ��ȡ�ļ�����ʧ��, CURLcode=%d", 
			this, code);
	}

	if (curl)
	{
		curl_easy_cleanup(curl);
		curl = NULL;
	}	
	//
	m_curlCode = code;
	m_iHttpCode = lCode;
	//
	// 0�ֽ��ļ���СҲҪ����
	if (downloadFileLenth == -1 || downloadFileLenth == 0)
	{
		if (lCode == 404)
		{
			//��Դ�����ڣ��Ͳ�������
			return false;
		}
		// ���Ի�ȡ�ļ�����
		if (iTryTimes < g_curlDownCfg.iMaxTryTimes)
		{
			iTryTimes++;
			RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, ��ȡ�ļ�����ʧ��, lastHttpCode:%d, CURLcode:%d, ���Ե�%d������", this, lCode, code, iTryTimes);
			DWORD dwFlags;
			// ����ͣ����ֹ��������򲻱����� 
			if (!m_bPause && !m_bTerminate && IsNetworkAlive(&dwFlags))
			{
				// ����ʱ�䣺һ�룬���룬���� 
				float iVal = pow(2.0, iTryTimes);
				SleepEx(iVal*1000);
			}
			goto ReTry0;
		}
		else
		{
			RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, ���Ի�ȡ�ļ�����ʧ��, LastHttpCode:%d", this, lCode);
			//RICHLOG<<"���Ի�ȡ�ļ�����ʧ��";
			RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, end, Take time=%d", this, 
				GetTickCount() - dwTick);
			return false;
		}
	}

	if (iTryTimes)
	{
		RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, ��ȡ�ļ����ȳɹ������Դ�����%d", 
			this, iTryTimes);
	}

	// url����
	m_strUrl = strUrl;
	/////////////////////////////////////////
	//��ȡҪ���ص��ļ���С
	m_ulFullFileSize = (unsigned long)downloadFileLenth; 

	return true;
}

bool CurlDownloader::CheckIsSupportRange()
{
	int iTryTimes = 0;
	std::string strUrl = m_strUrl;
	// �ж��Ƿ�֧�ֶϵ����� 
	// �ļ�С����С�ֿ飬��ʹ�ж�֧�ֶ��̣߳�����ǿ�һ���̣߳���û��Ҫ�ж�
	// ��Ϊ���߳�Ӱ�쵽��ͣ���ָ���������Ҫ�������ж�
	if (m_ulFullFileSize/* > MMBLOCK_SIZE*/)
	{
		// �ж��Ƿ�֧�ֶ��߳�Ҳ���� 
		iTryTimes = 0;
ReTry2:
		if (m_bPause)
		{
			RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, �⵽��ͣ", this);
			m_bInitInterrupt = true;
			return false;
		}
		//curl = curl_easy_init(); 
		CURL* curl = Create_Share_Curl();
		if (!m_strProxy.empty())
		{
			curl_easy_setopt(curl, CURLOPT_PROXY, m_strProxy.c_str());
		}	
		// ֧��SSL 
		if (!CheckSSL(curl, strUrl)) 
		{
			return false;
		}
		curl_easy_setopt(curl, CURLOPT_URL, strUrl.c_str());  
		curl_easy_setopt(curl, CURLOPT_HEADER, 1); 
		curl_easy_setopt(curl, CURLOPT_NOBODY, 1);
		// ͨ��ͷ����Ϣ�ж��Ƿ�֧�ֶϵ�����
		std::string strHeader;
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, &strHeader);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &CurlDownloader::HeaderInfo);
		curl_easy_setopt(curl, CURLOPT_RANGE, "0-");
		// ��������ʱ����
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, g_curlDownCfg.iTimeOut);
		// �Ż����ܣ���ֹ��ʱ���� 
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
		CURLcode code = (CURLcode)curl_easy_perform(curl);
		curl_easy_cleanup(curl);
		curl = NULL;
		if (code == CURLE_OK)
		{
			m_bSupportMultiDown = (/*strHeader.find("Accept-Ranges: bytes") != std::string::npos
				|| */strHeader.find("Content-Range: bytes") != std::string::npos);
		}
		else
		{
			// �����ж��Ƿ�֧�ֶ��߳� 
			if (iTryTimes < g_curlDownCfg.iMaxTryTimes)
			{
				iTryTimes++;
				RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, �ж��Ƿ�֧�ֶ��߳�ʧ��, CURLcode:%d, ���Ե�%d������", this, code, iTryTimes);
				DWORD dwFlags;
				// ����ͣ����ֹ��������򲻱����� 
				if (!m_bPause && !m_bTerminate && IsNetworkAlive(&dwFlags))
				{
					// ����ʱ�䣺һ�룬���룬���� 
					float iVal = pow(2.0, iTryTimes);
					SleepEx(iVal*1000);
				}
				goto ReTry2;
			}
			else
			{
				RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, �ж��Ƿ�֧�ֶ��߳�ʧ�ܣ�Ĭ�ϲ��õ��߳�����", this);
			}
		}
	}
	else
	{
		//RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, �ļ���С<��С�ֿ�5M, �����ж��Ƿ�֧�ֶ��̣߳�ֱ�Ӳ��õ��߳�����", this);
	}
	return true;
}

bool CurlDownloader::CheckIsSupportRangeEx()
{
	if (m_ulFullFileSize)
	{
		//�ж��Ƿ�֧�ֶ��̷߳�Ƭ����
		if (m_bPause)
		{
			RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, �⵽��ͣ", this);
			m_bInitInterrupt = true;
			return false;
		}
		CURL* curl = Create_Share_Curl(); 
		if (!m_strProxy.empty())
		{
			curl_easy_setopt(curl, CURLOPT_PROXY, m_strProxy.c_str());
		}
		// ֧��SSL 
		if (!CheckSSL(curl, m_strUrl)) 
		{
			return false;
		}
		curl_easy_setopt(curl, CURLOPT_URL, m_strUrl.c_str()); 
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &CurlDownloader::WriteData); 
		//
		const int iRangeTestSize = 
			m_ulFullFileSize > RANGE_TEST_RECV_SIZE ? RANGE_TEST_RECV_SIZE : m_ulFullFileSize/2;
		int iRecvSize = 0;
		char* param = new char[32];
		memset(param, 0, sizeof(param));
		sprintf(param, "%s##%X##%d", RANGE_TEST_FLAG, &iRecvSize, iRangeTestSize);
		//
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, param); 
		//�ض���
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1); 
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
		//��������һС���ֽڵ�����
		char range[32] = {0};
		sprintf(range, "1-%d", iRangeTestSize);
		curl_easy_setopt(curl, CURLOPT_RANGE, range); 
		// �������ٶ�< 1 �ֽ�/�� ���� 5 ��ʱ,�����ӻ���ֹ����������ƽ��ճ�ʱ 
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);  
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 5L); 
		// ֻ���������ӳ�ʱ����Ϊ����ʱ�䲻�ɿ�
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, g_curlDownCfg.iTimeOut);
		CURLcode code = (CURLcode)curl_easy_perform(curl);
		if (CURLE_OK == code)
		{
			m_bSupportMultiDown = true;
		}
		else
		{
			m_bSupportMultiDown = false;
		}
		curl_easy_cleanup(curl);
		curl = NULL;
		delete[] param;
		param = NULL;
	}
	else
	{
		//���ļ�̫С��ֱ���õ��߳�
		m_bSupportMultiDown = false;
	}
	return true;
}

std::string CurlDownloader::GetRealUrl()
{
	return m_strUrl;
}

int CurlDownloader::GetThreadCount()
{
	return m_vecThrdInfo.size();
}

void CurlDownloader::SleepEx(unsigned long ulMilliseconds)
{
	unsigned long ulSleeped = 0;
	while (ulSleeped < ulMilliseconds)
	{
		if (m_bPause)
		{
			RICH_FORMAT_LOG("CurlDownloader Obj Ptr: 0X%p, �⵽��ͣ", this);
			break;
		}
		Sleep(100);
		ulSleeped += 100;
	}
}

void CurlDownloader::SetPause(bool bPause)
{
	m_bPause = bPause;
}

bool CurlDownloader::IsDownloadInitOver()
{
	return m_bInitOver;
}

bool CurlDownloader::IsDownloadInitInterrupt()
{
	return m_bInitInterrupt;
}