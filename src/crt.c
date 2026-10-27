#include "win.h"
int _fltused = 0;


void* memcpy(void* d, const void* s, SIZE_T n){ volatile BYTE* D=d; const BYTE* S=s; while(n--) *D++=*S++; return d; }
void* memset(void* d, int c, SIZE_T n){ volatile BYTE* D=d; while(n--) *D++=(BYTE)c; return d; }
