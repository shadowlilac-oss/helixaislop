// Functions exported to native code via `helixc --emit-obj`. (No `main` here so
// it doesn't clash with the C driver's main at link time.)
fn fib(n: int) -> int {
    if n < 2 { n } else { fib(n - 1) + fib(n - 2) }
}

fn gcd(a: int, b: int) -> int {
    loop (x = a, y = b) { if y == 0 { break x } else { next y, x % y } }
}

fn sum_to(n: int) -> int {
    loop (acc = 0, i = 0) { if i > n { break acc } else { next acc + i, i + 1 } }
}
