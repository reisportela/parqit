#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>
#include <cassert>

namespace parqit::statistics::exact_covariance {

inline std::pair<uint64_t,uint64_t> multiply64(uint64_t a, uint64_t b) {
    const uint64_t a0=uint32_t(a), a1=a>>32, b0=uint32_t(b), b1=b>>32;
    const uint64_t p0=a0*b0, t=a1*b0+(p0>>32);
    const uint64_t middle=uint32_t(t)+a0*b1;
    return {(middle<<32)|uint32_t(p0), a1*b1+(t>>32)+(middle>>32)};
}

template<unsigned W> struct UInt {
    std::array<uint64_t,W> word{};
    UInt() = default;
    explicit UInt(uint64_t value) { word[0]=value; }
    template<unsigned M> explicit UInt(const UInt<M> &other) {
        static_assert(W>=M);
        for (unsigned i=0;i<M;++i) word[i]=other.word[i];
    }
    void add_at(unsigned i, uint64_t value, bool subtract=false) {
        if (!value) return;
        assert(i<W);
        const uint64_t old=word[i];
        word[i]=subtract ? old-value : old+value;
        bool carry=subtract ? old<value : word[i]<old;
        while (carry && ++i<W) {
            const uint64_t before=word[i];
            word[i]=subtract ? before-1 : before+1;
            carry=subtract ? before==0 : word[i]==0;
        }
    }
    void inject(uint64_t value, unsigned shift, bool negative=false) {
        const unsigned i=shift/64, offset=shift%64;
        add_at(i,value<<offset,negative);
        if (offset) add_at(i+1,value>>(64-offset),negative);
    }
    void add(const UInt &other) {
        uint64_t carry=0;
        for (unsigned i=0;i<W;++i) {
            const uint64_t old=word[i], a=old+other.word[i], b=a+carry;
            carry=(a<old)|(b<a); word[i]=b;
        }
    }
    void negate() {
        for (auto &value:word) value=~value;
        add_at(0,1);
    }
    void subtract(const UInt &other) {
        uint64_t borrow=0;
        for (unsigned i=0;i<W;++i) {
            const uint64_t before=word[i], sub=other.word[i], difference=before-sub;
            word[i]=difference-borrow;
            borrow=(before<sub)|(difference<borrow);
        }
    }
    bool negative() const { return (word.back()>>63)!=0; }
    UInt magnitude() const { UInt result=*this; if (negative()) result.negate(); return result; }
    int compare(const UInt &other) const {
        for (int i=int(W)-1;i>=0;--i)
            if (word[i]!=other.word[i]) return word[i]<other.word[i] ? -1 : 1;
        return 0;
    }
    int highest() const {
        int i=int(W)-1;
        while (i>=0 && !word[i]) --i;
        if (i<0) return -1;
        unsigned bits=0; uint64_t value=word[i];
        for (unsigned step:{32U,16U,8U,4U,2U,1U})
            if (value>>step) { value>>=step; bits+=step; }
        return i*64+int(bits);
    }
    uint64_t shifted_word(unsigned shift) const {
        const unsigned i=shift/64, offset=shift%64;
        if (i>=W) return 0;
        uint64_t result=word[i]>>offset;
        if (offset && i+1<W) result|=word[i+1]<<(64-offset);
        return result;
    }
    void shift_left(unsigned shift) {
        assert(highest()<0 || highest()+int(shift)<int(W*64));
        const auto old=word;
        const unsigned whole=shift/64, bits=shift%64;
        for (unsigned i=0;i<W;++i) {
            word[i]=i>=whole ? old[i-whole]<<bits : 0;
            if (bits && i>whole) word[i]|=old[i-whole-1]>>(64-bits);
        }
    }
    void multiply(uint64_t value) {
        uint64_t carry=0;
        unsigned first=0,last=W;
        while (first<last && !word[first]) ++first;
        while (last>first && !word[last-1]) --last;
        for (unsigned i=first;i<last;++i) {
            const auto p=multiply64(word[i],value);
            word[i]=p.first+carry;
            carry=p.second+(word[i]<p.first);
        }
        if (last<W) word[last]=carry;
    }
    template<unsigned M> UInt<W+M> product(const UInt<M> &other) const {
        UInt<W+M> result;
        unsigned first=0,last=W,ofirst=0,olast=M;
        if constexpr (W>8) {
            while (first<last && !word[first]) ++first;
            while (last>first && !word[last-1]) --last;
        }
        if constexpr (M>8) {
            while (ofirst<olast && !other.word[ofirst]) ++ofirst;
            while (olast>ofirst && !other.word[olast-1]) --olast;
        }
        for (unsigned i=first;i<last;++i) {
            if (!word[i]) continue;
            for (unsigned j=ofirst;j<olast;++j) {
                if (!other.word[j]) continue;
                const auto p=multiply64(word[i],other.word[j]);
                result.add_at(i+j,p.first);
                result.add_at(i+j+1,p.second);
            }
        }
        return result;
    }
    double normalized() const {
        const int h=highest();
        if (h<0) return 0;
        const unsigned shift=h>52 ? unsigned(h-52) : 0;
        return std::scalbn(double(shifted_word(shift)),int(shift)-h);
    }
    unsigned trailing() const {
        unsigned i=0;
        while (i<W && !word[i]) ++i;
        if (i==W) return W*64;
        uint64_t value=word[i]; unsigned count=0;
        for (unsigned step:{32U,16U,8U,4U,2U,1U}) {
            if ((value&((1ULL<<step)-1))==0) { value>>=step; count+=step; }
        }
        return 64*i+count;
    }
    template<unsigned M> UInt<M> compact(unsigned shift) const {
        assert(highest()-int(shift)<int(M*64));
        UInt<M> out;
        for (unsigned i=0;i<M;++i) out.word[i]=shifted_word(shift+64*i);
        return out;
    }
};

template<unsigned W>
int compare_scaled(UInt<W> left, int le, UInt<W> right, int re) {
    const int lh=left.highest(), rh=right.highest();
    if (lh<0 || rh<0) return (lh>=0)-(rh>=0);
    if (lh+le!=rh+re) return lh+le<rh+re ? -1 : 1;
    if (le>re) left.shift_left(unsigned(le-re));
    else right.shift_left(unsigned(re-le));
    return left.compare(right);
}

template<unsigned W> double sqrt_ratio(const UInt<W> &numerator, const UInt<W> &denominator, int scale=0) {
    if (numerator.highest()<0) return 0;
    assert(denominator.highest()>=0);
    const int exponent=numerator.highest()-denominator.highest()+scale;
    const bool odd=exponent%2!=0;
    double candidate=std::scalbn(std::sqrt(numerator.normalized()/denominator.normalized()*(odd ? 2 : 1)),
                                  (exponent-(odd ? 1 : 0))/2);
    const UInt<W+2> left(numerator);
    auto midpoint=[&](uint64_t coefficient, int power) {
        const auto squared=UInt<1>(coefficient).product(UInt<1>(coefficient));
        const auto right=denominator.product(squared);
        return compare_scaled(left,scale,right,2*power);
    };
    for (;;) {
        uint64_t bits;
        std::memcpy(&bits,&candidate,sizeof bits);
        const unsigned field=unsigned((bits>>52)&2047);
        const bool uneven=(bits&1)!=0;
        if (field==0) {
            if (bits) {
                const int lower=midpoint(2*bits-1,-1075);
                if (lower<0 || (lower==0 && uneven)) { candidate=std::nextafter(candidate,0.); continue; }
            }
            const int upper=midpoint(2*bits+1,-1075);
            if (upper>0 || (upper==0 && uneven)) { candidate=std::nextafter(candidate,INFINITY); continue; }
            return candidate;
        }
        assert(field<2047);
        const uint64_t mantissa=(bits&0xfffffffffffffULL)|(1ULL<<52);
        const int power=int(field)-1023-52;
        const bool boundary=mantissa==(1ULL<<52) && field>1;
        const int lower=midpoint(boundary ? 4*mantissa-1 : 2*mantissa-1,power-(boundary ? 2 : 1));
        if (lower<0 || (lower==0 && uneven)) { candidate=std::nextafter(candidate,0.); continue; }
        const int upper=midpoint(2*mantissa+1,power-1);
        if (upper>0 || (upper==0 && uneven)) { candidate=std::nextafter(candidate,INFINITY); continue; }
        return candidate;
    }
}

template<unsigned W> double ratio(const UInt<W> &numerator, const UInt<W> &denominator, int scale=0) {
    if (numerator.highest()<0) return 0;
    assert(denominator.highest()>=0);
    double candidate=std::scalbn(numerator.normalized()/denominator.normalized(),
                                 numerator.highest()-denominator.highest()+scale);
    const UInt<W+1> left(numerator);
    auto midpoint=[&](uint64_t coefficient, int power) {
        return compare_scaled(left,scale,denominator.product(UInt<1>(coefficient)),power);
    };
    for (;;) {
        uint64_t bits;
        std::memcpy(&bits,&candidate,sizeof bits);
        const unsigned field=unsigned((bits>>52)&2047);
        const bool uneven=(bits&1)!=0;
        if (field==2047) {
            if (midpoint((1ULL<<54)-1,970)<0) { candidate=std::nextafter(candidate,0.); continue; }
            return candidate;
        }
        if (field==0) {
            if (bits) {
                const int lower=midpoint(2*bits-1,-1075);
                if (lower<0 || (lower==0 && uneven)) { candidate=std::nextafter(candidate,0.); continue; }
            }
            const int upper=midpoint(2*bits+1,-1075);
            if (upper>0 || (upper==0 && uneven)) { candidate=std::nextafter(candidate,INFINITY); continue; }
            return candidate;
        }
        const uint64_t mantissa=(bits&0xfffffffffffffULL)|(1ULL<<52);
        const int power=int(field)-1023-52;
        const bool boundary=mantissa==(1ULL<<52) && field>1;
        const int lower=midpoint(boundary ? 4*mantissa-1 : 2*mantissa-1,power-(boundary ? 2 : 1));
        if (lower<0 || (lower==0 && uneven)) { candidate=std::nextafter(candidate,0.); continue; }
        const int upper=midpoint(2*mantissa+1,power-1);
        if (upper>0 || (upper==0 && uneven)) { candidate=std::nextafter(candidate,INFINITY); continue; }
        return candidate;
    }
}

template<unsigned W> std::pair<double,double> fraction_results(const UInt<W> &n,const UInt<W> &d,int scale) {
    if (n.compare(d)==0 && scale%2==0)
        return {std::scalbn(1.,scale),std::scalbn(1.,scale/2)};
    return {ratio(n,d,scale),sqrt_ratio(n,d,scale)};
}
template<unsigned W> std::pair<double,double> compact_fraction(const UInt<W> &n,const UInt<W> &d,int scale) {
    const unsigned tn=n.trailing(),td=d.trailing();
    const int bits=std::max(n.highest()-int(tn),d.highest()-int(td))+1;
    const int power=scale+int(tn)-int(td);
    if (bits<=128) return fraction_results(n.template compact<2>(tn),d.template compact<2>(td),power);
    if (bits<=256) return fraction_results(n.template compact<4>(tn),d.template compact<4>(td),power);
    if (bits<=512) return fraction_results(n.template compact<8>(tn),d.template compact<8>(td),power);
    return fraction_results(n,d,scale);
}

struct Term {
    UInt<2> coefficient;
    unsigned shift=0;
    bool negative=false;
    explicit Term(double value) {
        uint64_t bits;
        std::memcpy(&bits,&value,sizeof bits);
        const unsigned exponent=(bits>>52)&2047;
        assert(exponent<2047);
        coefficient.word[0]=(bits&0xfffffffffffffULL)|(exponent ? 1ULL<<52 : 0);
        shift=exponent ? exponent-1 : 0;
        negative=(bits>>63)!=0;
    }
    template<class Integer, std::enable_if_t<!std::is_arithmetic<Integer>::value, int> = 0>
    explicit Term(const Integer &value) {
        negative=value.negative();
        const auto magnitude=negative ? value.negated() : value;
        assert(!magnitude.word[2] && !magnitude.word[3]);
        coefficient.word[0]=magnitude.word[0]; coefficient.word[1]=magnitude.word[1];
    }
    void add_to(UInt<34> &sum) const {
        for (unsigned i=0;i<2;++i) if (coefficient.word[i]) sum.inject(coefficient.word[i],shift+64*i,negative);
    }
    void product_to(const Term &other, UInt<68> &sum) const {
        const auto value=coefficient.product(other.coefficient);
        for (unsigned i=0;i<4;++i)
            if (value.word[i]) sum.inject(value.word[i],shift+other.shift+64*i,negative!=other.negative);
    }
};

struct CorrelationResult {
    double rho=NAN, sine=NAN;
    bool perfect=false, invalid=false;
};

struct Correlation {
    uint64_t n=0;
    UInt<34> sx,sy;
    UInt<68> sxx,syy,sxy;
    template<class X,class Y> void add(const X &x,const Y &y) {
        const Term a(x),b(y);
        a.add_to(sx); b.add_to(sy);
        a.product_to(a,sxx); b.product_to(b,syy); a.product_to(b,sxy);
        ++n;
    }
    void combine(const Correlation &other) {
        n+=other.n; sx.add(other.sx); sy.add(other.sy);
        sxx.add(other.sxx); syy.add(other.syy); sxy.add(other.sxy);
    }
    CorrelationResult result() const {
        if (n<2) return {};
        auto a=sxx,b=syy,c=sxy;
        a.multiply(n); b.multiply(n); c.multiply(n);
        const auto mx=sx.magnitude(),my=sy.magnitude();
        a.subtract(mx.product(mx)); b.subtract(my.product(my));
        auto xy=mx.product(my);
        if (sx.negative()!=sy.negative()) xy.negate();
        c.subtract(xy);
        if (a.negative() || b.negative()) return {NAN,NAN,false,true};
        if (a.highest()<0 || b.highest()<0) return {};
        const auto denominator=a.product(b), numerator=c.magnitude().product(c.magnitude());
        if (denominator.compare(numerator)<0) return {NAN,NAN,false,true};
        auto determinant=denominator;
        determinant.subtract(numerator);
        return {std::copysign(sqrt_ratio(numerator,denominator),c.negative() ? -1. : 1.),
                sqrt_ratio(determinant,denominator),determinant.highest()<0,false};
    }
    double value() const { return result().rho; }
};
}

static_assert(std::is_trivially_copyable<parqit::statistics::exact_covariance::Correlation>::value);
static_assert(alignof(parqit::statistics::exact_covariance::Correlation)==alignof(uint64_t));
static_assert(sizeof(parqit::statistics::exact_covariance::Correlation)==2184);
