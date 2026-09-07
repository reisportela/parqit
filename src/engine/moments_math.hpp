#pragma once
#include "engine/covariance_math.hpp"

namespace parqit::statistics::exact_covariance {
template<unsigned Order> struct Powers;
template<> struct Powers<2> { UInt<68> s2; };
template<> struct Powers<4> { UInt<68> s2; UInt<102> s3; UInt<136> s4; };
struct MomentResult { double sd=NAN, variance=NAN, skewness=NAN, kurtosis=NAN; bool invalid=false; };

template<unsigned Order,class Total> struct Moments {
    Total total;
    Powers<Order> powers;
    template<unsigned W,unsigned M>
    static void inject(UInt<W> &sum,const UInt<M> &value,unsigned shift,bool negative) {
        for (unsigned i=0;i<M;++i) if (value.word[i]) sum.inject(value.word[i],shift+64*i,negative);
    }
    template<class X> void add(const X &value,unsigned scale=0) {
        const Term term(value);
        if constexpr (std::is_arithmetic<X>::value) total.add(static_cast<double>(value));
        else total.add(value,scale);
        const auto p2=term.coefficient.product(term.coefficient);
        inject(powers.s2,p2,2*term.shift,false);
        if constexpr (Order==4) {
            inject(powers.s3,p2.product(term.coefficient),3*term.shift,term.negative);
            inject(powers.s4,p2.product(p2),4*term.shift,false);
        }
    }
    void combine(const Moments &other) {
        total.combine(other.total); powers.s2.add(other.powers.s2);
        if constexpr (Order==4) { powers.s3.add(other.powers.s3); powers.s4.add(other.powers.s4); }
    }
    MomentResult result() const {
        MomentResult out;
        const uint64_t n=total.n;
        if (n<2) return out;
        UInt<34> sum;
        if (total.integral) {
            for (auto &word:sum.word) word=total.sum.integer.negative() ? UINT64_MAX : 0;
            for (unsigned i=0;i<4;++i) sum.word[i]=total.sum.integer.word[i];
        } else sum.word=total.sum.floating.word;
        const auto magnitude=sum.magnitude();
        const auto square=magnitude.product(magnitude);
        auto a2=powers.s2;
        a2.multiply(n); a2.subtract(square);
        if (a2.negative()) return {NAN,NAN,NAN,NAN,true};
        if (a2.highest()<0) { out.sd=out.variance=0; return out; }
        UInt<68> denominator(n);
        denominator.multiply(n-1);
        for (unsigned i=0;i<2*total.decimal_scale;++i) denominator.multiply(10);
        const int unit=total.integral ? 0 : -2148;
        const auto values=compact_fraction(a2,denominator,unit);
        out.variance=values.first; out.sd=values.second;
        if constexpr (Order==4) {
            auto a3=powers.s3;
            a3.multiply(n); a3.multiply(n);
            auto cross=magnitude.product(powers.s2);
            if (sum.negative()) cross.negate();
            cross.multiply(n); cross.multiply(3); a3.subtract(cross);
            auto cube=square.product(magnitude);
            if (sum.negative()) cube.negate();
            cube.multiply(2); a3.add(cube);
            const auto a2square=a2.product(a2);
            const auto a3square=a3.magnitude().product(a3.magnitude());
            out.skewness=std::copysign(sqrt_ratio(a3square,a2square.product(a2)),a3.negative() ? -1. : 1.);
            auto a4=powers.s4;
            a4.multiply(n); a4.multiply(n); a4.multiply(n);
            auto cross3=magnitude.product(powers.s3.magnitude());
            if (sum.negative()!=powers.s3.negative()) cross3.negate();
            cross3.multiply(n); cross3.multiply(n); cross3.multiply(4); a4.subtract(cross3);
            auto cross2=square.product(powers.s2);
            cross2.multiply(n); cross2.multiply(6); a4.add(cross2);
            auto fourth=square.product(square); fourth.multiply(3); a4.subtract(fourth);
            if (a4.negative() || a4.compare(a2square)<0) return {NAN,NAN,NAN,NAN,true};
            auto pearson=a4;
            pearson.subtract(a2square);
            if (pearson.product(a2).compare(a3square)<0) return {NAN,NAN,NAN,NAN,true};
            out.kurtosis=ratio(a4,a2square);
        }
        return out;
    }
    double sd() const { return result().sd; }
    double variance() const { return result().variance; }
    double skewness() const { static_assert(Order==4); return result().skewness; }
    double kurtosis() const { static_assert(Order==4); return result().kurtosis; }

};
}
