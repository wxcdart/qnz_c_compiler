struct P{int x,y;};
struct P mk(int a,int b){ struct P p; p.x=a; p.y=b; return p; }
int main(void){ struct P q=mk(20,3); struct P *r=&q; return r->x - q.y; } /* 17 */
