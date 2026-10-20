#include <stdio.h>
int add(int a,int b){return a+b;}
int main(void){
  int (*fp)(int,int)=add;
  int r=fp(5,6);
  printf("add=%d fp=%d\n",add(3,4),r);
  return 0;
}
