#include <stdio.h>
int main(void){
  int a=7,b=3;
  printf("add=%d sub=%d mul=%d div=%d mod=%d\n",a+b,a-b,a*b,a/b,a%b);
  printf("shift=%d and=%d or=%d xor=%d not=%d\n",(a<<2),(a&b),(a|b),(a^b),~a);
  printf("neg=-%d abs=%d\n",-a, (a<0?-a:a));
  long n=-12345;
  printf("long=%ld lneg=%ld\n",n,-n);
  return 0;
}
