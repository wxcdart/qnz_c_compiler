#include <stdio.h>
int mylen(const char *s){ int n=0; while(s[n]) n++; return n; }
int main(void){
  const char *s="hello world";
  printf("len=%d first=%c\n",mylen(s),s[0]);
  char buf[8]; int n=0;
  while(s[n] && n<7){ buf[n]=s[n]; n++; } buf[n]=0;
  printf("copy=%s\n",buf);
  return 0;
}
