#pragma once
#include "engine/statistics.hpp"
#include "engine/statistics_math.hpp"
#include "engine/covariance_math.hpp"

namespace parqit::statistics {

struct MassThreshold {
    exact_covariance::UInt<34> mass;
    uint64_t rank=0;
    bool configured=false, invalid=false;
    void add(double weight,uint64_t k) {
        if (configured && rank!=k) invalid=true;
        configured=true; rank=k;
        exact_covariance::Term(weight).add_to(mass);
    }
    void combine(const MassThreshold &other) {
        if (!other.configured) return;
        if (configured && rank!=other.rank) invalid=true;
        configured=true; rank=other.rank; invalid|=other.invalid; mass.add(other.mass);
    }
    bool reached() const {
        if (!configured) return false;
        exact_covariance::UInt<34> target;
        target.inject(rank,1074);
        return mass.compare(target)>=0;
    }
};

struct ScaledMass {
    exact_covariance::UInt<34> mass;
    void add(double weight) { exact_covariance::Term(weight).add_to(mass); }
    void combine(const ScaledMass &other) { mass.add(other.mass); }
    std::pair<double,int> parts() const {
        const int highest=mass.highest();
        if (highest<0) return {0,0};
        return {exact_covariance::ratio(mass,exact_covariance::UInt<34>(1),-highest),highest-1074};
    }
};

/* Ordered blocks combine only when their value ranges do not overlap.
 * The window sorter supplies those blocks and can spill the input. */
struct OrderedGini {
    exact_covariance::UInt<34> mass;
    exact_covariance::UInt<68> total;
    exact_covariance::UInt<102> pairs;
    Number low,high;
    bool present=false, invalid=false;
    static exact_covariance::UInt<102> product(const exact_covariance::UInt<68> &s,
                                                const exact_covariance::UInt<34> &w) {
        auto value=s.magnitude().product(w);
        if (s.negative()) value.negate();
        return value;
    }
    void add(const Number &x,double weight) {
        using namespace exact_covariance;
        if (present && compare(high,x)>0) { invalid=true; return; }
        Int256 integer; integer.word=x.integer;
        Term value=x.integral ? Term(integer) : Term(x.real);
        if (x.integral) value.shift=1074;
        const Term w(weight);
        UInt<34> row_mass; w.add_to(row_mass);
        UInt<68> row_total; value.product_to(w,row_total);
        auto cross=product(row_total,mass);
        cross.subtract(product(total,row_mass));
        if (cross.negative()) { invalid=true; return; }
        pairs.add(cross); mass.add(row_mass); total.add(row_total);
        if (!present) low=x;
        high=x; present=true;
    }
    void combine(const OrderedGini &other) {
        if (!other.present) return;
        if (!present) { *this=other; return; }
        const bool forward=compare(high,other.low)<=0;
        const bool reverse=compare(other.high,low)<=0;
        if (!forward && !reverse) { invalid=true; return; }
        auto cross=forward ? product(other.total,mass) : product(total,other.mass);
        cross.subtract(forward ? product(total,other.mass) : product(other.total,mass));
        if (cross.negative()) { invalid=true; return; }
        pairs.add(other.pairs); pairs.add(cross); mass.add(other.mass); total.add(other.total);
        if (reverse) low=other.low;
        if (forward) high=other.high;
        invalid|=other.invalid;
    }
    double result() const {
        if (!present || total.highest()<0) return NAN;
        const auto denominator=total.magnitude().product(mass);
        return std::copysign(exact_covariance::ratio(pairs,denominator),total.negative() ? -1. : 1.);
    }
};

/* Fixed-size exact dyadic sums. Products are accumulated before rounding;
 * Decimal/integer sources retain their integer coefficient and scale. */
template<unsigned Order> struct WeightedPowers {};
template<> struct WeightedPowers<2> { exact_covariance::UInt<102> s2; };
template<> struct WeightedPowers<4> {
    exact_covariance::UInt<102> s2;
    exact_covariance::UInt<136> s3;
    exact_covariance::UInt<170> s4;
};
struct WeightedResult {
    uint64_t n=0;
    double mass=0, sum=0, sum_aw=0, mean=NAN, variance=NAN, variance_aw=NAN;
    double sd=NAN, sd_aw=NAN, skewness=NAN, kurtosis=NAN;
    double sebinomial=NAN, sepoisson=NAN;
    bool invalid=false;
};
template<unsigned Order> struct WeightedMoments : WeightedPowers<Order> {
    uint64_t n=0;
    unsigned scale=0;
    bool binary=true, nonnegative=true;
    exact_covariance::UInt<34> s0;
    exact_covariance::UInt<68> s1;

    template<unsigned W,unsigned M>
    static void inject(exact_covariance::UInt<W> &sum,
                       const exact_covariance::UInt<M> &value,
                       unsigned shift,bool negative) {
        for (unsigned i=0;i<M;++i)
            if (value.word[i]) sum.inject(value.word[i],shift+64*i,negative);
    }
    void add(const Number &x,double weight) {
        using namespace exact_covariance;
        ++n;
        scale=x.scale;
        Int256 integer; integer.word=x.integer;
        nonnegative &= x.integral ? !integer.negative() : x.real>=0;
        if (binary) {
            if (x.integral) {
                Int256 one=Int256::unsigned128(1);
                for (unsigned i=0;i<x.scale;++i) one=one.multiplied(10);
                binary=integer.is_zero() || integer.compare(one)==0;
            } else binary=x.real==0 || x.real==1;
        }
        Term value=x.integral ? Term(integer) : Term(x.real);
        if (x.integral) value.shift=1074;
        const Term w(weight);
        w.add_to(s0);
        const auto wx=w.coefficient.product(value.coefficient);
        inject(s1,wx,w.shift+value.shift,value.negative);
        if constexpr (Order>=2) {
            const auto wx2=wx.product(value.coefficient);
            inject(this->s2,wx2,w.shift+2*value.shift,false);
            if constexpr (Order==4) {
                const auto wx3=wx2.product(value.coefficient);
                inject(this->s3,wx3,w.shift+3*value.shift,value.negative);
                inject(this->s4,wx3.product(value.coefficient),w.shift+4*value.shift,false);
            }
        }
    }
    void combine(const WeightedMoments &other) {
        if (!other.n) return;
        n+=other.n; scale=other.scale;
        binary &= other.binary; nonnegative &= other.nonnegative;
        s0.add(other.s0); s1.add(other.s1);
        if constexpr (Order>=2) {
            this->s2.add(other.s2);
            if constexpr (Order==4) { this->s3.add(other.s3); this->s4.add(other.s4); }
        }
    }
    WeightedResult result(bool errors=false) const {
        using namespace exact_covariance;
        WeightedResult out; out.n=n;
        if (!n) return out;
        const UInt<68> one(1), mass(s0);
        out.mass=ratio(mass,one,-1074);
        UInt<68> decimal(1);
        for (unsigned i=0;i<scale;++i) decimal.multiply(10);
        const auto magnitude=s1.magnitude();
        out.sum=std::copysign(ratio(magnitude,decimal,-2148),s1.negative() ? -1. : 1.);
        auto mean_d=mass;
        for (unsigned i=0;i<scale;++i) mean_d.multiply(10);
        out.mean=std::copysign(ratio(magnitude,mean_d,-1074),s1.negative() ? -1. : 1.);
        auto aw_sum=magnitude; aw_sum.multiply(n);
        out.sum_aw=std::copysign(ratio(aw_sum,mean_d,-1074),s1.negative() ? -1. : 1.);
        if (errors) {
            if (binary) {
                auto ones=magnitude;
                for (unsigned k=0;k<scale;++k) divide10(ones);
                const auto successes=ones.template compact<34>(1074);
                auto failures=s0; failures.subtract(successes);
                const UInt<102> numerator(successes.product(failures));
                out.sebinomial=sqrt_ratio(numerator,s0.product(s0).product(s0),1074);
            }
            if (nonnegative) {
                auto total=magnitude;
                for (unsigned k=0;k<scale;++k) divide10(total);
                auto rounded=total.template compact<68>(2148);
                if (total.shifted_word(2147)&1) rounded.add_at(0,1);
                out.sepoisson=sqrt_ratio(rounded,UInt<68>(s0.product(s0)),2148);
            }
        }
        if constexpr (Order>=2) {
            auto a2=this->s2.product(s0);
            a2.subtract(magnitude.product(magnitude));
            if (a2.negative()) { out.invalid=true; return out; }
            const auto square_mass=s0.product(s0);
            auto aw_num=a2; aw_num.multiply(n);
            UInt<136> aw_den(square_mass); aw_den.multiply(n ? n-1 : 0);
            for (unsigned i=0;i<2*scale;++i) aw_den.multiply(10);
            if (n>1) {
                const auto aw=compact_fraction(aw_num,aw_den,-2148);
                out.variance_aw=aw.first; out.sd_aw=aw.second;
            }
            UInt<34> one_weight; one_weight.inject(1,1074);
            if (s0.compare(one_weight)>0) {
                auto mass_minus_one=s0; mass_minus_one.subtract(one_weight);
                UInt<136> denominator(s0.product(mass_minus_one));
                for (unsigned i=0;i<2*scale;++i) denominator.multiply(10);
                const auto var=compact_fraction(a2,denominator,-2148);
                out.variance=var.first; out.sd=var.second;
            }
            if constexpr (Order==4) {
                if (a2.highest()<0) return out;
                const auto m=magnitude;
                const auto m2=m.product(m);
                auto a3=this->s3.product(square_mass);
                auto cross=m.product(this->s2).product(s0); cross.multiply(3);
                if (s1.negative()) cross.negate();
                a3.subtract(cross);
                auto cube=m2.product(m); cube.multiply(2);
                if (s1.negative()) cube.negate();
                a3.add(cube);
                const auto a2sq=a2.product(a2);
                const auto numerator=a3.magnitude().product(a3.magnitude());
                const auto denominator=a2sq.product(a2);
                if (numerator.highest()-denominator.highest()<2046)
                    out.skewness=std::copysign(sqrt_ratio(numerator,denominator),a3.negative() ? -1. : 1.);
                auto a4=this->s4.product(square_mass).product(s0);
                auto c3=m.product(this->s3.magnitude()).product(square_mass); c3.multiply(4);
                if (s1.negative()!=this->s3.negative()) c3.negate();
                a4.subtract(c3);
                auto c2=m2.product(this->s2).product(s0); c2.multiply(6); a4.add(c2);
                auto c4=m2.product(m2); c4.multiply(3); a4.subtract(c4);
                out.kurtosis=ratio(a4,a2sq);
            }
        }
        return out;
    }
private:
    template<unsigned W> static void divide10(exact_covariance::UInt<W> &value) {
        uint64_t rem=0;
        for (int i=int(W)-1;i>=0;--i) {
            const uint64_t a=(rem<<32)|(value.word[i]>>32);
            const uint64_t hi=a/10; rem=a%10;
            const uint64_t b=(rem<<32)|uint32_t(value.word[i]);
            value.word[i]=(hi<<32)|(b/10); rem=b%10;
        }
    }
};
} // namespace parqit::statistics
