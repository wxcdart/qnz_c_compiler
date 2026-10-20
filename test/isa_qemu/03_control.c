#include <stdio.h>
int fact(int n){ return n<=1?1:n*fact(n-1); }
int fib(int n){ return n<2?n:fib(n-1)+fib(n-2); }
int main(void){
  int s=0; for(int i=1;i<=10;i++){ if(i%2==0) s+=i; }
  printf("sum_even=%d\n",s);
  printf("fact5=%d fib10=%d\n",fact(5),fib(10));
  for(int i=0;i<5;i++){ switch(i%3){case 0:printf("a");break;case 1:printf("b");break;default:printf("c");} }
  printf("\n");
  int v=0; while(v<4){v++;}
  do{v++;}while(v<5);
  printf("v=%d\n",v);
  return 0;
}
