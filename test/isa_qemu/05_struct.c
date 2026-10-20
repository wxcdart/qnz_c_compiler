#include <stdio.h>
struct P{int x,y;};
struct P mk(int x,int y){ struct P p; p.x=x; p.y=y; return p; }
int main(void){
  struct P a=mk(3,4);
  struct P *q=&a;
  printf("x=%d y=%d q->x=%d\n",a.x,a.y,q->x);
  int sz; sz=sizeof(struct P); printf("sizeofP=%d\n",sz);
  return 0;
}
