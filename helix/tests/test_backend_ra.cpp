// Tests for the optimizing (register-allocating) backend jit_compile_ra,
// validated against the interpreter oracle. Same gauntlet as the simple backend
// plus register-pressure / spilling stress and a randomized fuzzer.
#include <climits>
#include <functional>
#include <vector>

#include "helix/backend.hpp"
#include "helix/eval.hpp"
#include "helix/front.hpp"
#include "helix/ir.hpp"
#include "test.hpp"

using namespace helix;

namespace {
struct C { World w; JitModule jit; };
static void load(C& c, const char* src) {
    auto st = parse_module(c.w, src);
    if (!st.ok) std::printf("    parse error (line %d): %s\n", st.line, st.msg.c_str());
    CHECK(st.ok);
    c.jit = jit_compile_ra(c.w);
    if (!c.jit.ok) std::printf("    jit_ra error: %s\n", c.jit.err.c_str());
    CHECK(c.jit.ok);
}
static int64_t diff(C& c, const char* fn, std::vector<int64_t> a) {
    NodeId f = c.w.find_func(fn);
    auto ir = eval_func(c.w, f, a);
    int64_t jv = c.jit.call(f, a);
    CHECK(ir.ok);
    CHECK_EQ(jv, ir.value);
    return jv;
}

struct Rng { uint64_t s; explicit Rng(uint64_t x):s(x){} uint64_t nx(){ s=s*6364136223846793005ull+1442695040888963407ull; return s>>33; } };
}  // namespace

TEST("jit_ra: recursion, loops, mutual recursion all correct on real x64") {
    C c;
    load(c,
        "fn fib(n: int) -> int { if n < 2 { n } else { fib(n-1) + fib(n-2) } }\n"
        "fn sum(n: int) -> int { loop (acc=0,i=0){ if i>=n {break acc} else {next acc+i,i+1} } }\n"
        "fn fact(n: int) -> int { loop (acc=1,i=1){ if i>n {break acc} else {next acc*i,i+1} } }\n"
        "fn gcd(a: int, b: int) -> int { loop (x=a,y=b){ if y==0 {break x} else {next y, x%y} } }\n"
        "fn is_even(n:int)->bool { if n==0 {true} else {is_odd(n-1)} }\n"
        "fn is_odd(n:int)->bool { if n==0 {false} else {is_even(n-1)} }\n"
        "fn ack(m:int,n:int)->int { if m==0 {n+1} else { if n==0 {ack(m-1,1)} else {ack(m-1,ack(m,n-1))} } }\n"
        "fn collatz(n:int)->int { loop (x=n,s=0){ if x==1 {break s} else { if x%2==0 {next x/2,s+1} else {next 3*x+1,s+1} } } }\n");
    int64_t fibs[]={0,1,1,2,3,5,8,13,21,34,55};
    for (int n=0;n<=10;n++) CHECK_EQ(diff(c,"fib",{n}), fibs[n]);
    CHECK_EQ(diff(c,"fib",{25}), 75025);
    for (int n=0;n<=50;n++) CHECK_EQ(diff(c,"sum",{n}), (int64_t)n*(n-1)/2);
    CHECK_EQ(diff(c,"fact",{10}), 3628800);
    CHECK_EQ(diff(c,"gcd",{1071,462}), 21);
    for (int n=0;n<=12;n++){ CHECK_EQ(diff(c,"is_even",{n}), n%2==0); CHECK_EQ(diff(c,"is_odd",{n}), n%2==1); }
    CHECK_EQ(diff(c,"ack",{3,3}), 61);
    CHECK_EQ(diff(c,"collatz",{27}), 111);
}

TEST("jit_ra: register pressure forces spilling and stays correct") {
    C c;
    load(c,
        "fn spill(a: int, b: int, c: int, d: int) -> int {\n"
        "  let t0=a*a+1; let t1=b*b+2; let t2=c*c+3; let t3=d*d+4;\n"
        "  let t4=a*b+5; let t5=b*c+6; let t6=c*d+7; let t7=a*d+8;\n"
        "  let t8=a+b+9; let t9=c+d+10; let t10=a*c+11; let t11=b*d+12;\n"
        "  t0+t1+t2+t3+t4+t5+t6+t7+t8+t9+t10+t11\n"
        "}\n");
    auto ref=[&](int64_t a,int64_t b,int64_t cc,int64_t d){
        return a*a+1 + b*b+2 + cc*cc+3 + d*d+4 + a*b+5 + b*cc+6 + cc*d+7 + a*d+8 + a+b+9 + cc+d+10 + a*cc+11 + b*d+12;
    };
    Rng r(1234);
    for (int i=0;i<40;i++){
        int64_t a=(int64_t)(r.nx()%2000)-1000, b=(int64_t)(r.nx()%2000)-1000;
        int64_t cc=(int64_t)(r.nx()%2000)-1000, d=(int64_t)(r.nx()%2000)-1000;
        CHECK_EQ(diff(c,"spill",{a,b,cc,d}), ref(a,b,cc,d));
    }
}

TEST("jit_ra: div/rem guards and i32 width match the interpreter") {
    C c;
    load(c, "fn dv(a:int,b:int)->int{a/b}\nfn rv(a:int,b:int)->int{a%b}\n"
            "fn a32(a:i32,b:i32)->i32{a+b}\nfn m32(a:i32,b:i32)->i32{a*b}\n");
    CHECK_EQ(diff(c,"dv",{123,0}), 0);
    CHECK_EQ(diff(c,"rv",{123,0}), 0);
    CHECK_EQ(diff(c,"dv",{INT64_MIN,-1}), INT64_MIN);
    CHECK_EQ(diff(c,"rv",{INT64_MIN,-1}), 0);
    CHECK_EQ(diff(c,"dv",{-100,7}), -100/7);
    CHECK_EQ(diff(c,"a32",{2000000000,2000000000}), (int64_t)(int32_t)4000000000u);
    CHECK_EQ(diff(c,"m32",{100000,100000}), (int64_t)(int32_t)(100000u*100000u));
}

TEST("jit_ra: fuzz random expression graphs (jit_ra == interp == reference)") {
    World w;
    Rng rng(0xABCDEF1234567ull);
    std::vector<NodeId> funcs;
    std::vector<std::function<int64_t(const int64_t*)>> refs;
    std::function<std::pair<NodeId,std::function<int64_t(const int64_t*)>>(int, std::vector<NodeId>&)> gen;
    gen = [&](int depth, std::vector<NodeId>& ps) -> std::pair<NodeId,std::function<int64_t(const int64_t*)>> {
        if (depth<=0 || rng.nx()%4==0) {
            if (rng.nx()%2==0) { int idx=(int)(rng.nx()%ps.size()); NodeId p=ps[idx]; return {p,[idx](const int64_t*a){return a[idx];}}; }
            int64_t k=(int64_t)(rng.nx()%201)-100; return {w.konst(k,ty_i64()),[k](const int64_t*){return k;}};
        }
        int op=(int)(rng.nx()%7);
        auto x=gen(depth-1,ps), y=gen(depth-1,ps);
        auto fx=x.second, fy=y.second;
        switch(op){
            case 0: return {w.add(x.first,y.first),[fx,fy](const int64_t*a){return (int64_t)((uint64_t)fx(a)+(uint64_t)fy(a));}};
            case 1: return {w.sub(x.first,y.first),[fx,fy](const int64_t*a){return (int64_t)((uint64_t)fx(a)-(uint64_t)fy(a));}};
            case 2: return {w.mul(x.first,y.first),[fx,fy](const int64_t*a){return (int64_t)((uint64_t)fx(a)*(uint64_t)fy(a));}};
            case 3: return {w.bit_and(x.first,y.first),[fx,fy](const int64_t*a){return fx(a)&fy(a);}};
            case 4: return {w.bit_or(x.first,y.first),[fx,fy](const int64_t*a){return fx(a)|fy(a);}};
            case 5: return {w.bit_xor(x.first,y.first),[fx,fy](const int64_t*a){return fx(a)^fy(a);}};
            default: { auto t=gen(depth-1,ps),e=gen(depth-1,ps); auto ft=t.second,fe=e.second;
                NodeId cnd=w.cmp(Op::CmpLt,x.first,y.first);
                return {w.select(cnd,t.first,e.first),[fx,fy,ft,fe](const int64_t*a){return fx(a)<fy(a)?ft(a):fe(a);}}; }
        }
    };
    for (int i=0;i<150;i++){
        NodeId f=w.begin_func("fz"+std::to_string(i),{ty_i64(),ty_i64(),ty_i64()},ty_i64());
        std::vector<NodeId> ps=w.func_info(f).params;
        auto e=gen(4,ps);
        w.end_func(f,e.first); w.add_func(f);
        funcs.push_back(f); refs.push_back(e.second);
    }
    JitModule jit=jit_compile_ra(w);
    if(!jit.ok) std::printf("    jit_ra error: %s\n", jit.err.c_str());
    CHECK(jit.ok);
    for (int i=0;i<150;i++)
        for (int t=0;t<12;t++){
            int64_t a[3]; for(int k=0;k<3;k++) a[k]=(int64_t)(rng.nx()%200000)-100000;
            std::vector<int64_t> args={a[0],a[1],a[2]};
            int64_t expect=refs[i](a);
            auto ir=eval_func(w,funcs[i],args);
            int64_t jv=jit.call(funcs[i],args);
            CHECK(ir.ok); CHECK_EQ(ir.value,expect); CHECK_EQ(jv,expect);
        }
}
