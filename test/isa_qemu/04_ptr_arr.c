#include <stdio.h>
int main(void){
  int a[5]={10,20,30,40,50};
  int *p=a;
  printf("a[0]=%d a[2]=%d *p=%d p[3]=%d\n",a[0],a[2],*p,p[3]);
  *p=99; printf("after a[0]=%d\n",a[0]);
  int sum=0; for(int i=0;i<5;i++) sum+=a[i];
  printf("sum=%d\n",sum);
  int b[3][2]={{1,2},{3,4},{5,6}};
  printf("b[1][1]=%d b[2][0]=%d\n",b[1][1],b[2][0]);
  return 0;
}
