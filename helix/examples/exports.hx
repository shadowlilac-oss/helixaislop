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

// Read-only array processing: a[i] is a pure i64 load at a + i*8.
fn sum_arr(a: ptr, n: int) -> int {
    loop (acc = 0, i = 0) { if i >= n { break acc } else { next acc + a[i], i + 1 } }
}

fn dot(a: ptr, b: ptr, n: int) -> int {
    loop (acc = 0, i = 0) { if i >= n { break acc } else { next acc + a[i] * b[i], i + 1 } }
}

// Imperative array reduction with mutable variables + a while loop (read-only).
fn asum_imp(a: ptr, n: int) -> int {
    var s = 0; var i = 0;
    while i < n { s = s + a[i]; i = i + 1; }
    return s;
}
