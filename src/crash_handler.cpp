/*
 * crash_handler.cpp - Modernized crash capture for fxsound_engine.exe
 * x64 native, SEH + MiniDump + StackWalk + set_terminate
 */

#include "crash_handler.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <atomic>
#include <exception>
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <shlwapi.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "version.lib")

static std::wstring s_exeDir, s_crashDumpsDir;
static LPTOP_LEVEL_EXCEPTION_FILTER s_prevFilter = nullptr;
static std::terminate_handler s_prevTerminate = nullptr;
static std::atomic<bool> s_installed{false};
static volatile long s_handling = 0;

static std::string tsFn() { SYSTEMTIME s; GetLocalTime(&s); char b[32]; snprintf(b,32,"%04d%02d%02d_%02d%02d%02d",s.wYear,s.wMonth,s.wDay,s.wHour,s.wMinute,s.wSecond); return b; }
static std::string tsLog(){ SYSTEMTIME s; GetLocalTime(&s); char b[64]; snprintf(b,64,"%04d-%02d-%02d %02d:%02d:%02d.%03d",s.wYear,s.wMonth,s.wDay,s.wHour,s.wMinute,s.wSecond,s.wMilliseconds); return b; }
static std::string fmtA(DWORD64 a){ char b[24]; snprintf(b,24,"0x%016llX",(unsigned long long)a); return b; }

static std::wstring widenAscii(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

static std::string wideToUtf8(const std::wstring& w) {
    if(w.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if(size <= 0) return {};
    std::string out(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), size, nullptr, nullptr);
    return out;
}

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for(char c : s) {
        switch(c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if(static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned char>(c));
                    out += b;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

static bool writeTextFileW(const std::wstring& path, const std::string& text) {
    HANDLE hf = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if(hf == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const DWORD bom = 0xBFBBEF;
    WriteFile(hf, &bom, 3, &written, NULL);
    BOOL ok = WriteFile(hf, text.data(), static_cast<DWORD>(text.size()), &written, NULL);
    CloseHandle(hf);
    return ok == TRUE;
}

static const char* excStr(DWORD c) {
    switch(c) {
        case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
        case EXCEPTION_BREAKPOINT: return "BREAKPOINT";
        default: return "UNKNOWN";
    }
}

static bool ensureDir() {
    if(s_crashDumpsDir.empty()) return false;
    DWORD a=GetFileAttributesW(s_crashDumpsDir.c_str());
    if(a!=INVALID_FILE_ATTRIBUTES && (a&FILE_ATTRIBUTE_DIRECTORY)) return true;
    return SHCreateDirectoryExW(NULL,s_crashDumpsDir.c_str(),NULL)==S_OK || GetLastError()==ERROR_ALREADY_EXISTS;
}

static std::string modName(DWORD64 addr) {
    HMODULE h=NULL;
    if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,(LPCWSTR)(uintptr_t)addr,&h)){
        wchar_t b[MAX_PATH]={}; GetModuleFileNameW(h,b,MAX_PATH);
        std::wstring path(b);
        size_t pos = path.find_last_of(L"\\/");
        return wideToUtf8(pos == std::wstring::npos ? path : path.substr(pos + 1));
    }
    return "unknown";
}

static std::string osVer() {
    typedef LONG(WINAPI*Fn)(RTL_OSVERSIONINFOW*);
    HMODULE n=GetModuleHandleW(L"ntdll.dll"); if(!n) return "Windows";
    auto f=(Fn)GetProcAddress(n,"RtlGetVersion"); if(!f) return "Windows";
    RTL_OSVERSIONINFOW v={}; v.dwOSVersionInfoSize=sizeof(v);
    if(f(&v)!=0) return "Windows";
    char b[80]; snprintf(b,80,"Windows %lu.%lu Build %lu",v.dwMajorVersion,v.dwMinorVersion,v.dwBuildNumber);
    return b;
}

static bool writeDump(EXCEPTION_POINTERS* pEP, const std::wstring& path) {
    HANDLE hf=CreateFileW(path.c_str(),GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(hf==INVALID_HANDLE_VALUE) return false;
    MINIDUMP_EXCEPTION_INFORMATION mei={}; mei.ThreadId=GetCurrentThreadId();
    mei.ExceptionPointers=pEP; mei.ClientPointers=FALSE;
    MINIDUMP_TYPE dt=(MINIDUMP_TYPE)(MiniDumpWithDataSegs|MiniDumpWithHandleData|
        MiniDumpWithFullMemoryInfo|MiniDumpWithThreadInfo|MiniDumpWithUnloadedModules|
        MiniDumpWithIndirectlyReferencedMemory);
    BOOL ok=MiniDumpWriteDump(GetCurrentProcess(),GetCurrentProcessId(),hf,dt,pEP?&mei:NULL,NULL,NULL);
    CloseHandle(hf);
    if(!ok){ hf=CreateFileW(path.c_str(),GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
        if(hf!=INVALID_HANDLE_VALUE){ ok=MiniDumpWriteDump(GetCurrentProcess(),GetCurrentProcessId(),hf,MiniDumpWithDataSegs,pEP?&mei:NULL,NULL,NULL); CloseHandle(hf); }}
    return ok==TRUE;
}

static void writeStack(std::ostringstream& r, EXCEPTION_POINTERS* pEP) {
    HANDLE hp=GetCurrentProcess(),ht=GetCurrentThread();
    SymSetOptions(SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS|SYMOPT_LOAD_LINES);
    if(!SymInitialize(hp,NULL,TRUE)){ r<<"  [SymInit failed]\n"; return; }
    if(!s_exeDir.empty()){ SymSetSearchPathW(hp,s_exeDir.c_str()); }
    CONTEXT ctx={}; if(pEP&&pEP->ContextRecord) ctx=*pEP->ContextRecord; else RtlCaptureContext(&ctx);
    STACKFRAME64 sf={}; sf.AddrPC.Offset=ctx.Rip; sf.AddrPC.Mode=AddrModeFlat;
    sf.AddrFrame.Offset=ctx.Rbp; sf.AddrFrame.Mode=AddrModeFlat;
    sf.AddrStack.Offset=ctx.Rsp; sf.AddrStack.Mode=AddrModeFlat;
    r<<"\n--- Call Stack ---\n";
    for(int i=0;i<64;i++){
        if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64,hp,ht,&sf,&ctx,NULL,SymFunctionTableAccess64,SymGetModuleBase64,NULL)||sf.AddrPC.Offset==0) break;
        DWORD64 pc=sf.AddrPC.Offset; std::string mn=modName(pc);
        char sb[sizeof(SYMBOL_INFO)+MAX_SYM_NAME]={}; SYMBOL_INFO* si=(SYMBOL_INFO*)sb;
        si->SizeOfStruct=sizeof(SYMBOL_INFO); si->MaxNameLen=MAX_SYM_NAME;
        DWORD64 d=0; BOOL hs=SymFromAddr(hp,pc,&d,si);
        IMAGEHLP_LINE64 ln={}; ln.SizeOfStruct=sizeof(ln); DWORD ld=0;
        BOOL hl=SymGetLineFromAddr64(hp,pc,&ld,&ln);
        char buf[512];
        if(hs&&hl) snprintf(buf,512,"  #%02d %s!%s+0x%llX [%s:%lu]",i,mn.c_str(),si->Name,(unsigned long long)d,ln.FileName,ln.LineNumber);
        else if(hs) snprintf(buf,512,"  #%02d %s!%s+0x%llX (%s)",i,mn.c_str(),si->Name,(unsigned long long)d,fmtA(pc).c_str());
        else snprintf(buf,512,"  #%02d %s + 0x%llX",i,mn.c_str(),(unsigned long long)pc);
        r<<buf<<"\n"; if(i>0&&sf.AddrReturn.Offset==0) break;
    }
    SymCleanup(hp);
}

static void writeMods(std::ostringstream& r) {
    r<<"\n--- Modules ---\n";
    HANDLE hs=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,GetCurrentProcessId());
    if(hs==INVALID_HANDLE_VALUE){ r<<"  [failed]\n"; return; }
    MODULEENTRY32W me={}; me.dwSize=sizeof(me);
    if(Module32FirstW(hs,&me)){ do {
        char mn[MAX_PATH]={}; WideCharToMultiByte(CP_UTF8,0,me.szModule,-1,mn,MAX_PATH,NULL,NULL);
        char line[512]; snprintf(line,512,"  %-30s base=%s size=%u",mn,fmtA((DWORD64)(uintptr_t)me.modBaseAddr).c_str(),me.modBaseSize);
        r<<line<<"\n";
    } while(Module32NextW(hs,&me)); }
    CloseHandle(hs);
}

static void writeRegs(std::ostringstream& r, EXCEPTION_POINTERS* pEP) {
    if(!pEP||!pEP->ContextRecord){ r<<"\n--- Registers ---\n  [none]\n"; return; }
    const CONTEXT* c=pEP->ContextRecord;
    r<<"\n--- Registers (x64) ---\n"
      <<"  RIP="<<fmtA(c->Rip)<<" RSP="<<fmtA(c->Rsp)<<" RBP="<<fmtA(c->Rbp)<<" RAX="<<fmtA(c->Rax)<<"\n"
      <<"  RBX="<<fmtA(c->Rbx)<<" RCX="<<fmtA(c->Rcx)<<" RDX="<<fmtA(c->Rdx)<<" RSI="<<fmtA(c->Rsi)<<"\n"
      <<"  RDI="<<fmtA(c->Rdi)<<" R8 ="<<fmtA(c->R8) <<" R9 ="<<fmtA(c->R9) <<" R10="<<fmtA(c->R10)<<"\n";
}

// --- SEH handler ---
static LONG CALLBACK sehHandler(EXCEPTION_POINTERS* pEP) {
    if(InterlockedExchange(&s_handling,1)!=0) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code=pEP->ExceptionRecord->ExceptionCode;
    DWORD64 addr=(DWORD64)pEP->ExceptionRecord->ExceptionAddress;
    ensureDir();
    std::string ts=tsFn(); DWORD pid=GetCurrentProcessId();
    std::wstring base = s_crashDumpsDir + L"\\crash_" + widenAscii(ts) + L"_" + std::to_wstring(pid);
    std::wstring dp=base + L".dmp";
    std::wstring tp=base + L".txt";
    std::string dp8=wideToUtf8(dp);
    std::string tp8=wideToUtf8(tp);
    bool dok=writeDump(pEP,dp);
    std::ostringstream rpt;
    rpt<<"=== FxSound Engine Crash Report ===\n"
        <<"Time:      "<<tsLog()<<"\nPID:       "<<pid<<"\nOS:        "<<osVer()<<"\n"
        <<"Reason:    Unhandled SEH\nException: 0x"<<std::hex<<code<<std::dec<<" ("<<excStr(code)<<")\n"
        <<"Address:   "<<fmtA(addr)<<"\nModule:    "<<modName(addr)<<"\nDump:      "<<(dok?dp8:"[FAILED]")<<"\n";
    if(code==EXCEPTION_ACCESS_VIOLATION&&pEP->ExceptionRecord->NumberParameters>=2){
        ULONG_PTR fl=pEP->ExceptionRecord->ExceptionInformation[0];
        ULONG64 ma=pEP->ExceptionRecord->ExceptionInformation[1];
        rpt<<"AccessType: "<<(fl==0?"READ":fl==1?"WRITE":"DEP")<<"\nMemAddr:    "<<fmtA(ma)<<"\n";
    }
    writeStack(rpt,pEP); writeRegs(rpt,pEP); writeMods(rpt);
    rpt<<"\n=== End ===\n";
    writeTextFileW(tp, rpt.str());
    fprintf(stdout,"{\"type\":\"crash\",\"exception\":\"%s\",\"address\":\"%s\",\"dump\":\"%s\",\"report\":\"%s\"}\n",
            excStr(code),fmtA(addr).c_str(),jsonEscape(dp8).c_str(),jsonEscape(tp8).c_str());
    fflush(stdout);
    fprintf(stderr,"[CRASH] %s at %s in %s -> %s\n",excStr(code),fmtA(addr).c_str(),modName(addr).c_str(),dp8.c_str());
    return EXCEPTION_EXECUTE_HANDLER;
}

// --- C++ terminate handler ---
static void terminateHandler() {
    if(InterlockedExchange(&s_handling,1)!=0){ if(s_prevTerminate)s_prevTerminate(); std::abort(); }
    ensureDir(); std::string ts=tsFn(); DWORD pid=GetCurrentProcessId();
    std::wstring tp=s_crashDumpsDir+L"\\crash_"+widenAscii(ts)+L"_"+std::to_wstring(pid)+L".txt";
    std::string tp8=wideToUtf8(tp);
    std::string ew="unknown";
    try{ std::rethrow_exception(std::current_exception()); } catch(const std::exception& e){ ew=std::string("std::exception: ")+e.what(); } catch(...){ ew="unknown type"; }
    std::ostringstream rpt;
    rpt<<"=== FxSound Engine Crash Report ===\nTime: "<<tsLog()<<"\nPID: "<<pid<<"\nOS: "<<osVer()
        <<"\nReason: Unhandled C++ Exception (std::terminate)\nDetails: "<<ew<<"\n";
    writeStack(rpt,nullptr); writeMods(rpt); rpt<<"\n=== End ===\n";
    writeTextFileW(tp, rpt.str());
    fprintf(stdout,"{\"type\":\"crash\",\"exception\":\"cpp_terminate\",\"details\":\"%s\",\"report\":\"%s\"}\n",
            jsonEscape(ew).c_str(),jsonEscape(tp8).c_str());
    fflush(stdout); fprintf(stderr,"[CRASH] C++ unhandled: %s -> %s\n",ew.c_str(),tp8.c_str());
    if(s_prevTerminate) s_prevTerminate(); std::abort();
}

namespace CrashHandler {
bool install(const std::wstring& exeDir) {
    if(s_installed) return true;
    s_exeDir=exeDir; s_crashDumpsDir=exeDir+L"\\crash_dumps";
    s_prevFilter=SetUnhandledExceptionFilter(sehHandler);
    s_prevTerminate=std::set_terminate(terminateHandler);
    s_installed=true;
    fprintf(stderr,"[CrashHandler] Installed. Dir: %s\n",wideToUtf8(s_crashDumpsDir).c_str());
    return true;
}
void uninstall(){ if(!s_installed)return; SetUnhandledExceptionFilter(s_prevFilter); std::set_terminate(s_prevTerminate); s_installed=false; }
bool isInstalled(){ return s_installed.load(); }
std::wstring getCrashDumpsDir(){ return s_crashDumpsDir; }

std::vector<std::wstring> findCrashDumps() {
    std::vector<std::wstring> r; if(s_crashDumpsDir.empty()) return r;
    std::wstring pat=s_crashDumpsDir+L"\\crash_*.dmp";
    WIN32_FIND_DATAW fd={}; HANDLE hf=FindFirstFileW(pat.c_str(),&fd);
    if(hf==INVALID_HANDLE_VALUE) return r;
    do{ r.push_back(s_crashDumpsDir+L"\\"+fd.cFileName); } while(FindNextFileW(hf,&fd));
    FindClose(hf); return r;
}

int cleanupOldDumps(int maxAgeDays) {
    int count=0; FILETIME now; SYSTEMTIME st; GetSystemTime(&st); SystemTimeToFileTime(&st,&now);
    ULONGLONG nowNs=((ULONGLONG)now.dwHighDateTime<<32)|now.dwLowDateTime;
    ULONGLONG maxAge=(ULONGLONG)maxAgeDays*24*3600*10000000;
    auto dumps=findCrashDumps();
    for(auto& p:dumps){
        HANDLE hf=CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
        if(hf==INVALID_HANDLE_VALUE) continue;
        FILETIME ft; if(GetFileTime(hf,NULL,NULL,&ft)){
            ULONGLONG fNs=((ULONGLONG)ft.dwHighDateTime<<32)|ft.dwLowDateTime;
            if(nowNs-fNs>maxAge){ CloseHandle(hf); if(DeleteFileW(p.c_str())) count++; continue; }
        }
        CloseHandle(hf);
    }
    return count;
}
}
