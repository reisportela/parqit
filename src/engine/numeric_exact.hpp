#pragma once
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(__FAST_MATH__) || (defined(_M_FP_FAST) && _M_FP_FAST)
#error "parqit numeric kernels require IEEE arithmetic without reassociation"
#endif
static_assert(FLT_EVAL_METHOD == 0, "parqit numeric kernels require binary64 evaluation");

namespace parqit::numeric {

// Signed integer units of 2^-1074. Binary64 terms and a uint64 count need
// at most 2163 signed bits; 34 limbs leave headroom without a heap allocation.
struct BinarySum {
    static constexpr unsigned limbs = 34;
    std::array<uint64_t, limbs> word{};

    void add_word(unsigned i, uint64_t x, bool negative) {
        if (!x) return;
        uint64_t before = word[i];
        word[i] = negative ? before-x : before+x;
        bool carry = negative ? before<x : word[i]<before;
        while (carry && ++i<limbs) {
            before=word[i];
            word[i] = negative ? before-1 : before+1;
            carry = negative ? before==0 : word[i]==0;
        }
    }
    void add(double x) {
        uint64_t bits;
        std::memcpy(&bits,&x,sizeof bits);
        const unsigned exponent = (bits>>52)&2047;
        if (exponent==2047) return;
        const uint64_t m=(bits&0xfffffffffffffULL)|(exponent ? 1ULL<<52 : 0);
        const unsigned shift=exponent ? exponent-1 : 0;
        const unsigned i=shift/64, offset=shift%64;
        const bool negative=(bits>>63)!=0;
        add_word(i,m<<offset,negative);
        if (offset) add_word(i+1,m>>(64-offset),negative);
    }
    void add_multiple(double value, uint64_t count) {
        uint64_t bits;
        std::memcpy(&bits, &value, sizeof bits);
        const unsigned exponent = (bits >> 52) & 2047;
        if (exponent == 2047 || !count) return;
        const uint64_t mantissa = (bits & 0xfffffffffffffULL) | (exponent ? 1ULL << 52 : 0);
        const unsigned shift = exponent ? exponent - 1 : 0;
        for (unsigned a = 0; a < 2; ++a) {
            for (unsigned b = 0; b < 2; ++b) {
                const uint64_t product = uint64_t(uint32_t(mantissa >> (32 * a))) *
                                         uint32_t(count >> (32 * b));
                const unsigned position = shift + 32 * (a + b);
                const unsigned word_index = position / 64, offset = position % 64;
                add_word(word_index, product << offset, (bits >> 63) != 0);
                if (offset) add_word(word_index + 1, product >> (64 - offset), (bits >> 63) != 0);
            }
        }
    }
    int compare(const BinarySum &other) const {
        if (negative() != other.negative()) return negative() ? -1 : 1;
        for (int i = limbs - 1; i >= 0; --i)
            if (word[i] != other.word[i]) return word[i] < other.word[i] ? -1 : 1;
        return 0;
    }
    void combine(const BinarySum &other) {
        uint64_t carry=0;
        for (unsigned i=0;i<limbs;++i) {
            const uint64_t a=word[i], b=other.word[i], s=a+b;
            const uint64_t t=s+carry;
            carry=(s<a)|(t<s);
            word[i]=t;
        }
    }
    bool negative() const { return (word.back()>>63)!=0; }
    BinarySum magnitude() const {
        BinarySum out=*this;
        if (negative()) {
            for (auto &w:out.word) w=~w;
            out.add_word(0,1,false);
        }
        return out;
    }
    int highest() const {
        int i=limbs-1;
        while (i>=0 && word[i]==0) --i;
        if (i<0) return -1;
        uint64_t w=word[i];
        unsigned b=0;
        for (unsigned step:{32U,16U,8U,4U,2U,1U})
            if (w>>step) { w>>=step; b+=step; }
        return i*64+static_cast<int>(b);
    }
    bool any_below(unsigned bit) const {
        for (unsigned i=0;i<bit/64;++i) if (word[i]) return true;
        return bit%64 && (word[bit/64]&((1ULL<<(bit%64))-1));
    }
    double rounded(uint64_t remainder=0, uint64_t divisor=1) const {
        const int high=highest();
        const unsigned shift=high>52 ? static_cast<unsigned>(high-52) : 0;
        const unsigned i=shift/64, offset=shift%64;
        uint64_t top=word[i]>>offset;
        if (offset && i+1<limbs) top|=word[i+1]<<(64-offset);
        if (shift) {
            const bool guard=(word[(shift-1)/64]>>((shift-1)%64))&1;
            if (guard && ((top&1)||any_below(shift-1)||remainder)) ++top;
        } else if (remainder>divisor-remainder ||
                   (remainder==divisor-remainder && (top&1))) ++top;
        return std::scalbn(static_cast<double>(top),static_cast<int>(shift)-1074);
    }
    double sum() const {
        const double value=magnitude().rounded();
        return negative() ? -value : value;
    }
    uint64_t shifted_word(unsigned shift) const {
        const unsigned i=shift/64, offset=shift%64;
        if (i>=limbs) return 0;
        uint64_t value=word[i]>>offset;
        if (offset && i+1<limbs) value|=word[i+1]<<(64-offset);
        return value;
    }
    double mean(uint64_t n) const {
        if (!n) return NAN;
        const BinarySum mag=magnitude();
        const int high=mag.highest();
        if (high<0) return 0;
        uint64_t top=n;
        int n_high=0;
        for (unsigned step:{32U,16U,8U,4U,2U,1U})
            if (top>>step) { top>>=step; n_high+=step; }
        int exponent=high-n_high;
        if (exponent>=0 && mag.shifted_word(exponent)<n) --exponent;
        if (exponent<0) {
            const double value=mag.word[0]>n-mag.word[0] ? std::scalbn(1.,-1074) : 0.;
            return negative() ? -value : value;
        }
        const unsigned shift=exponent>52 ? unsigned(exponent-52) : 0;
        const uint64_t low=mag.shifted_word(shift), high_word=mag.shifted_word(shift+64);
        uint64_t q=0, remainder=high_word;
        if (!high_word) { q=low/n; remainder=low%n; }
        else if (n<=UINT32_MAX) {
            for (int half=1;half>=0;--half) {
                const uint64_t part=(remainder<<32)|((low>>(32*half))&UINT32_MAX);
                q|=(part/n)<<(32*half);
                remainder=part%n;
            }
        } else {
            for (int bit=63;bit>=0;--bit) {
                const bool overflow=(remainder>>63)!=0;
                remainder=(remainder<<1)|((low>>bit)&1);
                if (overflow || remainder>=n) { remainder-=n; q|=1ULL<<bit; }
            }
        }
        const uint64_t complement=n-remainder;
        if (remainder>complement) ++q;
        else if (remainder==complement && ((q&1)||mag.any_below(shift))) ++q;
        else if (complement-remainder==1 && shift &&
                 ((mag.shifted_word(shift-1)&1)!=0) &&
                 ((q&1)||mag.any_below(shift-1))) ++q;
        const double value=std::scalbn(static_cast<double>(q),int(shift)-1074);
        return negative() ? -value : value;
    }
};

struct UInt320 {
    std::array<uint32_t,10> word{};
    UInt320() = default;
    explicit UInt320(uint64_t x) { word[0]=x; word[1]=x>>32; }
    explicit UInt320(const std::array<uint64_t,4> &x) {
        for (unsigned i=0;i<4;++i) { word[2*i]=x[i]; word[2*i+1]=x[i]>>32; }
    }
    void multiply(uint64_t factor) {
        const auto source=word;
        uint64_t carry=0;
        for (unsigned i=0;i<word.size();++i) {
            const uint64_t v=uint64_t(source[i])*uint32_t(factor)+carry;
            word[i]=v; carry=v>>32;
        }
        carry=0;
        for (unsigned i=0;i+1<word.size();++i) {
            const uint64_t v=uint64_t(source[i])*uint32_t(factor>>32)+word[i+1]+carry;
            word[i+1]=v; carry=v>>32;
        }
    }
    void shift_left(unsigned bits) {
        const auto source=word;
        const unsigned whole=bits/32, part=bits%32;
        for (unsigned i=0;i<word.size();++i) {
            word[i]=i>=whole ? source[i-whole]<<part : 0;
            if (part && i>whole) word[i]|=source[i-whole-1]>>(32-part);
        }
    }
    int compare(const UInt320 &other) const {
        for (int i=9;i>=0;--i)
            if (word[i]!=other.word[i]) return word[i]<other.word[i] ? -1 : 1;
        return 0;
    }
    void subtract(const UInt320 &other) {
        uint64_t borrow=0;
        for (unsigned i=0;i<word.size();++i) {
            const uint64_t sub=uint64_t(other.word[i])+borrow;
            const uint64_t value=word[i];
            word[i]=uint32_t(value-sub);
            borrow=value<sub;
        }
    }
    void shift_right_one() {
        for (unsigned i=0;i+1<word.size();++i)
            word[i]=(word[i]>>1)|(word[i+1]<<31);
        word.back()>>=1;
    }
    int highest() const {
        int i=int(word.size())-1;
        while (i>=0 && !word[i]) --i;
        if (i<0) return -1;
        unsigned bits=0, value=word[i];
        for (unsigned step:{16U,8U,4U,2U,1U})
            if (value>>step) { value>>=step; bits+=step; }
        return 32*i+int(bits);
    }
};

inline const UInt320 &decimal_power(unsigned scale) {
    static const auto powers=[] {
        std::array<UInt320,39> result;
        result[0]=UInt320(1);
        for (unsigned i=1;i<result.size();++i) { result[i]=result[i-1]; result[i].multiply(10); }
        return result;
    }();
    return powers[scale];
}

template<class Integer>
inline double integer_ratio(const Integer &numerator, uint64_t count, unsigned scale) {
    if (!count) return NAN;
    if (numerator.is_zero()) return 0;
    if (count==1 && scale==0) return numerator.to_double();
    const bool negative=numerator.negative();
    const auto magnitude=negative ? numerator.negated() : numerator;
    const UInt320 num(magnitude.word);
    UInt320 denominator=decimal_power(scale);
    denominator.multiply(count);
    double candidate=std::fabs(numerator.to_double())/static_cast<double>(count)/std::pow(10.,scale);
    auto compare_midpoint=[&](uint64_t coefficient,int exponent) {
        UInt320 left=num, right=denominator;
        right.multiply(coefficient);
        if (exponent<0) left.shift_left(-exponent);
        else right.shift_left(exponent);
        return left.compare(right);
    };
    for (;;) {
        uint64_t bits;
        std::memcpy(&bits,&candidate,sizeof bits);
        const int exponent=int((bits>>52)&2047)-1023;
        const uint64_t m=(bits&0xfffffffffffffULL)|(1ULL<<52);
        const bool power_two=m==(1ULL<<52), odd=(bits&1)!=0;
        const int lower=compare_midpoint(power_two ? 4*m-1 : 2*m-1,
                                        exponent-(power_two ? 54 : 53));
        if (lower<0 || (lower==0 && odd)) {
            candidate=std::nextafter(candidate,0.);
            continue;
        }
        const int upper=compare_midpoint(2*m+1,exponent-53);
        if (upper>0 || (upper==0 && odd)) {
            candidate=std::nextafter(candidate,INFINITY);
            continue;
        }
        return negative ? -candidate : candidate;
    }
}

// Round the exact binary64 product N*p/100 to a row count, half ties upward.
inline uint64_t sample_count(uint64_t n, double percentage) {
    if (!n || percentage<=0) return 0;
    if (percentage>=100) return n;
    uint64_t bits;
    std::memcpy(&bits,&percentage,sizeof bits);
    const unsigned field=(bits>>52)&2047;
    const int exponent=field ? int(field)-1023-52 : -1074;
    if (exponent < -128) return 0;
    const uint64_t m=(bits&0xfffffffffffffULL)|(field ? 1ULL<<52 : 0);
    UInt320 numerator(n), denominator(100);
    numerator.multiply(m);
    denominator.shift_left(unsigned(-exponent));
    uint64_t quotient=0;
    int bit=std::min(63,numerator.highest()-denominator.highest());
    if (bit>=0) {
        UInt320 shifted=denominator;
        shifted.shift_left(unsigned(bit));
        for (;bit>=0;--bit) {
            if (numerator.compare(shifted)>=0) {
                numerator.subtract(shifted);
                quotient|=1ULL<<bit;
            }
            shifted.shift_right_one();
        }
    }
    numerator.shift_left(1);
    if (numerator.compare(denominator)>=0) ++quotient;
    return quotient;
}

inline double round_units(double value, double unit) {
    if (!std::isfinite(value) || !std::isfinite(unit)) return NAN;
    if (unit==0) return value;
    const double remainder=std::remainder(value,unit);
    const bool tie=std::fabs(remainder)==std::fabs(unit)-std::fabs(remainder);
    const double result=tie && std::signbit(remainder)==std::signbit(unit)
                            ? value+remainder : value-remainder;
    return result==0 ? 0 : result;
}

inline double positive_mod(double value, double unit) {
    if (!std::isfinite(value) || !std::isfinite(unit) || unit<=0) return NAN;
    const double remainder=std::fmod(value,unit);
    return remainder==0 ? 0 : (remainder<0 ? remainder+unit : remainder);
}

} // namespace parqit::numeric
