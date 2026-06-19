/* Native C driver that calls Helix-compiled functions from helix.obj. */
#include <stdio.h>

long long fib(long long);
long long gcd(long long, long long);
long long sum_to(long long);
long long sum_arr(long long* a, long long n);
long long dot(long long* a, long long* b, long long n);
long long bubble(long long* a, long long n);

int main(void) {
    long long xs[6] = {10, 20, 30, 40, 50, 60};
    long long ys[6] = {1, 2, 3, 4, 5, 6};
    long long data[8] = {5, 3, 9, 1, 7, 2, 8, 4};
    int i;
    printf("fib(30)       = %lld\n", fib(30));
    printf("gcd(1071,462) = %lld\n", gcd(1071, 462));
    printf("sum_to(100)   = %lld\n", sum_to(100));
    printf("sum_arr(xs,6) = %lld\n", sum_arr(xs, 6));   /* 210 */
    printf("dot(xs,ys,6)  = %lld\n", dot(xs, ys, 6));   /* 910 */
    bubble(data, 8);                                    /* sort in place, natively */
    printf("bubble sorted = ");
    for (i = 0; i < 8; i++) printf("%lld ", data[i]);
    printf("\n");
    return (int)(fib(10) + gcd(48, 36));                /* 55 + 12 = 67 */
}
