#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
int main(void) {
 DeleteFileA("DamageTrace.log");
 HMODULE m=LoadLibraryW(L"DamageTrace.asi");
 if(!m){printf("LoadLibrary failed: %lu\n",GetLastError());return 1;}
 for(unsigned n=0;n<100;n++){
  Sleep(100);FILE *f=fopen("DamageTrace.log","rb");if(!f)continue;
  char text[4096]={0};fread(text,1,sizeof(text)-1,f);fclose(f);
  if(strstr(text,"UNSUPPORTED:") && !strstr(text,"ACTIVE:")){
   puts("PASS: actual ASI loads in a host and refuses an unsupported EXE before installing hooks");
   return 0;
  }
 }
 puts("FAIL: unsupported-host refusal was not observed");return 1;
}
