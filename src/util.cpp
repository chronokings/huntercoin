// Copyright (c) 2009-2010 Satoshi Nakamoto
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.
#include "headers.h"
#include "strlcpy.h"
#include <boost/program_options/detail/config_file.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/interprocess_recursive_mutex.hpp>
#include <boost/foreach.hpp>

using namespace std;
using namespace boost;

map<string, string> mapArgs;
map<string, vector<string> > mapMultiArgs;
bool fDebug = false;
bool fPrintToConsole = false;
bool fPrintToDebugger = false;
char pszSetDataDir[MAX_PATH] = "";
bool fRequestShutdown = false;
bool fShutdown = false;
bool fDaemon = false;
bool fServer = false;
bool fCommandLine = false;
string strMiscWarning;
bool fTestNet = false;
bool fNoListen = false;
bool fLogTimestamps = false;




// Workaround for "multiple definition of `_tls_used'"
// http://svn.boost.org/trac/boost/ticket/4258
extern "C" void tss_cleanup_implemented() { }





// Init openssl library multithreading support
static boost::interprocess::interprocess_mutex** ppmutexOpenSSL;
void locking_callback(int mode, int i, const char* file, int line)
{
    if (mode & CRYPTO_LOCK)
        ppmutexOpenSSL[i]->lock();
    else
        ppmutexOpenSSL[i]->unlock();
}

// Init
class CInit
{
public:
    CInit()
    {
        // Init openssl library multithreading support
        ppmutexOpenSSL = (boost::interprocess::interprocess_mutex**)OPENSSL_malloc(CRYPTO_num_locks() * sizeof(boost::interprocess::interprocess_mutex*));
        for (int i = 0; i < CRYPTO_num_locks(); i++)
            ppmutexOpenSSL[i] = new boost::interprocess::interprocess_mutex();
        CRYPTO_set_locking_callback(locking_callback);

#ifdef __WXMSW__
        // Seed random number generator with screen scrape and other hardware sources
        RAND_screen();
#endif

        // Seed random number generator with performance counter
        RandAddSeed();
    }
    ~CInit()
    {
        // Shutdown openssl library multithreading support
        CRYPTO_set_locking_callback(NULL);
        for (int i = 0; i < CRYPTO_num_locks(); i++)
            delete ppmutexOpenSSL[i];
        OPENSSL_free(ppmutexOpenSSL);
    }
}
instance_of_cinit;








void RandAddSeed()
{
    // Seed with CPU performance counter
    int64 nCounter = GetPerformanceCounter();
    RAND_add(&nCounter, sizeof(nCounter), 1.5);
    memset(&nCounter, 0, sizeof(nCounter));
}

void RandAddSeedPerfmon()
{
    RandAddSeed();

    // This can take up to 2 seconds, so only do it every 10 minutes
    static int64 nLastPerfmon;
    if (GetTime() < nLastPerfmon + 10 * 60)
        return;
    nLastPerfmon = GetTime();

#ifdef __WXMSW__
    // Don't need this on Linux, OpenSSL automatically uses /dev/urandom
    // Seed with the entire set of perfmon data
    unsigned char pdata[250000];
    memset(pdata, 0, sizeof(pdata));
    unsigned long nSize = sizeof(pdata);
    long ret = RegQueryValueExA(HKEY_PERFORMANCE_DATA, "Global", NULL, NULL, pdata, &nSize);
    RegCloseKey(HKEY_PERFORMANCE_DATA);
    if (ret == ERROR_SUCCESS)
    {
        RAND_add(pdata, nSize, nSize/100.0);
        memset(pdata, 0, nSize);
        printf("%s RandAddSeed() %d bytes\n", DateTimeStrFormat("%x %H:%M", GetTime()).c_str(), nSize);
    }
#endif
}

uint64 GetRand(uint64 nMax)
{
    if (nMax == 0)
        return 0;

    // The range of the random source must be a multiple of the modulus
    // to give every possible output value an equal possibility
    uint64 nRange = (UINT64_MAX / nMax) * nMax;
    uint64 nRand = 0;
    do
        RAND_bytes((unsigned char*)&nRand, sizeof(nRand));
    while (nRand >= nRange);
    return (nRand % nMax);
}

int GetRandInt(int nMax)
{
    return GetRand(nMax);
}

string EncodeBase64(const unsigned char* pch, size_t len)
{
    static const char *pbase64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    string strRet="";
    strRet.reserve((len+2)/3*4);

    int mode=0, left=0;
    const unsigned char *pchEnd = pch+len;

    while (pch<pchEnd)
    {
        int enc = *(pch++);
        switch (mode)
        {
            case 0: // we have no bits
                strRet += pbase64[enc >> 2];
                left = (enc & 3) << 4;
                mode = 1;
                break;

            case 1: // we have two bits
                strRet += pbase64[left | (enc >> 4)];
                left = (enc & 15) << 2;
                mode = 2;
                break;

            case 2: // we have four bits
                strRet += pbase64[left | (enc >> 6)];
                strRet += pbase64[enc & 63];
                mode = 0;
                break;
        }
    }

    if (mode)
    {
        strRet += pbase64[left];
        strRet += '=';
        if (mode == 1)
            strRet += '=';
    }

    return strRet;
}

vector<unsigned char> DecodeBase64(const char* p, bool* pfInvalid)
{
    static const int decode64_table[256] =
    {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, 62, -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1,
        -1, -1, -1, -1, -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28,
        29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
        49, 50, 51, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
    };

    if (pfInvalid)
        *pfInvalid = false;

    vector<unsigned char> vchRet;
    vchRet.reserve(strlen(p)*3/4);

    int mode = 0;
    int left = 0;

    while (1)
    {
         int dec = decode64_table[(unsigned char)*p];
         if (dec == -1) break;
         p++;
         switch (mode)
         {
             case 0: // we have no bits and get 6
                 left = dec;
                 mode = 1;
                 break;

              case 1: // we have 6 bits and keep 4
                  vchRet.push_back((left<<2) | (dec>>4));
                  left = dec & 15;
                  mode = 2;
                  break;

             case 2: // we have 4 bits and get 6, we keep 2
                 vchRet.push_back((left<<4) | (dec>>2));
                 left = dec & 3;
                 mode = 3;
                 break;

             case 3: // we have 2 bits and get 6
                 vchRet.push_back((left<<6) | dec);
                 mode = 0;
                 break;
         }
    }

    if (pfInvalid)
        switch (mode)
        {
            case 0: // 4n base64 characters processed: ok
                break;

            case 1: // 4n+1 base64 character processed: impossible
                *pfInvalid = true;
                break;

            case 2: // 4n+2 base64 characters processed: require '=='
                if (left || p[0] != '=' || p[1] != '=' || decode64_table[(unsigned char)p[2]] != -1)
                    *pfInvalid = true;
                break;

            case 3: // 4n+3 base64 characters processed: require '='
                if (left || p[0] != '=' || decode64_table[(unsigned char)p[1]] != -1)
                    *pfInvalid = true;
                break;
        }

    return vchRet;
}











inline int OutputDebugStringF(const char* pszFormat, ...)
{
    int ret = 0;
    if (fPrintToConsole)
    {
        // print to console
        va_list arg_ptr;
        va_start(arg_ptr, pszFormat);
        ret = vprintf(pszFormat, arg_ptr);
        va_end(arg_ptr);
    }
    else
    {
        // print to debug.log
        static FILE* fileout = NULL;

        if (!fileout)
        {
            char pszFile[MAX_PATH+100];
            GetDataDir(pszFile);
            strlcat(pszFile, "/debug.log", sizeof(pszFile));
            fileout = fopen(pszFile, "a");
            if (fileout) setbuf(fileout, NULL); // unbuffered
        }
        if (fileout)
        {
            static bool fStartedNewLine = true;

            // Debug print useful for profiling
            if (fLogTimestamps && fStartedNewLine)
                fprintf(fileout, "%s ", DateTimeStrFormat("%x %H:%M:%S", GetTime()).c_str());
            if (pszFormat[strlen(pszFormat) - 1] == '\n')
                fStartedNewLine = true;
            else
                fStartedNewLine = false;

            va_list arg_ptr;
            va_start(arg_ptr, pszFormat);
            ret = vfprintf(fileout, pszFormat, arg_ptr);
            va_end(arg_ptr);
        }
    }

#ifdef __WXMSW__
    if (fPrintToDebugger)
    {
        static CCriticalSection cs_OutputDebugStringF;

        // accumulate a line at a time
        CRITICAL_BLOCK(cs_OutputDebugStringF)
        {
            static char pszBuffer[50000];
            static char* pend;
            if (pend == NULL)
                pend = pszBuffer;
            va_list arg_ptr;
            va_start(arg_ptr, pszFormat);
            int limit = END(pszBuffer) - pend - 2;
            int ret = _vsnprintf(pend, limit, pszFormat, arg_ptr);
            va_end(arg_ptr);
            if (ret < 0 || ret >= limit)
            {
                pend = END(pszBuffer) - 2;
                *pend++ = '\n';
            }
            else
                pend += ret;
            *pend = '\0';
            char* p1 = pszBuffer;
            char* p2;
            while (p2 = strchr(p1, '\n'))
            {
                p2++;
                char c = *p2;
                *p2 = '\0';
                OutputDebugStringA(p1);
                *p2 = c;
                p1 = p2;
            }
            if (p1 != pszBuffer)
                memmove(pszBuffer, p1, pend - p1 + 1);
            pend -= (p1 - pszBuffer);
        }
    }
#endif
    return ret;
}


// Safer snprintf
//  - prints up to limit-1 characters
//  - output string is always null terminated even if limit reached
//  - return value is the number of characters actually printed
int my_snprintf(char* buffer, size_t limit, const char* format, ...)
{
    if (limit == 0)
        return 0;
    va_list arg_ptr;
    va_start(arg_ptr, format);
    int ret = _vsnprintf(buffer, limit, format, arg_ptr);
    va_end(arg_ptr);
    if (ret < 0 || ret >= limit)
    {
        ret = limit - 1;
        buffer[limit-1] = 0;
    }
    return ret;
}


string vstrprintf(const char *format, va_list ap)
{
    char buffer[50000];
    char* p = buffer;
    int limit = sizeof(buffer);
    int ret;
    loop
    {
        va_list arg_ptr;
        va_copy(arg_ptr, ap);
#ifdef WIN32
        ret = _vsnprintf(p, limit, format, arg_ptr);
#else
        ret = vsnprintf(p, limit, format, arg_ptr);
#endif
        va_end(arg_ptr);
        if (ret >= 0 && ret < limit)
            break;
        if (p != buffer)
            delete[] p;
        limit *= 2;
        p = new char[limit];
        if (p == NULL)
            throw std::bad_alloc();
    }
    string str(p, p+ret);
    if (p != buffer)
        delete[] p;
    return str;
}

string real_strprintf(const char *format, int dummy, ...)
{
    va_list arg_ptr;
    va_start(arg_ptr, dummy);
    string str = vstrprintf(format, arg_ptr);
    va_end(arg_ptr);
    return str;
}

string real_strprintf(const std::string &format, int dummy, ...)
{
    va_list arg_ptr;
    va_start(arg_ptr, dummy);
    string str = vstrprintf(format.c_str(), arg_ptr);
    va_end(arg_ptr);
    return str;
}

bool error(const char* format, ...)
{
    char buffer[50000];
    int limit = sizeof(buffer);
    va_list arg_ptr;
    va_start(arg_ptr, format);
    int ret = _vsnprintf(buffer, limit, format, arg_ptr);
    va_end(arg_ptr);
    if (ret < 0 || ret >= limit)
    {
        ret = limit - 1;
        buffer[limit-1] = 0;
    }
    printf("ERROR: %s\n", buffer);
    return false;
}


void ParseString(const string& str, char c, vector<string>& v)
{
    if (str.empty())
        return;
    string::size_type i1 = 0;
    string::size_type i2;
    loop
    {
        i2 = str.find(c, i1);
        if (i2 == str.npos)
        {
            v.push_back(str.substr(i1));
            return;
        }
        v.push_back(str.substr(i1, i2-i1));
        i1 = i2+1;
    }
}


string FormatMoney(int64 n, bool fPlus)
{
    // Note: not using straight sprintf here because we do NOT want
    // localized number formatting.
    int64 n_abs = (n > 0 ? n : -n);
    int64 quotient = n_abs/COIN;
    int64 remainder = n_abs%COIN;
    string str = strprintf("%"PRI64d".%08"PRI64d, quotient, remainder);

    // Right-trim excess 0's before the decimal point:
    int nTrim = 0;
    for (int i = str.size()-1; (str[i] == '0' && isdigit(str[i-2])); --i)
        ++nTrim;
    if (nTrim)
        str.erase(str.size()-nTrim, nTrim);

    if (n < 0)
        str.insert((unsigned int)0, 1, '-');
    else if (fPlus && n > 0)
        str.insert((unsigned int)0, 1, '+');
    return str;
}


bool ParseMoney(const string& str, int64& nRet)
{
    return ParseMoney(str.c_str(), nRet);
}

bool ParseMoney(const char* pszIn, int64& nRet)
{
    string strWhole;
    int64 nUnits = 0;
    const char* p = pszIn;
    while (isspace(*p))
        p++;
    for (; *p; p++)
    {
        if (*p == '.')
        {
            p++;
            int64 nMult = CENT*10;
            while (isdigit(*p) && (nMult > 0))
            {
                nUnits += nMult * (*p++ - '0');
                nMult /= 10;
            }
            break;
        }
        if (isspace(*p))
            break;
        if (!isdigit(*p))
            return false;
        strWhole.insert(strWhole.end(), *p);
    }
    for (; *p; p++)
        if (!isspace(*p))
            return false;
    if (strWhole.size() > 14)
        return false;
    if (nUnits < 0 || nUnits > COIN)
        return false;
    int64 nWhole = atoi64(strWhole);
    int64 nValue = nWhole*COIN + nUnits;

    nRet = nValue;
    return true;
}

static const signed char phexdigit[256] =
{ -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  0,1,2,3,4,5,6,7,8,9,-1,-1,-1,-1,-1,-1,
  -1,0xa,0xb,0xc,0xd,0xe,0xf,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,0xa,0xb,0xc,0xd,0xe,0xf,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, };

bool IsHex(const string& str)
{
    BOOST_FOREACH(unsigned char c, str)
    {
        if (phexdigit[c] < 0)
            return false;
    }
    return (str.size() > 0) && (str.size()%2 == 0);
}

vector<unsigned char> ParseHex(const char* psz)
{
    // convert hex dump to vector
    vector<unsigned char> vch;
    loop
    {
        while (isspace(*psz))
            psz++;
        signed char c = phexdigit[(unsigned char)*psz++];
        if (c == (signed char)-1)
            break;
        unsigned char n = (c << 4);
        c = phexdigit[(unsigned char)*psz++];
        if (c == (signed char)-1)
            break;
        n |= c;
        vch.push_back(n);
    }
    return vch;
}

vector<unsigned char> ParseHex(const string& str)
{
    return ParseHex(str.c_str());
}

void ParseParameters(int argc, char* argv[])
{
    mapArgs.clear();
    mapMultiArgs.clear();
    for (int i = 1; i < argc; i++)
    {
        char psz[10000];
        strlcpy(psz, argv[i], sizeof(psz));
        char* pszValue = (char*)"";
        if (strchr(psz, '='))
        {
            pszValue = strchr(psz, '=');
            *pszValue++ = '\0';
        }
        #ifdef __WXMSW__
        _strlwr(psz);
        if (psz[0] == '/')
            psz[0] = '-';
        #endif
        if (psz[0] != '-')
            break;
        mapArgs[psz] = pszValue;
        mapMultiArgs[psz].push_back(pszValue);
    }

    // Enforce testnet mode for BETA version
    if (VERSION_IS_BETA && !GetBoolArg("-testnet"))
        mapArgs["-testnet"] = "";
}


const char* wxGetTranslation(const char* pszEnglish)
{
//#ifdef GUI
#if 0
    // Wrapper of wxGetTranslation returning the same const char* type as was passed in
    static CCriticalSection cs;
    CRITICAL_BLOCK(cs)
    {
        // Look in cache
        static map<string, char*> mapCache;
        map<string, char*>::iterator mi = mapCache.find(pszEnglish);
        if (mi != mapCache.end())
            return (*mi).second;

        // wxWidgets translation
        wxString strTranslated = wxGetTranslation(wxString(pszEnglish, wxConvUTF8));

        // We don't cache unknown strings because caller might be passing in a
        // dynamic string and we would keep allocating memory for each variation.
        if (strcmp(pszEnglish, strTranslated.utf8_str()) == 0)
            return pszEnglish;

        // Add to cache, memory doesn't need to be freed.  We only cache because
        // we must pass back a pointer to permanently allocated memory.
        char* pszCached = new char[strlen(strTranslated.utf8_str())+1];
        strcpy(pszCached, strTranslated.utf8_str());
        mapCache[pszEnglish] = pszCached;
        return pszCached;
    }
    return NULL;
#else
    return pszEnglish;
#endif
}


bool WildcardMatch(const char* psz, const char* mask)
{
    loop
    {
        switch (*mask)
        {
        case '\0':
            return (*psz == '\0');
        case '*':
            return WildcardMatch(psz, mask+1) || (*psz && WildcardMatch(psz+1, mask));
        case '?':
            if (*psz == '\0')
                return false;
            break;
        default:
            if (*psz != *mask)
                return false;
            break;
        }
        psz++;
        mask++;
    }
}

bool WildcardMatch(const string& str, const string& mask)
{
    return WildcardMatch(str.c_str(), mask.c_str());
}








void FormatException(char* pszMessage, std::exception* pex, const char* pszThread)
{
#ifdef __WXMSW__
    char pszModule[MAX_PATH];
    pszModule[0] = '\0';
    GetModuleFileNameA(NULL, pszModule, sizeof(pszModule));
#else
    const char* pszModule = "huntercoin";
#endif
    if (pex)
        snprintf(pszMessage, 1000,
            "EXCEPTION: %s       \n%s       \n%s in %s       \n", typeid(*pex).name(), pex->what(), pszModule, pszThread);
    else
        snprintf(pszMessage, 1000,
            "UNKNOWN EXCEPTION       \n%s in %s       \n", pszModule, pszThread);
}

void LogException(std::exception* pex, const char* pszThread)
{
    char pszMessage[10000];
    FormatException(pszMessage, pex, pszThread);
    printf("\n%s", pszMessage);
}

void PrintException(std::exception* pex, const char* pszThread)
{
    char pszMessage[10000];
    FormatException(pszMessage, pex, pszThread);
    printf("\n\n************************\n%s\n", pszMessage);
    fprintf(stderr, "\n\n************************\n%s\n", pszMessage);
    strMiscWarning = pszMessage;
#ifdef GUI
    if (wxTheApp && !fDaemon)
        MyMessageBox(pszMessage, "Huntercoin", wxOK | wxICON_ERROR);
#endif
    throw;
}

void ThreadOneMessageBox(string strMessage)
{
    // Skip message boxes if one is already open
    static bool fMessageBoxOpen;
    if (fMessageBoxOpen)
        return;
    fMessageBoxOpen = true;
#ifdef GUI
    uiInterface.ThreadSafeMessageBox(strMessage, "Huntercoin", wxOK | wxICON_EXCLAMATION);
#else
    ThreadSafeMessageBox(strMessage, "Huntercoin", wxOK | wxICON_EXCLAMATION);
#endif
    fMessageBoxOpen = false;
}

void PrintExceptionContinue(std::exception* pex, const char* pszThread)
{
    char pszMessage[10000];
    FormatException(pszMessage, pex, pszThread);
    printf("\n\n************************\n%s\n", pszMessage);
    fprintf(stderr, "\n\n************************\n%s\n", pszMessage);
    strMiscWarning = pszMessage;
#ifdef GUI
    if (wxTheApp && !fDaemon)
        boost::thread(boost::bind(ThreadOneMessageBox, string(pszMessage)));
#endif
}








#ifdef __WXMSW__
typedef WINSHELLAPI BOOL (WINAPI *PSHGETSPECIALFOLDERPATHA)(HWND hwndOwner, LPSTR lpszPath, int nFolder, BOOL fCreate);

string MyGetSpecialFolderPath(int nFolder, bool fCreate)
{
    char pszPath[MAX_PATH+100] = "";

    // SHGetSpecialFolderPath isn't always available on old Windows versions
    HMODULE hShell32 = LoadLibraryA("shell32.dll");
    if (hShell32)
    {
        PSHGETSPECIALFOLDERPATHA pSHGetSpecialFolderPath =
            (PSHGETSPECIALFOLDERPATHA)GetProcAddress(hShell32, "SHGetSpecialFolderPathA");
        if (pSHGetSpecialFolderPath)
            (*pSHGetSpecialFolderPath)(NULL, pszPath, nFolder, fCreate);
        FreeModule(hShell32);
    }

    // Backup option
    if (pszPath[0] == '\0')
    {
        if (nFolder == CSIDL_STARTUP)
        {
            strcpy(pszPath, getenv("USERPROFILE"));
            strcat(pszPath, "\\Start Menu\\Programs\\Startup");
        }
        else if (nFolder == CSIDL_APPDATA)
        {
            strcpy(pszPath, getenv("APPDATA"));
        }
    }

    return pszPath;
}
#endif

string GetDefaultDataDir()
{
    string strSuffix = GetDefaultDataDirSuffix();

    // Windows: C:\Documents and Settings\username\Application Data\Huntercoin
    // Mac: ~/Library/Application Support/Huntercoin
    // Unix: ~/.huntercoin
#ifdef __WXMSW__
    // Windows
    return MyGetSpecialFolderPath(CSIDL_APPDATA, true) + "\\" + strSuffix;
#else
    char* pszHome = getenv("HOME");
    if (pszHome == NULL || strlen(pszHome) == 0)
        pszHome = (char*)"/";
    string strHome = pszHome;
    if (strHome[strHome.size()-1] != '/')
        strHome += '/';
#ifdef MAC_OSX
    // Mac
    strHome += "Library/Application Support/";
    filesystem::create_directory(strHome.c_str());
    return strHome + strSuffix;
#else
    // Unix
    return strHome + strSuffix;
#endif
#endif
}

void GetDataDir(char* pszDir)
{
    // pszDir must be at least MAX_PATH length.
    int nVariation;
    if (pszSetDataDir[0] != 0)
    {
        strlcpy(pszDir, pszSetDataDir, MAX_PATH);
        nVariation = 0;
    }
    else
    {
        // This can be called during exceptions by printf, so we cache the
        // value so we don't have to do memory allocations after that.
        static char pszCachedDir[MAX_PATH];
        if (pszCachedDir[0] == 0)
            strlcpy(pszCachedDir, GetDefaultDataDir().c_str(), sizeof(pszCachedDir));
        strlcpy(pszDir, pszCachedDir, MAX_PATH);
        nVariation = 1;
    }
    if (fTestNet)
    {
        char* p = pszDir + strlen(pszDir);
        if (p > pszDir && p[-1] != '/' && p[-1] != '\\')
            *p++ = '/';
        strcpy(p, "testnet");
        nVariation += 2;
    }
    static bool pfMkdir[4];
    if (!pfMkdir[nVariation])
    {
        pfMkdir[nVariation] = true;
        boost::filesystem::create_directory(pszDir);
    }
}

string GetDataDir()
{
    char pszDir[MAX_PATH];
    GetDataDir(pszDir);
    return pszDir;
}

string GetConfigFile(string confFile)
{
    namespace fs = boost::filesystem;
    fs::path pathConfig(GetArg("-conf", confFile));
    if (!pathConfig.is_complete())
        pathConfig = fs::path(GetDataDir()) / pathConfig;
    return pathConfig.string();
}

string GetConfigFile()
{
    return GetConfigFile("huntercoin.conf");
}

void ReadConfigFile(map<string, string>& mapSettingsRet,
                    map<string, vector<string> >& mapMultiSettingsRet)
{
    namespace fs = boost::filesystem;
    namespace pod = boost::program_options::detail;

    fs::ifstream streamConfig(GetConfigFile());
    if (!streamConfig.good())
        return;

    set<string> setOptions;
    setOptions.insert("*");
    
    for (pod::config_file_iterator it(streamConfig, setOptions), end; it != end; ++it)
    {
        // Don't overwrite existing settings so command line settings override huntercoin.conf
        string strKey = string("-") + it->string_key;
        if (mapSettingsRet.count(strKey) == 0)
            mapSettingsRet[strKey] = it->value[0];
        mapMultiSettingsRet[strKey].push_back(it->value[0]);
    }
}

string GetPidFile()
{
    namespace fs = boost::filesystem;
    fs::path pathConfig(GetArg("-pid", "huntercoind.pid"));
    if (!pathConfig.is_complete())
        pathConfig = fs::path(GetDataDir()) / pathConfig;
    return pathConfig.string();
}

void CreatePidFile(string pidFile, pid_t pid)
{
    FILE* file;
    if (file = fopen(pidFile.c_str(), "w"))
    {
        fprintf(file, "%d\n", pid);
        fclose(file);
    }
}

int GetFilesize(FILE* file)
{
    int nSavePos = ftell(file);
    int nFilesize = -1;
    if (fseek(file, 0, SEEK_END) == 0)
        nFilesize = ftell(file);
    fseek(file, nSavePos, SEEK_SET);
    return nFilesize;
}

void ShrinkDebugFile()
{
    // Scroll debug.log if it's getting too big
    boost::filesystem::path pathLog(GetDataDir());
    pathLog /= "debug.log";
    FILE* file = fopen(pathLog.string().c_str(), "r");
    if (file && GetFilesize(file) > 10 * 1000000)
    {
        // Restart the file with some of the end
        char pch[200000];
        fseek(file, -sizeof(pch), SEEK_END);
        int nBytes = fread(pch, 1, sizeof(pch), file);
        fclose(file);

        file = fopen(pathLog.string().c_str(), "w");
        if (file)
        {
            fwrite(pch, 1, nBytes, file);
            fclose(file);
        }
    }
    else if (file != NULL)
        fclose(file);
}







//
// "Never go to sea with two chronometers; take one or three."
// Our three time sources are:
//  - System clock
//  - Median of other nodes's clocks
//  - The user (asking the user to fix the system clock if the first two disagree)
//
int64 GetTime()
{
    return time(NULL);
}

static int64 nTimeOffset = 0;

int64 GetTimeOffset()
{
    return nTimeOffset;
}

int64 GetAdjustedTime()
{
    return GetTime() + nTimeOffset;
}

void AddTimeData(unsigned int ip, int64 nTime)
{
    int64 nOffsetSample = nTime - GetTime();

    // Ignore duplicates
    static set<unsigned int> setKnown;
    if (!setKnown.insert(ip).second)
        return;

    // Add data
    static vector<int64> vTimeOffsets;
    if (vTimeOffsets.empty())
        vTimeOffsets.push_back(0);
    vTimeOffsets.push_back(nOffsetSample);
    printf("Added time data, samples %d, offset %+"PRI64d" (%+"PRI64d" minutes)\n", vTimeOffsets.size(), vTimeOffsets.back(), vTimeOffsets.back()/60);
    if (vTimeOffsets.size() >= 5 && vTimeOffsets.size() % 2 == 1)
    {
        sort(vTimeOffsets.begin(), vTimeOffsets.end());
        int64 nMedian = vTimeOffsets[vTimeOffsets.size()/2];
        // Only let other nodes change our time by so much
        if (abs64(nMedian) < 13 * 60)
        {
            nTimeOffset = nMedian;
        }
        else
        {
            nTimeOffset = 0;

            static bool fDone;
            if (!fDone)
            {
                // If nobody has a time different than ours but within 5 minutes of ours, give a warning
                bool fMatch = false;
                BOOST_FOREACH(int64 nOffset, vTimeOffsets)
                    if (nOffset != 0 && abs64(nOffset) < 5 * 60)
                        fMatch = true;

                if (!fMatch)
                {
                    fDone = true;
                    string strMessage = _("Warning: Please check that your computer's date and time are correct.  If your clock is wrong Huntercoin will not work properly.");
                    strMiscWarning = strMessage;
                    printf("*** %s\n", strMessage.c_str());
                    boost::thread(boost::bind(MyMessageBox, strMessage+" ", string("Huntercoin"), wxOK | wxICON_EXCLAMATION, (wxWindow*)NULL, -1, -1));
                }
            }
        }
        BOOST_FOREACH(int64 n, vTimeOffsets)
            printf("%+"PRI64d"  ", n);
        printf("|  nTimeOffset = %+"PRI64d"  (%+"PRI64d" minutes)\n", nTimeOffset, nTimeOffset/60);
    }
}






string FormatVersion(int nVersion)
{
    if (nVersion%100 == 0)
        return strprintf("%d.%d.%02d", nVersion/1000000, (nVersion/10000)%100, (nVersion/100)%100);
    else
        return strprintf("%d.%d.%02d.%d", nVersion/1000000, (nVersion/10000)%100, (nVersion/100)%100, nVersion%100);
}

string FormatFullVersion()
{
    string s = FormatVersion(VERSION) + pszSubVer;
    if (VERSION_IS_BETA) {
        s += "-";
        s += _("beta");
    }
    return s;
}





bool SoftSetArg(const std::string& strArg, const std::string& strValue)
{
    if (mapArgs.count(strArg))
        return false;
    mapArgs[strArg] = strValue;
    return true;
}

bool SoftSetBoolArg(const std::string& strArg, bool fValue)
{
    if (fValue)
        return SoftSetArg(strArg, std::string("1"));
    else
        return SoftSetArg(strArg, std::string("0"));
}
