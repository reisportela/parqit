#include "engine/exprtrans.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "engine/session.hpp" /* quote_ident / quote_literal / dtoa */
#include "engine/typemap.hpp" /* kStataMissThreshold */

namespace parqit {

namespace {

/* ------------------------------------------------------------- lexer --- */

enum class Tok {
    End, Num, MissingDot, String, Ident, SysN, SysBigN,
    LParen, RParen, Comma,
    Or, And, Not,
    Eq, Ne, Lt, Gt, Le, Ge,
    Plus, Minus, Star, Slash, Caret,
};

struct Token {
    Tok t = Tok::End;
    std::string text;   /* ident name / string payload / number literal */
    size_t pos = 0;
};

struct Lexer {
    const std::string &s;
    size_t i = 0;
    std::string error;

    explicit Lexer(const std::string &src) : s(src) {}

    void skip_ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    }

    bool ident_start(unsigned char c) {
        return std::isalpha(c) || c == '_' || c >= 0x80;
    }
    bool ident_char(unsigned char c) {
        return std::isalnum(c) || c == '_' || c >= 0x80;
    }

    Token next() {
        skip_ws();
        Token tok;
        tok.pos = i;
        if (i >= s.size()) return tok;
        char c = s[i];

        /* numbers: 12, 12.5, .5, 1e-7 ; missing literals: . .a .. .z */
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < s.size() &&
             std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            size_t j = i;
            while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) ||
                                    s[j] == '.'))
                j++;
            if (j < s.size() && (s[j] == 'e' || s[j] == 'E')) {
                size_t k = j + 1;
                if (k < s.size() && (s[k] == '+' || s[k] == '-')) k++;
                if (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) {
                    j = k;
                    while (j < s.size() &&
                           std::isdigit(static_cast<unsigned char>(s[j])))
                        j++;
                }
            }
            tok.t = Tok::Num;
            tok.text = s.substr(i, j - i);
            i = j;
            return tok;
        }
        if (c == '.') {
            /* . or .a-.z as a missing literal (when not part of a number) */
            if (i + 1 < s.size() && std::isalpha(static_cast<unsigned char>(s[i + 1]))) {
                if (i + 2 < s.size() && ident_char(s[i + 2])) {
                    error = "invalid missing-value literal";
                    return tok;
                }
                tok.t = Tok::MissingDot;
                tok.text = s.substr(i, 2);
                i += 2;
                return tok;
            }
            tok.t = Tok::MissingDot;
            tok.text = ".";
            i += 1;
            return tok;
        }
        if (c == '"') {
            size_t j = i + 1;
            std::string out;
            while (j < s.size() && s[j] != '"') out.push_back(s[j++]);
            if (j >= s.size()) {
                error = "unterminated string literal";
                return tok;
            }
            tok.t = Tok::String;
            tok.text = out;
            i = j + 1;
            return tok;
        }
        if (c == '`' && i + 1 < s.size() && s[i + 1] == '"') {
            /* compound string `"..."' (no nesting) */
            size_t j = i + 2;
            std::string out;
            while (j + 1 < s.size() && !(s[j] == '"' && s[j + 1] == '\'')) {
                out.push_back(s[j++]);
            }
            if (j + 1 >= s.size()) {
                error = "unterminated compound string literal";
                return tok;
            }
            tok.t = Tok::String;
            tok.text = out;
            i = j + 2;
            return tok;
        }
        if (ident_start(static_cast<unsigned char>(c))) {
            size_t j = i;
            while (j < s.size() && ident_char(static_cast<unsigned char>(s[j]))) j++;
            tok.text = s.substr(i, j - i);
            i = j;
            if (tok.text == "_n") tok.t = Tok::SysN;
            else if (tok.text == "_N") tok.t = Tok::SysBigN;
            else tok.t = Tok::Ident;
            return tok;
        }
        auto two = [&](char a, char b) {
            return c == a && i + 1 < s.size() && s[i + 1] == b;
        };
        if (two('=', '=')) { tok.t = Tok::Eq; i += 2; return tok; }
        if (two('!', '=') || two('~', '=')) { tok.t = Tok::Ne; i += 2; return tok; }
        if (two('>', '=')) { tok.t = Tok::Ge; i += 2; return tok; }
        if (two('<', '=')) { tok.t = Tok::Le; i += 2; return tok; }
        switch (c) {
        case '|': tok.t = Tok::Or; i++; if (i < s.size() && s[i] == '|') i++; return tok;
        case '&': tok.t = Tok::And; i++; if (i < s.size() && s[i] == '&') i++; return tok;
        case '!': case '~': tok.t = Tok::Not; i++; return tok;
        case '<': tok.t = Tok::Lt; i++; return tok;
        case '>': tok.t = Tok::Gt; i++; return tok;
        case '(': tok.t = Tok::LParen; i++; return tok;
        case ')': tok.t = Tok::RParen; i++; return tok;
        case ',': tok.t = Tok::Comma; i++; return tok;
        case '+': tok.t = Tok::Plus; i++; return tok;
        case '-': tok.t = Tok::Minus; i++; return tok;
        case '*': tok.t = Tok::Star; i++; return tok;
        case '/': tok.t = Tok::Slash; i++; return tok;
        case '^': tok.t = Tok::Caret; i++; return tok;
        case '=':
            error = "single = (did you mean == ?)";
            return tok;
        default:
            error = std::string("unexpected character '") + c + "'";
            return tok;
        }
    }
};

/* ------------------------------------------------------- date literals --- */

const char *kMonths[12] = {"jan", "feb", "mar", "apr", "may", "jun",
                           "jul", "aug", "sep", "oct", "nov", "dec"};

/* days from civil (Howard Hinnant), shifted to the Stata epoch 1960-01-01 */
long long stata_days(int y, int m, int d) {
    long long yy = y;
    yy -= m <= 2;
    long long era = (yy >= 0 ? yy : yy - 399) / 400;
    long long yoe = yy - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days_unix = era * 146097 + doe - 719468; /* since 1970-01-01 */
    return days_unix + 3653;                           /* since 1960-01-01 */
}

/* proleptic Gregorian leap year and month length, for calendar validation of
 * td()/tc()/tC() literals (DATE-LIT-1). Native Stata rejects an impossible
 * calendar date (31feb2020, 29feb2019, 31apr2020, day 0/32) with r(198); parqit
 * must not silently roll it forward via stata_days() arithmetic. */
static bool is_leap_year(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}
static int month_length(int y, int m) {
    static const int dpm[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && is_leap_year(y)) return 29;
    return dpm[m];
}

bool parse_dmy(const std::string &raw, int *dd, int *mm, int *yy) {
    /* ddmonyyyy with optional spaces: "1 jan 2020", "01jan2020" */
    std::string t;
    for (char c : raw)
        if (c != ' ' && c != '\t') t.push_back(static_cast<char>(std::tolower(c)));
    size_t p = 0;
    if (p >= t.size() || !std::isdigit(static_cast<unsigned char>(t[p]))) return false;
    int d = 0;
    while (p < t.size() && std::isdigit(static_cast<unsigned char>(t[p])))
        d = d * 10 + (t[p++] - '0');
    if (p + 3 > t.size()) return false;
    std::string mon = t.substr(p, 3);
    int m = 0;
    for (int k = 0; k < 12; k++)
        if (mon == kMonths[k]) m = k + 1;
    if (!m) return false;
    p += 3;
    if (p >= t.size()) return false;
    int y = 0;
    size_t yd = 0;
    while (p < t.size() && std::isdigit(static_cast<unsigned char>(t[p]))) {
        y = y * 10 + (t[p++] - '0');
        yd++;
    }
    if (p != t.size() || yd != 4) return false;
    /* DATE-LIT-1: reject impossible day-of-month (m is already 1..12 from the
     * month-name lookup), matching native Stata's r(198) rather than rolling
     * the date forward. */
    if (d < 1 || d > month_length(y, m)) return false;
    *dd = d;
    *mm = m;
    *yy = y;
    return true;
}

/* yyyy<sep>n forms: 2020m7, 2020q3, 2020h2, 2020w52 */
bool parse_period(const std::string &raw, char sep, int maxn, int *yy, int *nn) {
    std::string t;
    for (char c : raw)
        if (c != ' ') t.push_back(static_cast<char>(std::tolower(c)));
    size_t p = 0;
    int y = 0;
    size_t yd = 0;
    while (p < t.size() && std::isdigit(static_cast<unsigned char>(t[p]))) {
        y = y * 10 + (t[p++] - '0');
        yd++;
    }
    if (yd != 4 || p >= t.size() || t[p] != sep) return false;
    p++;
    int n = 0;
    size_t nd = 0;
    while (p < t.size() && std::isdigit(static_cast<unsigned char>(t[p]))) {
        n = n * 10 + (t[p++] - '0');
        nd++;
    }
    if (!nd || p != t.size() || n < 1 || n > maxn) return false;
    *yy = y;
    *nn = n;
    return true;
}

bool parse_hms(const std::string &raw, long long *ms_out) {
    /* hh:mm[:ss[.fff]] */
    int h = 0, m = 0;
    double sec = 0;
    size_t p = 0;
    std::string t;
    for (char c : raw)
        if (c != ' ') t.push_back(c);
    auto num = [&](int *out) {
        int v = 0;
        size_t d = 0;
        while (p < t.size() && std::isdigit(static_cast<unsigned char>(t[p]))) {
            v = v * 10 + (t[p++] - '0');
            d++;
        }
        *out = v;
        return d > 0;
    };
    if (!num(&h)) return false;
    if (p >= t.size() || t[p++] != ':') return false;
    if (!num(&m)) return false;
    if (p < t.size() && t[p] == ':') {
        p++;
        size_t start = p;
        while (p < t.size() &&
               (std::isdigit(static_cast<unsigned char>(t[p])) || t[p] == '.'))
            p++;
        if (p == start) return false;
        if (!atod(t.substr(start, p - start), &sec)) return false;
    }
    /* DATE-LIT-1: a clock second is 0..59 (fractional ok); 60 is not a valid
     * tc() second (native rejects with r(198)). %tC leap seconds (a real :60 on
     * a leap-second instant) are intentionally rejected too: parqit stores tC as
     * the same count as tc (no leap-second table — documented), so a :60 here
     * could only be silently mis-converted, and a loud error is the safe match. */
    if (p != t.size() || h > 23 || m > 59 || sec >= 60.0) return false;
    *ms_out = static_cast<long long>(((h * 60 + m) * 60) * 1000 +
                                     static_cast<long long>(sec * 1000.0 + 0.5));
    return true;
}

/* ------------------------------------------------------------- parser --- */

struct Val {
    std::string sql;
    char kind = 'n'; /* n / s / b */
    std::string col; /* MISS-1: source column name iff this is a bare column
                      * reference (else ""); lets missing() skip the finite
                      * scan on a boundary-normalized column. */
};

struct Parser {
    Lexer lex;
    Token cur;
    const ExprSchema &schema;
    bool stmiss;
    std::string error;
    const std::string &src;

    Parser(const std::string &s, const ExprSchema &sch, bool sm)
        : lex(s), schema(sch), stmiss(sm), src(s) {
        advance();
    }

    void advance() {
        cur = lex.next();
        if (!lex.error.empty() && error.empty()) error = lex.error;
    }

    bool fail(const std::string &msg) {
        if (error.empty())
            error = msg + " (at position " + std::to_string(cur.pos + 1) + " in: " +
                    src + ")";
        return false;
    }

    bool expect(Tok t, const char *what) {
        if (cur.t != t) return fail(std::string("expected ") + what);
        advance();
        return true;
    }

    /* numeric coercion of boolean (comparisons are 0/1 numbers in Stata) */
    std::string as_num(const Val &v) {
        if (v.kind == 'b') return "(CASE WHEN " + v.sql + " THEN 1 WHEN NOT (" +
                                  v.sql + ") THEN 0 ELSE NULL END)";
        return v.sql;
    }
    std::string as_bool(const Val &v) {
        if (v.kind == 'b') return v.sql;
        if (v.kind == 's') return ""; /* caller errors */
        /* Stata truth: nonzero OR missing — a missing value counts as true
         * (`. & 1` == 1, `keep if x` keeps a missing x). */
        return "((" + v.sql + ") IS NULL OR (" + v.sql + ") <> 0)";
    }

    /* ordering comparison with optional statamissing expansion */
    bool relational(const Val &l, Tok op, const Val &r, Val *out) {
        if (l.kind == 's' || r.kind == 's') {
            if (l.kind != r.kind)
                return fail("cannot compare a string with a number");
            const char *o = op == Tok::Eq ? " = " : op == Tok::Ne ? " <> "
                           : op == Tok::Lt ? " < " : op == Tok::Gt ? " > "
                           : op == Tok::Le ? " <= " : " >= ";
            /* Stata strings have no missing: "" == "" is just equality, but
             * a NULL produced mid-pipeline must behave like "" */
            out->sql = "(coalesce(" + l.sql + ", '')" + o + "coalesce(" + r.sql +
                       ", ''))";
            out->kind = 'b';
            return true;
        }
        std::string a = as_num(l), b = as_num(r);
        const bool lmiss = (l.sql == "NULL"), rmiss = (r.sql == "NULL");
        auto wrap = [&](const std::string &cmp) {
            out->sql = cmp;
            out->kind = 'b';
            return true;
        };
        /* literal-missing comparisons always mean IS NULL tests */
        if (rmiss || lmiss) {
            const std::string &x = rmiss ? a : b;
            Tok o = op;
            if (lmiss && !rmiss) { /* flip so the column is on the left */
                o = (op == Tok::Lt ? Tok::Gt : op == Tok::Gt ? Tok::Lt
                     : op == Tok::Le ? Tok::Ge : op == Tok::Ge ? Tok::Le : op);
            }
            if (lmiss && rmiss) {
                /* . == . is true; . < . false, etc. */
                bool v = (o == Tok::Eq || o == Tok::Le || o == Tok::Ge);
                return wrap(v ? "TRUE" : "FALSE");
            }
            switch (o) {
            case Tok::Eq: case Tok::Ge:
                return wrap("(" + x + " IS NULL)");
            case Tok::Ne: case Tok::Lt:
                return wrap("(" + x + " IS NOT NULL)");
            case Tok::Gt: /* x > . : never */
                return wrap("FALSE");
            case Tok::Le: /* x <= . : always (missing included) */
                return wrap("TRUE");
            default:
                return fail("internal: bad relational");
            }
        }
        const char *o = op == Tok::Eq ? " = " : op == Tok::Ne ? " <> "
                       : op == Tok::Lt ? " < " : op == Tok::Gt ? " > "
                       : op == Tok::Le ? " <= " : " >= ";
        if (!stmiss) return wrap("(" + a + o + b + ")");
        /* statamissing: missing sorts ABOVE every number and comparisons are
         * TOTAL (Stata comparisons never yield missing). Treat NULL as +inf. */
        switch (op) {
        case Tok::Eq:
            return wrap("((" + a + " IS NULL AND " + b + " IS NULL) OR (" + a +
                        " IS NOT NULL AND " + b + " IS NOT NULL AND " + a +
                        " = " + b + "))");
        case Tok::Ne:
            return wrap("(NOT ((" + a + " IS NULL AND " + b + " IS NULL) OR (" +
                        a + " IS NOT NULL AND " + b + " IS NOT NULL AND " + a +
                        " = " + b + ")))");
        case Tok::Lt:
            return wrap("(" + a + " IS NOT NULL AND (" + b + " IS NULL OR " + a +
                        " < " + b + "))");
        case Tok::Le:
            return wrap("((" + a + " IS NULL AND " + b + " IS NULL) OR (" + a +
                        " IS NOT NULL AND (" + b + " IS NULL OR " + a + " <= " +
                        b + ")))");
        case Tok::Gt:
            return wrap("(" + b + " IS NOT NULL AND (" + a + " IS NULL OR " + a +
                        " > " + b + "))");
        case Tok::Ge:
            return wrap("((" + a + " IS NULL AND " + b + " IS NULL) OR (" + b +
                        " IS NOT NULL AND (" + a + " IS NULL OR " + a + " >= " +
                        b + ")))");
        default:
            return fail("internal: bad relational");
        }
    }

    /* function call: name already consumed */
    bool call(const std::string &fname, Val *out);

    bool primary(Val *out) {
        if (!error.empty()) return false;
        switch (cur.t) {
        case Tok::Num:
            out->sql = cur.text;
            out->kind = 'n';
            advance();
            return true;
        case Tok::MissingDot:
            out->sql = "NULL"; /* .a-.z collapse to NULL (documented) */
            out->kind = 'n';
            advance();
            return true;
        case Tok::String:
            /* reject the internal _n/_N row-context markers in user literals so
             * the view compiler's substitution can never corrupt the literal
             * or spuriously activate row-context machinery */
            if (cur.text.find("__PARQIT_ROW__") != std::string::npos ||
                cur.text.find("__PARQIT_NROWS__") != std::string::npos)
                return fail("string literal may not contain the reserved token "
                            "__PARQIT_ROW__/__PARQIT_NROWS__");
            out->sql = quote_literal(cur.text);
            out->kind = 's';
            advance();
            return true;
        case Tok::SysN:
            out->sql = "__PARQIT_ROW__"; /* resolved by the view compiler */
            out->kind = 'n';
            advance();
            return true;
        case Tok::SysBigN:
            out->sql = "__PARQIT_NROWS__";
            out->kind = 'n';
            advance();
            return true;
        case Tok::LParen: {
            advance();
            if (!or_expr(out)) return false;
            return expect(Tok::RParen, ")");
        }
        case Tok::Ident: {
            std::string name = cur.text;
            advance();
            /* INJID-1: the row-context sentinels are substituted by a raw
             * find/replace in the view compiler; a column (or function) named
             * with one would be corrupted into malformed SQL. Reject it loudly
             * here, mirroring the string-literal guard above. */
            if (name.find("__PARQIT_ROW__") != std::string::npos ||
                name.find("__PARQIT_NROWS__") != std::string::npos)
                return fail("identifier may not contain the reserved token "
                            "__PARQIT_ROW__/__PARQIT_NROWS__ (used internally "
                            "for _n/_N)");
            if (cur.t == Tok::LParen) {
                advance();
                return call(name, out);
            }
            auto it = schema.kinds.find(name);
            if (it == schema.kinds.end())
                return fail("variable " + name + " not found in the view");
            out->sql = quote_ident(name);
            out->kind = it->second == 's' ? 's' : 'n';
            out->col = name; /* MISS-1: remember this is a bare column ref */
            return true;
        }
        default:
            return fail("expected a value");
        }
    }

    bool unary(Val *out) {
        if (cur.t == Tok::Minus) {
            advance();
            Val v;
            if (!unary(&v)) return false;
            if (v.kind == 's') return fail("cannot negate a string");
            out->sql = "(-" + as_num(v) + ")";
            out->kind = 'n';
            return true;
        }
        if (cur.t == Tok::Plus) {
            advance();
            return unary(out);
        }
        if (cur.t == Tok::Not) {
            advance();
            Val v;
            if (!unary(&v)) return false;
            std::string b = as_bool(v);
            if (b.empty()) return fail("cannot apply ! to a string");
            /* Stata: !x is 1/0; a missing x counts as true (as_bool), so
             * !missing == 0 — matching Stata's `!.` == 0. */
            out->sql = "(NOT " + b + ")";
            out->kind = 'b';
            return true;
        }
        return power(out);
    }

    /* a primary with optional leading sign — the operand of ^, which binds
     * tighter than * and + but lets the exponent itself be signed (2^-1) */
    bool signed_primary(Val *out) {
        if (cur.t == Tok::Minus) {
            advance();
            Val v;
            if (!signed_primary(&v)) return false;
            if (v.kind == 's') return fail("cannot negate a string");
            out->sql = "(-" + as_num(v) + ")";
            out->kind = 'n';
            return true;
        }
        if (cur.t == Tok::Plus) {
            advance();
            return signed_primary(out);
        }
        return primary(out);
    }

    bool power(Val *out) {
        Val base;
        if (!primary(&base)) return false;
        /* ^ is LEFT-associative in Stata: 2^3^2 == (2^3)^2 == 64. Fold left,
         * parsing each exponent as a signed primary (not unary) so the chain
         * does not re-descend into power() and become right-associative. */
        while (cur.t == Tok::Caret) {
            advance();
            Val expo;
            if (!signed_primary(&expo)) return false;
            if (base.kind == 's' || expo.kind == 's')
                return fail("^ needs numeric operands");
            /* Stata returns missing for a non-real or overflowing power, e.g.
             * (-8)^0.5 = . ; DuckDB pow() yields nan/inf. Guard to missing so
             * collect and save agree (NUM-1). */
            std::string pw = "pow(" + as_num(base) + ", " + as_num(expo) + ")";
            base.sql = "(CASE WHEN isfinite(" + pw + ") THEN " + pw +
                       " ELSE NULL END)";
            base.kind = 'n';
        }
        *out = base;
        return true;
    }

    bool term(Val *out) {
        if (!unary(out)) return false;
        while (cur.t == Tok::Star || cur.t == Tok::Slash) {
            Tok op = cur.t;
            advance();
            Val r;
            if (!unary(&r)) return false;
            if (out->kind == 's' || r.kind == 's')
                return fail("* and / need numeric operands");
            if (op == Tok::Slash) {
                /* Stata x/0 = . (missing) and never integer-divides; DuckDB
                 * double division yields inf/nan. Guard so a finite quotient
                 * passes through and any non-finite result (incl. 0/0) becomes
                 * missing — keeping collect and save in agreement (NUM-1). */
                std::string d = "(" + as_num(*out) + " / CAST(" + as_num(r) +
                                " AS DOUBLE))";
                out->sql = "(CASE WHEN isfinite(" + d + ") THEN " + d +
                           " ELSE NULL END)";
            } else {
                out->sql = "(" + as_num(*out) + " * " + as_num(r) + ")";
            }
            out->kind = 'n';
        }
        return true;
    }

    bool arith(Val *out) {
        if (!term(out)) return false;
        while (cur.t == Tok::Plus || cur.t == Tok::Minus) {
            Tok op = cur.t;
            advance();
            Val r;
            if (!term(&r)) return false;
            if (out->kind == 's' || r.kind == 's') {
                if (op == Tok::Plus && out->kind == 's' && r.kind == 's') {
                    /* Stata string concatenation */
                    out->sql = "(coalesce(" + out->sql + ",'') || coalesce(" +
                               r.sql + ",''))";
                    out->kind = 's';
                    continue;
                }
                return fail("+/- need matching operand types");
            }
            out->sql = "(" + as_num(*out) + (op == Tok::Plus ? " + " : " - ") +
                       as_num(r) + ")";
            out->kind = 'n';
        }
        return true;
    }

    bool rel(Val *out) {
        if (!arith(out)) return false;
        /* EXPR-4: relational operators chain LEFT-associatively in Stata, so
         * `1 < wage < 3000` parses as `(1 < wage) < 3000` — a 0/1 result, never
         * an error. Loop and fold; relational() coerces the boolean left operand
         * back to a number via as_num(). */
        while (cur.t == Tok::Eq || cur.t == Tok::Ne || cur.t == Tok::Lt ||
               cur.t == Tok::Gt || cur.t == Tok::Le || cur.t == Tok::Ge) {
            Tok op = cur.t;
            advance();
            Val r;
            if (!arith(&r)) return false;
            Val res;
            if (!relational(*out, op, r, &res)) return false;
            *out = res;
        }
        return true;
    }

    bool and_expr(Val *out) {
        if (!rel(out)) return false;
        while (cur.t == Tok::And) {
            advance();
            Val r;
            if (!rel(&r)) return false;
            std::string a = as_bool(*out), b = as_bool(r);
            if (a.empty() || b.empty()) return fail("& needs boolean operands");
            out->sql = "(" + a + " AND " + b + ")";
            out->kind = 'b';
        }
        return true;
    }

    bool or_expr(Val *out) {
        if (!and_expr(out)) return false;
        while (cur.t == Tok::Or) {
            advance();
            Val r;
            if (!and_expr(&r)) return false;
            std::string a = as_bool(*out), b = as_bool(r);
            if (a.empty() || b.empty()) return fail("| needs boolean operands");
            out->sql = "(" + a + " OR " + b + ")";
            out->kind = 'b';
        }
        return true;
    }
};

bool Parser::call(const std::string &fname, Val *out) {
    /* ---- date/time literal pseudofunctions: consume raw text to ')' ---- */
    if (fname == "td" || fname == "tm" || fname == "tq" || fname == "th" ||
        fname == "tw" || fname == "ty" || fname == "tc" || fname == "tC") {
        size_t start = cur.pos;
        /* re-scan raw source from the first token after '(' */
        size_t depth = 1, j = start;
        while (j < src.size() && depth > 0) {
            if (src[j] == '(') depth++;
            else if (src[j] == ')') depth--;
            if (depth == 0) break;
            j++;
        }
        if (depth != 0) return fail("unterminated " + fname + "() literal");
        std::string raw = src.substr(start, j - start);
        /* resync the lexer past the ')' */
        lex.i = j + 1;
        advance();

        long long value = 0;
        int y, m, d, n;
        if (fname == "td") {
            if (!parse_dmy(raw, &d, &m, &y))
                return fail("td(): expected ddmonyyyy, got '" + raw + "'");
            value = stata_days(y, m, d);
        } else if (fname == "ty") {
            char *end = nullptr;
            value = std::strtoll(raw.c_str(), &end, 10);
            if (!end || *end != '\0')
                return fail("ty(): expected a year, got '" + raw + "'");
        } else if (fname == "tm") {
            if (!parse_period(raw, 'm', 12, &y, &n))
                return fail("tm(): expected yyyymN like 2026m1, got '" + raw + "'");
            value = static_cast<long long>(y - 1960) * 12 + (n - 1);
        } else if (fname == "tq") {
            if (!parse_period(raw, 'q', 4, &y, &n))
                return fail("tq(): expected yyyyqN, got '" + raw + "'");
            value = static_cast<long long>(y - 1960) * 4 + (n - 1);
        } else if (fname == "th") {
            if (!parse_period(raw, 'h', 2, &y, &n))
                return fail("th(): expected yyyyhN, got '" + raw + "'");
            value = static_cast<long long>(y - 1960) * 2 + (n - 1);
        } else if (fname == "tw") {
            if (!parse_period(raw, 'w', 52, &y, &n))
                return fail("tw(): expected yyyywN, got '" + raw + "'");
            value = static_cast<long long>(y - 1960) * 52 + (n - 1);
        } else { /* tc / tC: ddmonyyyy hh:mm[:ss[.fff]] */
            size_t timepos = raw.find(':');
            if (timepos == std::string::npos)
                return fail(fname + "(): expected ddmonyyyy hh:mm:ss");
            size_t cut = raw.substr(0, timepos).rfind(' ');
            if (cut == std::string::npos)
                return fail(fname + "(): expected ddmonyyyy hh:mm:ss");
            std::string dpart = raw.substr(0, cut), tpart = raw.substr(cut + 1);
            long long ms;
            if (!parse_dmy(dpart, &d, &m, &y) || !parse_hms(tpart, &ms))
                return fail(fname + "(): expected ddmonyyyy hh:mm:ss, got '" +
                            raw + "'");
            value = stata_days(y, m, d) * 86400000LL + ms;
            /* %tC literals: parqit stores tC as the same count (leap seconds
             * not added — documented; tC(...) == tc(...) in parqit) */
        }
        out->sql = std::to_string(value);
        out->kind = 'n';
        return true;
    }

    /* ---- regular functions: parse comma-separated arguments ---- */
    std::vector<Val> args;
    if (cur.t != Tok::RParen) {
        while (true) {
            Val a;
            if (!or_expr(&a)) return false;
            args.push_back(a);
            if (cur.t == Tok::Comma) {
                advance();
                continue;
            }
            break;
        }
    }
    if (!expect(Tok::RParen, ") after function arguments")) return false;

    auto need = [&](size_t lo, size_t hi) {
        if (args.size() < lo || args.size() > hi) {
            fail(fname + "(): wrong number of arguments");
            return false;
        }
        return true;
    };
    auto num1 = [&](const char *sqlname) {
        if (!need(1, 1)) return false;
        if (args[0].kind == 's') return fail(fname + "() needs a numeric argument");
        out->sql = std::string(sqlname) + "(" + as_num(args[0]) + ")";
        out->kind = 'n';
        return true;
    };

    if (fname == "missing" || fname == "mi") {
        if (args.empty()) return fail("missing() needs at least one argument");
        std::string sql = "(";
        for (size_t k = 0; k < args.size(); k++) {
            if (k) sql += " OR ";
            if (args[k].kind == 's') {
                sql += "(coalesce(" + args[k].sql + ", '') = '')";
            } else if (!args[k].col.empty() &&
                       args[k].sql == quote_ident(args[k].col) &&
                       schema.normalized.count(args[k].col)) {
                /* MISS-1 fast path: a bare reference to a boundary-normalized
                 * column cannot hold a non-finite/out-of-range value (the lazy
                 * boundary already nulled any NaN/±Inf), so the cheap IS NULL is
                 * exact — and lets DuckDB use the column's null statistics. The
                 * sql==quote_ident(col) guard makes this robust against a stale
                 * col on a transformed Val: a compound expression never matches,
                 * so it always takes the full finite check below. */
                sql += "(" + args[k].sql + " IS NULL)";
            } else {
                /* MISS-1: a numeric value is Stata-missing when it is SQL NULL,
                 * or a non-finite/out-of-Stata-range float (NaN, ±Inf, or a
                 * magnitude >= the missing sentinel 2^1023). A gen/replace
                 * result or a compound expression can still generate one (e.g.
                 * exp(10000) -> +Inf), which native Stata reports as missing;
                 * mirror that here so missing()/mi() never silently calls a
                 * generated special "not missing". */
                const std::string n = as_num(args[k]);
                const std::string d = "CAST(" + n + " AS DOUBLE)";
                sql += "(" + n + " IS NULL OR NOT isfinite(" + d +
                       ") OR abs(" + d + ") >= " + dtoa(kStataMissThreshold) +
                       ")";
            }
        }
        out->sql = sql + ")";
        out->kind = 'b';
        return true;
    }
    if (fname == "abs") return num1("abs");
    if (fname == "exp") return num1("exp");
    if (fname == "ln" || fname == "log") {
        if (!need(1, 1)) return false;
        /* Stata: ln(x<=0) is missing; DuckDB ln(0)=-inf, ln(<0) NaN/error */
        out->sql = "(CASE WHEN " + as_num(args[0]) + " > 0 THEN ln(" +
                   as_num(args[0]) + ") ELSE NULL END)";
        out->kind = 'n';
        return true;
    }
    if (fname == "log10") {
        if (!need(1, 1)) return false;
        out->sql = "(CASE WHEN " + as_num(args[0]) + " > 0 THEN log10(" +
                   as_num(args[0]) + ") ELSE NULL END)";
        out->kind = 'n';
        return true;
    }
    if (fname == "sqrt") {
        if (!need(1, 1)) return false;
        out->sql = "(CASE WHEN " + as_num(args[0]) + " >= 0 THEN sqrt(" +
                   as_num(args[0]) + ") ELSE NULL END)";
        out->kind = 'n';
        return true;
    }
    if (fname == "floor") return num1("floor");
    if (fname == "ceil") return num1("ceil");
    if (fname == "int" || fname == "trunc") return num1("trunc");
    if (fname == "round") {
        if (!need(1, 2)) return false;
        std::string x = as_num(args[0]);
        /* Stata round(x) = floor(x + 0.5): ties round toward +infinity, NOT
         * away from zero the way SQL round() does — round(-2.5) = -2 (not -3),
         * round(-0.5) = 0 (not -1) (NUM-2). The 2-arg form rounds to units of u
         * the same way, with u = 0 a documented pass-through. */
        if (args.size() == 1) {
            out->sql = "floor((" + x + ") + 0.5)";
        } else {
            std::string u = as_num(args[1]);
            out->sql = "(CASE WHEN (" + u + ") = 0 THEN " + x +
                       " ELSE floor((" + x + ") / (" + u + ") + 0.5) * (" + u +
                       ") END)";
        }
        out->kind = 'n';
        return true;
    }
    if (fname == "mod") {
        if (!need(2, 2)) return false;
        std::string a = as_num(args[0]), b = as_num(args[1]);
        /* Stata mod(x,y) is the nonnegative remainder, and is MISSING for a
         * nonpositive modulus (mod(7,-3)==., mod(7,0)==.). */
        out->sql = "(CASE WHEN (" + b + ") <= 0 THEN NULL ELSE (" + a +
                   ") - (" + b + ") * floor((" + a + ") / CAST(" + b +
                   " AS DOUBLE)) END)";
        out->kind = 'n';
        return true;
    }
    if (fname == "min" || fname == "max") {
        if (!need(2, 64)) return false;
        std::string sql = (fname == "min" ? "least(" : "greatest(");
        for (size_t k = 0; k < args.size(); k++) {
            if (args[k].kind == 's') return fail(fname + "() needs numbers");
            if (k) sql += ", ";
            sql += as_num(args[k]);
        }
        out->sql = sql + ")";
        out->kind = 'n';
        return true;
    }
    if (fname == "cond") {
        if (!need(3, 4)) return false;
        std::string c = as_bool(args[0]);
        if (c.empty()) return fail("cond(): first argument must be boolean");
        if (args[1].kind != args[2].kind &&
            !(args[1].kind != 's' && args[2].kind != 's'))
            return fail("cond(): branches must have the same type");
        std::string a = args[1].kind == 'b' ? as_num(args[1]) : args[1].sql;
        std::string b = args[2].kind == 'b' ? as_num(args[2]) : args[2].sql;
        if (args.size() == 4) {
            std::string d = args[3].kind == 'b' ? as_num(args[3]) : args[3].sql;
            /* 4-arg: a MISSING condition selects the 4th branch. A boolean
             * (comparison) condition is never missing in Stata, so only a
             * numeric condition can take the missing branch. */
            std::string cnull = args[0].kind == 'b'
                                    ? "FALSE"
                                    : "(" + as_num(args[0]) + " IS NULL)";
            out->sql = "(CASE WHEN " + cnull + " THEN " + d + " WHEN " + c +
                       " THEN " + a + " ELSE " + b + " END)";
        } else {
            /* 3-arg: a missing condition is treated as TRUE (Stata) — handled
             * by as_bool() mapping a missing numeric to true. */
            out->sql = "(CASE WHEN " + c + " THEN " + a + " ELSE " + b + " END)";
        }
        out->kind = args[1].kind == 's' ? 's' : 'n';
        return true;
    }
    if (fname == "inrange") {
        if (!need(3, 3)) return false;
        if (args[0].kind == 's' || args[1].kind == 's' || args[2].kind == 's') {
            /* string inrange: keep the equality/ordering path */
            Val lo, hi;
            if (!relational(args[0], Tok::Ge, args[1], &lo)) return false;
            if (!relational(args[0], Tok::Le, args[2], &hi)) return false;
            out->sql = "(coalesce(" + lo.sql + " AND " + hi.sql + ", FALSE))";
            out->kind = 'b';
            return true;
        }
        std::string x = as_num(args[0]), lo = as_num(args[1]), hi = as_num(args[2]);
        /* Stata numeric inrange: a missing x is never in range; a missing
         * lower bound means -inf, a missing upper bound means +inf. */
        out->sql = "((" + x + " IS NOT NULL) AND (" + lo + " IS NULL OR " + x +
                   " >= " + lo + ") AND (" + hi + " IS NULL OR " + x + " <= " +
                   hi + "))";
        out->kind = 'b';
        return true;
    }
    if (fname == "inlist") {
        if (!need(2, 255)) return false;
        std::string sql = "(";
        for (size_t k = 1; k < args.size(); k++) {
            if ((args[k].kind == 's') != (args[0].kind == 's'))
                return fail("inlist(): mixed string/numeric arguments");
            if (k > 1) sql += " OR ";
            Val eq;
            if (!relational(args[0], Tok::Eq, args[k], &eq)) return false;
            sql += eq.sql;
        }
        out->sql = "coalesce(" + sql + "), FALSE)";
        out->kind = 'b';
        return true;
    }
    /* ---- strings (lengths are BYTES, like Stata) ---- */
    if (fname == "strlen" || fname == "length") {
        if (!need(1, 1)) return false;
        /* LENGTH-NUMERIC-1: parqit's length()/strlen() are byte-length of a
         * string; Stata's length() is also defined on numerics (display width),
         * which parqit does not implement here (no per-variable format in the
         * translator). Name the actual function in the error so the message is
         * not misleading; use parqit sql for a numeric width. */
        if (args[0].kind != 's')
            return fail(fname + "() here needs a string argument (parqit does not "
                        "implement numeric " + fname + "(); use parqit sql)");
        out->sql = "strlen(coalesce(" + args[0].sql + ", ''))";
        out->kind = 'n';
        return true;
    }
    if (fname == "ustrlen") {
        if (!need(1, 1)) return false;
        if (args[0].kind != 's') return fail("ustrlen() needs a string");
        out->sql = "length(coalesce(" + args[0].sql + ", ''))";
        out->kind = 'n';
        return true;
    }
    if (fname == "upper" || fname == "strupper" || fname == "ustrupper" ||
        fname == "lower" || fname == "strlower" || fname == "ustrlower") {
        if (!need(1, 1)) return false;
        if (args[0].kind != 's') return fail(fname + "() needs a string");
        bool up = fname.find("upper") != std::string::npos;
        bool uni = fname.rfind("ustr", 0) == 0; /* ustrupper / ustrlower */
        std::string in = "coalesce(" + args[0].sql + ", '')";
        if (uni) {
            /* Stata ustrupper/ustrlower are Unicode-aware. */
            out->sql = std::string(up ? "upper(" : "lower(") + in + ")";
        } else {
            /* Stata upper()/lower() (and strupper/strlower) fold ASCII a-z/A-Z
             * ONLY and leave every byte >= 0x80 untouched, so upper("café") is
             * "CAFé", not "CAFÉ" (STR-1). translate() over the 26 ASCII letters
             * is byte-safe in UTF-8: the letters are single code points and the
             * bytes of a multibyte sequence are never in the from-set. */
            static const char *kLow = "abcdefghijklmnopqrstuvwxyz";
            static const char *kUp = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
            out->sql = "translate(" + in + ", '" + (up ? kLow : kUp) + "', '" +
                       (up ? kUp : kLow) + "')";
        }
        out->kind = 's';
        return true;
    }
    if (fname == "trim" || fname == "strtrim") {
        if (!need(1, 1)) return false;
        out->sql = "trim(coalesce(" + args[0].sql + ", ''))";
        out->kind = 's';
        return true;
    }
    if (fname == "ltrim" || fname == "rtrim") {
        if (!need(1, 1)) return false;
        out->sql = fname.substr(0, 5) + "(coalesce(" + args[0].sql + ", ''))";
        out->kind = 's';
        return true;
    }
    if (fname == "substr") {
        if (!need(3, 3)) return false;
        if (args[0].kind != 's') return fail("substr() needs a string");
        std::string s = "coalesce(" + args[0].sql + ", '')";
        std::string p = as_num(args[1]);
        /* Stata substr() indexes by BYTE and can return invalid UTF-8 byte
         * fragments. DuckDB's SQL substring is character-based, so use parqit's
         * internal C scalar over the raw string bytes. */
        std::string n = args[2].sql == "NULL" ? "CAST(NULL AS DOUBLE)" : as_num(args[2]);
        out->sql = "parqit_substr_bytes(" + s + ", " + p + ", " + n + ")";
        out->kind = 's';
        return true;
    }
    if (fname == "strpos") {
        if (!need(2, 2)) return false;
        if (args[0].kind != 's' || args[1].kind != 's')
            return fail("strpos() needs strings");
        std::string s = "coalesce(" + args[0].sql + ", '')";
        std::string sub = "coalesce(" + args[1].sql + ", '')";
        /* Stata strpos() returns a BYTE offset (ustrpos() is the character
         * variant). Convert DuckDB's character position to a byte offset via
         * the byte length of the matched prefix. Stata strpos(s,"") == 0 for an
         * empty needle, but DuckDB strpos(s,'') == 1 — guard it (STRPOS-EMPTY-1). */
        out->sql = "(CASE WHEN " + sub + " = '' THEN 0 "
                   "WHEN strpos(" + s + ", " + sub + ") = 0 THEN 0 ELSE "
                   "strlen(substr(" + s + ", 1, strpos(" + s + ", " + sub +
                   ") - 1)) + 1 END)";
        out->kind = 'n';
        return true;
    }
    if (fname == "subinstr") {
        if (!need(4, 4)) return false;
        if (args[3].sql != "NULL")
            return fail("subinstr(): only the replace-all form (last argument .) "
                        "is supported");
        out->sql = "replace(coalesce(" + args[0].sql + ", ''), " + args[1].sql +
                   ", " + args[2].sql + ")";
        out->kind = 's';
        return true;
    }
    if (fname == "string" || fname == "strofreal") {
        if (!need(1, 1)) return false;
        if (args[0].kind == 's') return fail("string() needs a number");
        /* Stata's default string()/strofreal() is %9.0g, a width-constrained
         * general format rather than a fixed significant-digit printf. The
         * internal scalar mirrors Stata's decimal/scientific switch and
         * mantissa width, while NULL numerics still become ".". */
        std::string x = "CAST(" + as_num(args[0]) + " AS DOUBLE)";
        out->sql = "parqit_stata_string(" + x + ")";
        out->kind = 's';
        return true;
    }
    if (fname == "real") {
        if (!need(1, 1)) return false;
        if (args[0].kind != 's') return fail("real() needs a string");
        /* Stata real() yields missing for non-finite text ('inf'/'nan'); a raw
         * TRY_CAST would keep them as inf/nan, so map non-finite to NULL. */
        out->sql = "(CASE WHEN isfinite(TRY_CAST(" + args[0].sql +
                   " AS DOUBLE)) THEN TRY_CAST(" + args[0].sql +
                   " AS DOUBLE) ELSE NULL END)";
        out->kind = 'n';
        return true;
    }
    if (fname == "regexm") {
        if (!need(2, 2)) return false;
        if (args[0].kind != 's' || args[1].kind != 's')
            return fail("regexm() needs strings");
        out->sql = "regexp_matches(coalesce(" + args[0].sql + ", ''), " +
                   args[1].sql + ")";
        out->kind = 'b';
        return true;
    }
    /* date part extraction over Stata day counts */
    auto datepart = [&](const char *part) {
        if (!need(1, 1)) return false;
        if (args[0].kind == 's') return fail(fname + "() needs a numeric date");
        out->sql = std::string(part) + "(DATE '1960-01-01' + CAST(" +
                   as_num(args[0]) + " AS INTEGER))";
        out->kind = 'n';
        return true;
    };
    if (fname == "year") return datepart("year");
    if (fname == "month") return datepart("month");
    if (fname == "day") return datepart("day");
    if (fname == "quarter") return datepart("quarter");
    if (fname == "dow") {
        if (!need(1, 1)) return false;
        /* Stata: 0=Sunday; DuckDB dayofweek: 0=Sunday too */
        out->sql = "dayofweek(DATE '1960-01-01' + CAST(" + as_num(args[0]) +
                   " AS INTEGER))";
        out->kind = 'n';
        return true;
    }
    if (fname == "doy") {
        if (!need(1, 1)) return false;
        out->sql = "dayofyear(DATE '1960-01-01' + CAST(" + as_num(args[0]) +
                   " AS INTEGER))";
        out->kind = 'n';
        return true;
    }
    if (fname == "mdy") {
        if (!need(3, 3)) return false;
        /* Stata mdy() of an invalid date (mdy(2,30,2020), mdy(13,1,2020)) is
         * row-local missing, not a hard error; try() turns DuckDB's range error
         * into NULL so one bad triple no longer aborts the whole query (DATE-1). */
        out->sql = "(try(make_date(CAST(" + as_num(args[2]) +
                   " AS INTEGER), CAST(" + as_num(args[0]) + " AS INTEGER), CAST(" +
                   as_num(args[1]) + " AS INTEGER)) - DATE '1960-01-01'))";
        out->kind = 'n';
        return true;
    }
    if (fname == "dofm") { /* month count → day count of month start */
        if (!need(1, 1)) return false;
        /* try(): an out-of-range month count yields missing, not a query abort
         * (DATE-1). */
        out->sql = "(try(make_date(1960 + CAST(floor((" + as_num(args[0]) +
                   ") / 12.0) AS INTEGER), CAST((" + as_num(args[0]) +
                   ") - 12 * floor((" + as_num(args[0]) +
                   ") / 12.0) AS INTEGER) + 1, 1) - DATE '1960-01-01'))";
        out->kind = 'n';
        return true;
    }
    if (fname == "mofd") {
        if (!need(1, 1)) return false;
        std::string dd = "(DATE '1960-01-01' + CAST(" + as_num(args[0]) +
                         " AS INTEGER))";
        out->sql = "((year" + std::string("(") + dd + ") - 1960) * 12 + month(" +
                   dd + ") - 1)";
        out->kind = 'n';
        return true;
    }
    if (fname == "yofd") {
        if (!need(1, 1)) return false;
        out->sql = "year(DATE '1960-01-01' + CAST(" + as_num(args[0]) +
                   " AS INTEGER))";
        out->kind = 'n';
        return true;
    }

    return fail("function " + fname +
                "() is not supported in parqit expressions (yet); see help parqit "
                "for the supported list, or use parqit sql");
}

} // namespace

ExprResult translate_expression(const std::string &expr, const ExprSchema &schema,
                                bool statamissing) {
    ExprResult r;
    Parser p(expr, schema, statamissing);
    Val v;
    if (!p.or_expr(&v) || !p.error.empty()) {
        r.error = p.error.empty() ? "could not parse expression" : p.error;
        return r;
    }
    if (p.cur.t != Tok::End) {
        p.fail("unexpected trailing input");
        r.error = p.error;
        return r;
    }
    r.ok = true;
    r.sql = v.kind == 'b'
                ? "(CASE WHEN " + v.sql + " THEN 1 WHEN NOT (" + v.sql +
                      ") THEN 0 ELSE NULL END)"
                : v.sql;
    if (v.kind == 'b') {
        /* comparisons are 1/0 numbers when assigned (gen y = a > b) */
        r.kind = 'n';
        /* Stata: a > b with missing a is TRUE (missing sorts high) only in
         * stata semantics; in SQL mode NULL comparison → NULL → missing.
         * For assignment Stata makes (5 > .) = 0… faithful translation of
         * assignment context uses the same comparison semantics chosen by
         * the mode; documented. */
    } else {
        r.kind = v.kind;
    }
    return r;
}

ExprResult translate_filter(const std::string &expr, const ExprSchema &schema,
                            bool statamissing) {
    ExprResult r;
    Parser p(expr, schema, statamissing);
    Val v;
    if (!p.or_expr(&v) || !p.error.empty()) {
        r.error = p.error.empty() ? "could not parse expression" : p.error;
        return r;
    }
    if (p.cur.t != Tok::End) {
        p.fail("unexpected trailing input");
        r.error = p.error;
        return r;
    }
    if (v.kind == 's') {
        r.error = "a string expression cannot be a condition";
        return r;
    }
    r.ok = true;
    r.kind = 'b';
    /* Stata `keep if x`: a row is kept when x is nonzero OR missing (a missing
     * value is treated as true), matching as_bool(). */
    r.sql = (v.kind == 'b')
                ? v.sql
                : "((" + v.sql + ") IS NULL OR (" + v.sql + ") <> 0)";
    return r;
}

} // namespace parqit
