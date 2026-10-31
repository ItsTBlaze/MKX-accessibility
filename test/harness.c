#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define K
#define __stdcall
#define __declspec(x)
#define memcpy memcpy_
#define memset memset_
#include "../src/mkxaccess.c"
#undef memcpy
static unsigned g_clock;
void* memcpy_(void* d,const void* s,SIZE_T n){return __builtin_memcpy(d,s,n);}
void* memset_(void* d,int c,SIZE_T n){return __builtin_memset(d,c,n);}
DWORD WINAPI GetTickCount(void){return g_clock;}
BOOL WINAPI WriteFile(HANDLE h,const void* b,DWORD n,DWORD* w,void* o){ if(strstr(b,"SPEAK")) fwrite(b,1,n,stdout); *w=n; return 1;}
int WINAPI WideCharToMultiByte(UINT cp,DWORD f,LPCWSTR s,int n,char* o,int m,const char*a,BOOL*b){int i=0;for(;s[i]&&i<m-4;i++){unsigned c=s[i]; if(c<0x80)o[i]=c; else o[i]='?';} o[i]=0; return i+1;}
int WINAPI MultiByteToWideChar(UINT a,DWORD b,const char* s,int n,LPWSTR o,int m){int i=0;for(;s[i]&&i<m-1;i++)o[i]=(unsigned char)s[i];o[i]=0;return i+1;}
void WINAPI EnterCriticalSection(CRITICAL_SECTION*c){} void WINAPI LeaveCriticalSection(CRITICAL_SECTION*c){}
SIZE_T WINAPI VirtualQuery(const void*p,MEMORY_BASIC_INFORMATION*m,SIZE_T n){return 0;}
#define S(f) f
HMODULE WINAPI GetModuleHandleW(LPCWSTR a){return 0;} DWORD WINAPI GetModuleFileNameW(HMODULE a,LPWSTR b,DWORD c){return 0;}
void* WINAPI VirtualAlloc(void*a,SIZE_T b,DWORD c,DWORD d){return calloc(1,b);} BOOL WINAPI VirtualProtect(void*a,SIZE_T b,DWORD c,DWORD*d){return 0;}
BOOL WINAPI FlushInstructionCache(HANDLE a,const void*b,SIZE_T c){return 0;} HANDLE WINAPI GetCurrentProcess(void){return 0;} DWORD WINAPI GetCurrentProcessId(void){return 0;}
HANDLE WINAPI CreateThread(void*a,SIZE_T b,LPTHREAD_START_ROUTINE c,void*d,DWORD e,DWORD*f){return 0;} BOOL WINAPI CloseHandle(HANDLE a){return 0;}
void WINAPI InitializeCriticalSection(CRITICAL_SECTION*c){} HANDLE WINAPI CreateEventW(void*a,BOOL b,BOOL c,LPCWSTR d){return 0;} BOOL WINAPI SetEvent(HANDLE a){return 0;}
DWORD WINAPI WaitForSingleObject(HANDLE a,DWORD b){return 0;} HMODULE WINAPI LoadLibraryW(LPCWSTR a){return 0;} void* WINAPI GetProcAddress(HMODULE a,const char*b){return 0;}
HANDLE WINAPI CreateFileW(LPCWSTR a,DWORD b,DWORD c,void*d,DWORD e,DWORD f,HANDLE g){return 0;} BOOL WINAPI FlushFileBuffers(HANDLE a){return 0;} void WINAPI Sleep(DWORD a){}
HANDLE WINAPI FindFirstFileW(LPCWSTR a,WIN32_FIND_DATAW*b){return 0;} BOOL WINAPI FindNextFileW(HANDLE a,WIN32_FIND_DATAW*b){return 0;} BOOL WINAPI FindClose(HANDLE a){return 0;}
UINT WINAPI GetSystemDirectoryW(LPWSTR a,UINT b){return 0;} BOOL WINAPI DisableThreadLibraryCalls(HMODULE a){return 0;}
short WINAPI GetAsyncKeyState(int a){return 0;} HWND WINAPI GetForegroundWindow(void){return 0;} DWORD WINAPI GetWindowThreadProcessId(HWND a,DWORD*b){return 0;}
static void tick(void){ speech_tick(g_clock); }
int main(int argc,char**argv){
  g_ent=calloc(NENT,sizeof(Entry)); g_log=(HANDLE)1;
  FILE* f=fopen(argv[1],"r"); char line[8192]; static Event ev;
  while(fgets(line,sizeof line,f)){
    unsigned t; char type[16]; if(sscanf(line,"%u %15s",&t,type)!=2) continue;
    int et=0; if(!strcmp(type,"TEXT"))et=EV_TEXT; else if(!strcmp(type,"HTML"))et=EV_HTML; else if(!strcmp(type,"FOCUS"))et=EV_FOCUS; else if(!strcmp(type,"GRID"))et=EV_GRID; else if(!strcmp(type,"ANIMFOCUS"))et=EV_ANIM; else if(!strcmp(type,"COMBO-END"))et=EV_COMBOEND; else continue;
    while(g_clock+25<=t){g_clock+=25;tick();}
    g_clock=t;
    memset(&ev,0,sizeof ev); ev.type=et; ev.t=t;
    char* p=strchr(line,'['); char* q=p?strstr(p,"] "):0; if(!q) q=p?strchr(p,']'):0; if(!p||!q) continue;
    *q=0; char* path=p+1; char* txt=q+1; if(*txt==' ')txt++; txt[strcspn(txt,"\r\n")]=0;
    if(strcmp(path,"?")) for(int i=0;path[i]&&i<PATHMAX-1;i++) ev.path[i]=(unsigned char)path[i];
    if(et<=EV_HTML) for(int i=0;txt[i]&&i<TEXTMAX-1;i++) ev.text[i]=(unsigned char)txt[i];
    if(et==EV_GRID){ sscanf(txt,"x=%d y=%d",&ev.x,&ev.y); }
    if(et<=EV_VHTML) handle_text(&ev); else if(et==EV_COMBOEND) combo_finished(&ev); else handle_focus(&ev);
  }
  for(int i=0;i<40;i++){g_clock+=25;tick();}
}
