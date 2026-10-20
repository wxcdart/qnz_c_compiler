#include <stdio.h>
int gc=100; double gd=2.5;
int inc(int x){ gc+=x; return gc; }
int main(void){
  gd += 1.5;
  printf("gc=%d gd=%.1f\n",gc,gd);
  int r=inc(5);
  printf("after inc gc=%d r=%d\n",gc,r);
  return 0;
}
