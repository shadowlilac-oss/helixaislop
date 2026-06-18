#include "helix/front.hpp"

#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "helix/eval.hpp"

namespace helix {
namespace {

// ------------------------------- Lexer -------------------------------------
enum class Tok {
    End, Ident, Int,
    KwFn, KwComptime, KwLet, KwIf, KwElse, KwLoop, KwBreak, KwNext, KwTrue, KwFalse,
    KwIntTy, KwI64, KwI32, KwBool,
    LParen, RParen, LBrace, RBrace, Comma, Semi, Colon, Arrow, Assign,
    Plus, Minus, Star, Slash, Percent,
    Amp, Pipe, Caret, Shl, Shr,
    AndAnd, OrOr, Bang,
    EqEq, Ne, Lt, Le, Gt, Ge,
};

struct Token {
    Tok kind;
    std::string text;
    int64_t ival = 0;
    int line = 1;
};

struct Lexer {
    const std::string& s;
    size_t i = 0;
    int line = 1;
    explicit Lexer(const std::string& src) : s(src) {}

    [[noreturn]] void err(const std::string& m) { throw std::pair<std::string, int>{m, line}; }

    std::vector<Token> lex() {
        std::vector<Token> out;
        for (;;) {
            skip_ws();
            if (i >= s.size()) { out.push_back({Tok::End, "", 0, line}); break; }
            char c = s[i];
            if (isalpha((unsigned char)c) || c == '_') { out.push_back(ident()); continue; }
            if (isdigit((unsigned char)c)) { out.push_back(number()); continue; }
            out.push_back(punct());
        }
        return out;
    }

    void skip_ws() {
        while (i < s.size()) {
            char c = s[i];
            if (c == '\n') { line++; i++; }
            else if (isspace((unsigned char)c)) { i++; }
            else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n') i++;
            } else break;
        }
    }

    Token ident() {
        size_t start = i;
        while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '_')) i++;
        std::string t = s.substr(start, i - start);
        Tok k = Tok::Ident;
        if (t == "fn") k = Tok::KwFn;
        else if (t == "comptime") k = Tok::KwComptime;
        else if (t == "let") k = Tok::KwLet;
        else if (t == "if") k = Tok::KwIf;
        else if (t == "else") k = Tok::KwElse;
        else if (t == "loop") k = Tok::KwLoop;
        else if (t == "break") k = Tok::KwBreak;
        else if (t == "next") k = Tok::KwNext;
        else if (t == "true") k = Tok::KwTrue;
        else if (t == "false") k = Tok::KwFalse;
        else if (t == "int") k = Tok::KwIntTy;
        else if (t == "i64") k = Tok::KwI64;
        else if (t == "i32") k = Tok::KwI32;
        else if (t == "bool") k = Tok::KwBool;
        return {k, t, 0, line};
    }

    Token number() {
        size_t start = i;
        while (i < s.size() && isdigit((unsigned char)s[i])) i++;
        std::string t = s.substr(start, i - start);
        return {Tok::Int, t, std::stoll(t), line};
    }

    Token punct() {
        int ln = line;
        char c = s[i];
        auto two = [&](char a, char b) { return c == a && i + 1 < s.size() && s[i + 1] == b; };
        if (two('-', '>')) { i += 2; return {Tok::Arrow, "->", 0, ln}; }
        if (two('=', '=')) { i += 2; return {Tok::EqEq, "==", 0, ln}; }
        if (two('!', '=')) { i += 2; return {Tok::Ne, "!=", 0, ln}; }
        if (two('<', '=')) { i += 2; return {Tok::Le, "<=", 0, ln}; }
        if (two('>', '=')) { i += 2; return {Tok::Ge, ">=", 0, ln}; }
        if (two('<', '<')) { i += 2; return {Tok::Shl, "<<", 0, ln}; }
        if (two('>', '>')) { i += 2; return {Tok::Shr, ">>", 0, ln}; }
        if (two('&', '&')) { i += 2; return {Tok::AndAnd, "&&", 0, ln}; }
        if (two('|', '|')) { i += 2; return {Tok::OrOr, "||", 0, ln}; }
        i++;
        switch (c) {
            case '(': return {Tok::LParen, "(", 0, ln};
            case ')': return {Tok::RParen, ")", 0, ln};
            case '{': return {Tok::LBrace, "{", 0, ln};
            case '}': return {Tok::RBrace, "}", 0, ln};
            case ',': return {Tok::Comma, ",", 0, ln};
            case ';': return {Tok::Semi, ";", 0, ln};
            case ':': return {Tok::Colon, ":", 0, ln};
            case '=': return {Tok::Assign, "=", 0, ln};
            case '+': return {Tok::Plus, "+", 0, ln};
            case '-': return {Tok::Minus, "-", 0, ln};
            case '*': return {Tok::Star, "*", 0, ln};
            case '/': return {Tok::Slash, "/", 0, ln};
            case '%': return {Tok::Percent, "%", 0, ln};
            case '&': return {Tok::Amp, "&", 0, ln};
            case '|': return {Tok::Pipe, "|", 0, ln};
            case '^': return {Tok::Caret, "^", 0, ln};
            case '!': return {Tok::Bang, "!", 0, ln};
            case '<': return {Tok::Lt, "<", 0, ln};
            case '>': return {Tok::Gt, ">", 0, ln};
        }
        err(std::string("unexpected character '") + c + "'");
    }
};

// ------------------------------- Parser ------------------------------------
struct Step {  // result of parsing a loop body
    NodeId is_break = NONE;
    NodeId break_val = NONE;
    std::vector<NodeId> next_vals;
};

struct FuncHeader {
    bool is_comptime = false;
    std::string name;
    std::vector<std::string> param_names;
    std::vector<Type> param_types;
    Type result_type;
    size_t body_begin = 0;  // token index of '{'
    size_t body_end = 0;    // token index of matching '}'
    NodeId node = NONE;
};

struct Parser {
    World& w;
    std::vector<Token> toks;
    size_t p = 0;
    std::unordered_map<std::string, FuncHeader*> funcs;
    std::vector<std::unordered_map<std::string, NodeId>> scopes;
    Type cur_result_ty = ty_i64();

    explicit Parser(World& world) : w(world) {}

    [[noreturn]] void err(const std::string& m) {
        throw std::pair<std::string, int>{m, toks[p < toks.size() ? p : toks.size() - 1].line};
    }
    const Token& cur() { return toks[p]; }
    bool is(Tok k) { return toks[p].kind == k; }
    bool accept(Tok k) { if (is(k)) { p++; return true; } return false; }
    void expect(Tok k, const char* what) { if (!accept(k)) err(std::string("expected ") + what); }

    Type parse_type() {
        if (accept(Tok::KwIntTy) || accept(Tok::KwI64)) return ty_i64();
        if (accept(Tok::KwI32)) return ty_i32();
        if (accept(Tok::KwBool)) return ty_bool();
        err("expected type");
    }

    void scope_push() { scopes.emplace_back(); }
    void scope_pop() { scopes.pop_back(); }
    void bind(const std::string& n, NodeId v) { scopes.back()[n] = v; }
    NodeId lookup(const std::string& n) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->find(n);
            if (f != it->end()) return f->second;
        }
        return NONE;
    }

    // ---- pass 1: scan headers ----
    void scan_headers(std::vector<FuncHeader>& headers) {
        while (!is(Tok::End)) {
            FuncHeader h;
            if (accept(Tok::KwComptime)) h.is_comptime = true;
            expect(Tok::KwFn, "fn");
            if (!is(Tok::Ident)) err("expected function name");
            h.name = cur().text; p++;
            expect(Tok::LParen, "(");
            if (!is(Tok::RParen)) {
                do {
                    if (!is(Tok::Ident)) err("expected parameter name");
                    h.param_names.push_back(cur().text); p++;
                    expect(Tok::Colon, ":");
                    h.param_types.push_back(parse_type());
                } while (accept(Tok::Comma));
            }
            expect(Tok::RParen, ")");
            expect(Tok::Arrow, "->");
            h.result_type = parse_type();
            if (!is(Tok::LBrace)) err("expected function body");
            h.body_begin = p;
            // brace-match
            int depth = 0; size_t q = p;
            for (;; q++) {
                if (toks[q].kind == Tok::LBrace) depth++;
                else if (toks[q].kind == Tok::RBrace) { depth--; if (depth == 0) break; }
                else if (toks[q].kind == Tok::End) err("unterminated function body");
            }
            h.body_end = q;
            p = q + 1;
            headers.push_back(std::move(h));
        }
    }

    // ---- pass 2: bodies ----
    void parse_body(FuncHeader& h) {
        FuncInfo& fi = w.func_info(h.node);
        cur_result_ty = h.result_type;
        scope_push();
        for (size_t i = 0; i < h.param_names.size(); i++) bind(h.param_names[i], fi.params[i]);
        p = h.body_begin;
        expect(Tok::LBrace, "{");
        NodeId v = parse_expr(0);
        expect(Tok::RBrace, "}");
        w.end_func(h.node, v);
        scope_pop();
    }

    // precedence-climbing
    int lbp(Tok k) {
        switch (k) {
            case Tok::OrOr: return 1;
            case Tok::AndAnd: return 2;
            case Tok::EqEq: case Tok::Ne: return 3;
            case Tok::Lt: case Tok::Le: case Tok::Gt: case Tok::Ge: return 4;
            case Tok::Pipe: return 5;
            case Tok::Caret: return 6;
            case Tok::Amp: return 7;
            case Tok::Shl: case Tok::Shr: return 8;
            case Tok::Plus: case Tok::Minus: return 9;
            case Tok::Star: case Tok::Slash: case Tok::Percent: return 10;
            default: return 0;
        }
    }

    NodeId apply_bin(Tok op, NodeId a, NodeId b) {
        switch (op) {
            case Tok::Plus: return w.add(a, b);
            case Tok::Minus: return w.sub(a, b);
            case Tok::Star: return w.mul(a, b);
            case Tok::Slash: return w.sdiv(a, b);
            case Tok::Percent: return w.srem(a, b);
            case Tok::Amp: case Tok::AndAnd: return w.bit_and(a, b);
            case Tok::Pipe: case Tok::OrOr: return w.bit_or(a, b);
            case Tok::Caret: return w.bit_xor(a, b);
            case Tok::Shl: return w.shl(a, b);
            case Tok::Shr: return w.ashr(a, b);
            case Tok::EqEq: return w.cmp(Op::CmpEq, a, b);
            case Tok::Ne: return w.cmp(Op::CmpNe, a, b);
            case Tok::Lt: return w.cmp(Op::CmpLt, a, b);
            case Tok::Le: return w.cmp(Op::CmpLe, a, b);
            case Tok::Gt: return w.cmp(Op::CmpGt, a, b);
            case Tok::Ge: return w.cmp(Op::CmpGe, a, b);
            default: err("bad operator");
        }
    }

    NodeId parse_expr(int min_bp) {
        NodeId lhs = parse_unary();
        for (;;) {
            int bp = lbp(cur().kind);
            if (bp == 0 || bp < min_bp) break;
            Tok op = cur().kind; p++;
            NodeId rhs = parse_expr(bp + 1);
            lhs = apply_bin(op, lhs, rhs);
        }
        return lhs;
    }

    NodeId parse_unary() {
        if (accept(Tok::Minus)) return w.neg(parse_unary());
        if (accept(Tok::Bang)) return w.cmp(Op::CmpEq, parse_unary(), w.konst_bool(false));
        return parse_primary();
    }

    NodeId parse_primary() {
        const Token& t = cur();
        switch (t.kind) {
            case Tok::Int: { p++; return w.konst(t.ival, cur_result_ty.is_int() ? ty_i64() : ty_i64()); }
            case Tok::KwTrue: p++; return w.konst_bool(true);
            case Tok::KwFalse: p++; return w.konst_bool(false);
            case Tok::LParen: { p++; NodeId v = parse_expr(0); expect(Tok::RParen, ")"); return v; }
            case Tok::KwLet: return parse_let();
            case Tok::KwIf: return parse_if();
            case Tok::KwLoop: return parse_loop();
            case Tok::Ident: return parse_ident();
            default: err("expected expression");
        }
    }

    NodeId parse_let() {
        expect(Tok::KwLet, "let");
        if (!is(Tok::Ident)) err("expected name after let");
        std::string name = cur().text; p++;
        expect(Tok::Assign, "=");
        NodeId val = parse_expr(0);
        expect(Tok::Semi, ";");
        scope_push();
        bind(name, val);
        NodeId body = parse_expr(0);
        scope_pop();
        return body;
    }

    // Coerce a value to a canonical bool (0/1) so Cond predicates are always in
    // {0,1} and truthiness == index everywhere (interp, folder, backend agree).
    NodeId to_bool(NodeId c) {
        if (w.node(c).type.kind == TyKind::Bool) return c;
        return w.cmp(Op::CmpNe, c, w.konst(0, w.node(c).type));
    }

    NodeId parse_if() {
        expect(Tok::KwIf, "if");
        NodeId c = to_bool(parse_expr(0));
        expect(Tok::LBrace, "{");
        NodeId then_v = parse_expr(0);
        expect(Tok::RBrace, "}");
        expect(Tok::KwElse, "else");
        expect(Tok::LBrace, "{");
        NodeId else_v = parse_expr(0);
        expect(Tok::RBrace, "}");
        // yields[0] = predicate-false = else, yields[1] = predicate-true = then
        return w.make_cond(c, w.node(then_v).type, {else_v, then_v});
    }

    NodeId parse_ident() {
        std::string name = cur().text; p++;
        if (is(Tok::LParen)) return parse_call(name);
        NodeId v = lookup(name);
        if (v == NONE) err("unknown identifier '" + name + "'");
        return v;
    }

    NodeId parse_call(const std::string& name) {
        expect(Tok::LParen, "(");
        std::vector<NodeId> args;
        if (!is(Tok::RParen)) {
            do { args.push_back(parse_expr(0)); } while (accept(Tok::Comma));
        }
        expect(Tok::RParen, ")");
        auto it = funcs.find(name);
        if (it == funcs.end()) err("call to unknown function '" + name + "'");
        NodeId target = it->second->node;
        if (args.size() != it->second->param_types.size())
            err("call to '" + name + "' expects " +
                std::to_string(it->second->param_types.size()) + " argument(s), got " +
                std::to_string(args.size()));
        // comptime evaluation as graph reduction: fold a comptime call on constant args.
        // Only when the callee body is finalized (result != NONE) — never fold against
        // an unparsed forward-declared body (which would silently yield 0).
        if (it->second->is_comptime && w.func_info(target).result != NONE) {
            bool all_const = true;
            std::vector<int64_t> cargs;
            for (NodeId a : args) {
                auto c = w.as_const(a);
                if (!c) { all_const = false; break; }
                cargs.push_back(*c);
            }
            if (all_const) {
                auto r = eval_func(w, target, cargs);
                if (r.ok) return w.konst(r.value, w.func_info(target).result_type);
                err("comptime evaluation of '" + name + "' exceeded fuel");
            }
        }
        return w.call(target, std::move(args));
    }

    NodeId parse_loop() {
        expect(Tok::KwLoop, "loop");
        expect(Tok::LParen, "(");
        std::vector<std::string> names;
        std::vector<NodeId> inits;
        do {
            if (!is(Tok::Ident)) err("expected loop variable name");
            names.push_back(cur().text); p++;
            expect(Tok::Assign, "=");
            inits.push_back(parse_expr(0));
        } while (accept(Tok::Comma));
        expect(Tok::RParen, ")");
        // carried params
        std::vector<NodeId> params;
        scope_push();
        for (size_t i = 0; i < names.size(); i++) {
            NodeId pn = w.param(w.node(inits[i]).type, (int)i, names[i]);
            params.push_back(pn);
            bind(names[i], pn);
        }
        expect(Tok::LBrace, "{");
        Step st = parse_step(params);
        expect(Tok::RBrace, "}");
        scope_pop();
        NodeId lp = w.make_loop(inits, w.node(st.break_val).type, params,
                                st.is_break, st.break_val, st.next_vals);
        return lp;
    }

    Step parse_step(const std::vector<NodeId>& carried) {
        if (accept(Tok::KwBreak)) {
            Step s;
            s.is_break = w.konst_bool(true);
            s.break_val = parse_expr(0);
            s.next_vals = carried;  // unused on break; keep arity/types valid
            return s;
        }
        if (accept(Tok::KwNext)) {
            Step s;
            s.is_break = w.konst_bool(false);
            do { s.next_vals.push_back(parse_expr(0)); } while (accept(Tok::Comma));
            if (s.next_vals.size() != carried.size()) err("next arity mismatch");
            s.break_val = w.konst(0, ty_i64());  // unused on continue
            return s;
        }
        if (accept(Tok::KwLet)) {
            if (!is(Tok::Ident)) err("expected name after let");
            std::string name = cur().text; p++;
            expect(Tok::Assign, "=");
            NodeId val = parse_expr(0);
            expect(Tok::Semi, ";");
            scope_push();
            bind(name, val);
            Step s = parse_step(carried);
            scope_pop();
            return s;
        }
        if (accept(Tok::KwIf)) {
            NodeId c = to_bool(parse_expr(0));
            expect(Tok::LBrace, "{");
            Step then_s = parse_step(carried);
            expect(Tok::RBrace, "}");
            expect(Tok::KwElse, "else");
            expect(Tok::LBrace, "{");
            Step else_s = parse_step(carried);
            expect(Tok::RBrace, "}");
            Step s;
            s.is_break = w.make_cond(c, ty_bool(), {else_s.is_break, then_s.is_break});
            s.break_val = w.make_cond(c, w.node(then_s.break_val).type,
                                      {else_s.break_val, then_s.break_val});
            for (size_t k = 0; k < carried.size(); k++)
                s.next_vals.push_back(w.make_cond(c, w.node(then_s.next_vals[k]).type,
                                                  {else_s.next_vals[k], then_s.next_vals[k]}));
            return s;
        }
        err("loop body must be break / next / if / let");
    }

    void run(const std::string& src) {
        Lexer lx(src);
        toks = lx.lex();
        std::vector<FuncHeader> headers;
        scan_headers(headers);
        // create func nodes
        for (auto& h : headers) {
            h.node = w.begin_func(h.name, h.param_types, h.result_type, false, h.is_comptime);
            w.add_func(h.node);
        }
        for (auto& h : headers) funcs[h.name] = &h;
        // Parse comptime function bodies first so their results are finalized before
        // any (constant-arg) comptime call site is folded.
        for (auto& h : headers) if (h.is_comptime) parse_body(h);
        for (auto& h : headers) if (!h.is_comptime) parse_body(h);
    }
};

}  // namespace

ParseStatus parse_module(World& w, const std::string& src) {
    Parser ps(w);
    try {
        ps.run(src);
    } catch (const std::pair<std::string, int>& e) {
        return {false, e.first, e.second};
    } catch (const std::exception& e) {
        return {false, e.what(), 0};
    }
    return {true, "", 0};
}

}  // namespace helix
