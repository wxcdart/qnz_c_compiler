#include <stdio.h>
int main(void){
  signed char sc=-5; signed short ss=-300; signed int si=-12345;
  printf("sext sc=%d ss=%d si=%d\n",(int)sc,(int)ss,(int)si);
  unsigned char uc=200; unsigned short us=70000;
  printf("zext uc=%u us=%u\n",(unsigned)uc,(unsigned)us);
  int x=100; long y=(long)x; unsigned z=(unsigned)x;
  printf("conv y=%ld z=%u\n",y,z);
  printf("intmax neg=%d\n", -2147483647-1);
  return 0;
}
