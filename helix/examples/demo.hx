// Helix sample program. Build the compiler, then:
//   helixc examples/demo.hx --print
//   helixc examples/demo.hx --run fib 30
//   helixc examples/demo.hx --run main

fn fib(n: int) -> int {
    if n < 2 { n } else { fib(n - 1) + fib(n - 2) }
}

fn gcd(a: int, b: int) -> int {
    loop (x = a, y = b) {
        if y == 0 { break x } else { next y, x % y }
    }
}

fn sum_to(n: int) -> int {
    loop (acc = 0, i = 0) {
        if i > n { break acc } else { next acc + i, i + 1 }
    }
}

fn collatz(n: int) -> int {
    loop (x = n, steps = 0) {
        if x == 1 { break steps }
        else { if x % 2 == 0 { next x / 2, steps + 1 } else { next 3 * x + 1, steps + 1 } }
    }
}

// `comptime` runs at compile time via the SAME reduction engine as the optimizer:
// the call below folds to a constant in the graph before any code is generated.
comptime fn fact(n: int) -> int {
    loop (acc = 1, i = 1) { if i > n { break acc } else { next acc * i, i + 1 } }
}

fn main() -> int {
    // fact(10) is evaluated at compile time -> 3628800
    fact(10) + fib(15) + gcd(48, 36) + sum_to(100)
}
