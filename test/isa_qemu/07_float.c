#include <stdio.h>
int main(void){
  double a=3.5, b=2.0;
  printf("fadd=%.1f fsub=%.1f fmul=%.1f fdiv=%.2f\n",a+b,a-b,a*b,a/b);
  int i=(int)(a*2); printf("cvti=%d\n",i);
  double d=1.0/3.0; printf("d=%.6f\n",d);
  float f=2.5f; printf("f=%.1f\n",(double)f);
  return 0;
}
