/* Minimal Win32 declarations (no SDK needed) */
#pragma once
typedef unsigned char BYTE; typedef unsigned short WORD; typedef unsigned int DWORD, UINT;
typedef int BOOL, LONG, HRESULT; typedef unsigned long long ULONG_PTR, SIZE_T, u64;
typedef long long LONG_PTR, i64; typedef void *HANDLE, *HMODULE, *HWND, *LPVOID;
typedef unsigned short wchar; typedef const wchar *LPCWSTR; typedef wchar *LPWSTR;
#define WINAPI __stdcall
#define TRUE 1
#define FALSE 0
#define NULL ((void*)0)
#define MAX_PATH 260
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_FREE 0x10000
#define PAGE_EXECUTE_READWRITE 0x40
#define GENERIC_WRITE 0x40000000
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define CREATE_ALWAYS 2
#define FILE_ATTRIBUTE_NORMAL 0x80
#define CP_UTF8 65001
#define CP_ACP 0
#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0
typedef struct { void *a,*b; LONG c,d; HANDLE e; ULONG_PTR f; } CRITICAL_SECTION;
typedef struct { void* BaseAddress; void* AllocationBase; DWORD AllocationProtect; WORD PartitionId; SIZE_T RegionSize; DWORD State, Protect, Type; } MEMORY_BASIC_INFORMATION;
typedef struct { DWORD lo, hi; } FILETIME;
typedef struct { DWORD dwFileAttributes; FILETIME a,b,c; DWORD sizeHi, sizeLo, r0, r1; wchar cFileName[MAX_PATH]; wchar cAlt[14]; DWORD x,y; WORD z; } WIN32_FIND_DATAW;
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(void*);
#define K __declspec(dllimport)
K HMODULE WINAPI GetModuleHandleW(LPCWSTR);
K DWORD WINAPI GetModuleFileNameW(HMODULE, LPWSTR, DWORD);
K void* WINAPI VirtualAlloc(void*, SIZE_T, DWORD, DWORD);
K BOOL WINAPI VirtualProtect(void*, SIZE_T, DWORD, DWORD*);
K SIZE_T WINAPI VirtualQuery(const void*, MEMORY_BASIC_INFORMATION*, SIZE_T);
K BOOL WINAPI FlushInstructionCache(HANDLE, const void*, SIZE_T);
K HANDLE WINAPI GetCurrentProcess(void);
K DWORD WINAPI GetCurrentProcessId(void);
K HANDLE WINAPI CreateThread(void*, SIZE_T, LPTHREAD_START_ROUTINE, void*, DWORD, DWORD*);
K BOOL WINAPI CloseHandle(HANDLE);
K void WINAPI InitializeCriticalSection(CRITICAL_SECTION*);
K void WINAPI EnterCriticalSection(CRITICAL_SECTION*);
K void WINAPI LeaveCriticalSection(CRITICAL_SECTION*);
K HANDLE WINAPI CreateEventW(void*, BOOL, BOOL, LPCWSTR);
K BOOL WINAPI SetEvent(HANDLE);
K DWORD WINAPI WaitForSingleObject(HANDLE, DWORD);
K DWORD WINAPI GetTickCount(void);
K HMODULE WINAPI LoadLibraryW(LPCWSTR);
K void* WINAPI GetProcAddress(HMODULE, const char*);
K HANDLE WINAPI CreateFileW(LPCWSTR, DWORD, DWORD, void*, DWORD, DWORD, HANDLE);
K BOOL WINAPI WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);
K BOOL WINAPI FlushFileBuffers(HANDLE);
K void WINAPI Sleep(DWORD);
K HANDLE WINAPI FindFirstFileW(LPCWSTR, WIN32_FIND_DATAW*);
K BOOL WINAPI FindNextFileW(HANDLE, WIN32_FIND_DATAW*);
K BOOL WINAPI FindClose(HANDLE);
K UINT WINAPI GetSystemDirectoryW(LPWSTR, UINT);
K int WINAPI WideCharToMultiByte(UINT, DWORD, LPCWSTR, int, char*, int, const char*, BOOL*);
K int WINAPI MultiByteToWideChar(UINT, DWORD, const char*, int, LPWSTR, int);
K BOOL WINAPI DisableThreadLibraryCalls(HMODULE);
K short WINAPI GetAsyncKeyState(int);
K HWND WINAPI GetForegroundWindow(void);
K DWORD WINAPI GetWindowThreadProcessId(HWND, DWORD*);

/* freestanding helpers */
void* memcpy(void* d, const void* s, SIZE_T n);
void* memset(void* d, int c, SIZE_T n);
static int wlen(const wchar* s){int n=0; while(s[n]) n++; return n;}
static void wcpy(wchar* d, const wchar* s, int max){int i=0; for(;s[i]&&i<max-1;i++) d[i]=s[i]; d[i]=0;}
static void wcat(wchar* d, const wchar* s, int max){int n=wlen(d); wcpy(d+n,s,max-n);}
static int weq(const wchar* a, const wchar* b){while(*a&&*a==*b){a++;b++;} return *a==*b;}
