/* Native C driver that calls Helix-compiled functions from helix.obj. */
#include <stdio.h>

long long fib(long long);
long long gcd(long long, long long);
long long sum_to(long long);

int main(void) {
    printf("fib(30)    = %lld\n", fib(30));
    printf("gcd(1071,462) = %lld\n", gcd(1071, 462));
    printf("sum_to(100)  = %lld\n", sum_to(100));
    return (int)(fib(10) + gcd(48, 36));  /* 55 + 12 = 67 */
}
