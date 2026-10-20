int fact(int n){ return n<=1?1:n*fact(n-1); }
int main(void){ int s=0; for(int i=1;i<=10;i++) s+=i; return s + fact(4); } /* 55+24=79 */
