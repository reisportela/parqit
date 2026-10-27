#include "engine/view.hpp"
#include "engine/exprtrans.hpp"
#include "engine/session.hpp"
#include "engine/sanitize.hpp"
#include "engine/typemap.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <iomanip>
#include <locale>
#include <regex>
#include <set>
#include <sstream>

namespace parqit {
namespace {
std::string number(double x) { std::ostringstream s; s.imbue(std::locale::classic()); s<<std::setprecision(17)<<x; return s.str(); }
void replace_all(std::string &s,const std::string &from,const std::string &to) {
    size_t p=0; while ((p=s.find(from,p))!=std::string::npos) { s.replace(p,from.size(),to); p+=to.size(); }
}
std::string statistic(std::string s) {
    const std::map<std::string,std::string> aliases={
        {"sem","semean"},{"seb","sebinomial"},{"sep","sepoisson"},
        {"var","variance"},{"skew","skewness"},{"kurt","kurtosis"},
        {"gini dropneg","gini_dropneg"},{"gini keepneg","gini_keepneg"}};
    const auto i=aliases.find(s); if (i!=aliases.end()) return i->second;
    const std::vector<std::pair<std::string,size_t>> prefixes={{"variance",3},{"max",2},{"min",2},
        {"range",1},{"count",2},{"median",3},{"percent",4},{"semean",3},{"sebinomial",3},
        {"sepoisson",3},{"nunique",5},{"nmissing",5},{"skewness",2},{"kurtosis",1},{"rawsum",5}};
    for (const auto &p:prefixes) if (s.size()>=p.second && p.first.rfind(s,0)==0) return p.first;
    return s;
}
bool percentile(const std::string &s,double *p) {
    if (s=="median") { *p=50; return true; }
    if (s.size()<2 || s[0]!='p') return false;
    std::istringstream in(s.substr(1)); in.imbue(std::locale::classic());
    in>>std::noskipws>>*p;
    return !in.fail() && in.peek()==std::char_traits<char>::eof() && std::isfinite(*p) && *p>0 && *p<100;
}
bool selection(const std::string &s,long long *k,bool *raw) {
    *raw=s.rfind("rawselect",0)==0;
    const std::string prefix=*raw ? "rawselect" : "select";
    if (s.rfind(prefix,0)!=0) return false;
    size_t n=0;
    try { *k=std::stoll(s.substr(prefix.size()),&n); } catch (...) { return false; }
    return n==s.size()-prefix.size() && *k!=0 && *k!=LLONG_MIN;
}
bool gini_stat(const std::string &s) { return s=="gini" || s=="gini_dropneg" || s=="gini_keepneg"; }
std::string fold_name(std::string s) { for (char &c:s) if (c>='A' && c<='Z') c=char(c-'A'+'a'); return s; }
std::string wild_target(const std::string &pattern,const std::string &target,const std::string &source) {
    std::string re="^";
    for (char c:pattern) {
        if (c=='*') re+="(.*)";
        else if (c=='?') re+="(.)";
        else if (c=='#') re+="([0-9]+)";
        else { if (std::string(".[]{}()+^$|\\").find(c)!=std::string::npos) re+='\\'; re+=c; }
    }
    std::smatch match;
    if (!std::regex_match(source,match,std::regex(re+"$"))) return "";
    std::string out; size_t part=1;
    for (char c:target) {
        if (c=='*' || c=='?' || c=='#') { if (part>=match.size()) return ""; out+=match[part++].str(); }
        else out+=c;
    }
    return out;
}
std::string pretty_stat(const std::string &s) {
    const std::map<std::string,std::string> pretty={{"mean","Mean"},{"sum","Sum"},
        {"sd","St Dev."},{"count","Count"},{"freq","Group size"},{"variance","Variance"},
        {"geomean","Geometric mean"},{"nmissing","Missing count"},{"nunique","Unique count"},
        {"unique","Unique count"},{"cv","CV"},{"skewness","Skewness"},{"kurtosis","Kurtosis"},
        {"gini","Gini Coefficient"},{"gini_dropneg","Gini Coefficient (drop neg)"},
        {"gini_keepneg","Gini Coefficient (keep neg)"}};
    const auto it=pretty.find(s); return it==pretty.end() ? s : it->second;
}
std::string label_for(const View::CollapseSpec &sp,const ViewCol &source,const View::CollapseOptions &o) {
    std::string s=sp.stat; replace_all(s,"_"," ");
    if (o.labelformat.empty()) return "("+s+") "+source.exposed();
    std::string upper=s,title=s;
    for (char &c:upper) c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (!title.empty()) title[0]=static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
    auto pretty=o.pretty.find(sp.stat);
    if (pretty==o.pretty.end()) for (auto i=o.pretty.begin();i!=o.pretty.end();++i)
        if (statistic(i->first)==sp.stat) { pretty=i; break; }
    const std::string nice=pretty==o.pretty.end() || pretty->second=="#default#" ? pretty_stat(sp.stat) : pretty->second;
    auto render=[&](std::string value) {
        replace_all(value,"#stat:pretty#",nice); replace_all(value,"#stat#",s);
        replace_all(value,"#Stat#",title); replace_all(value,"#STAT#",upper); return value;
    };
    const std::string source_label=render(source.varlab.empty() ? source.exposed() : source.varlab);
    std::string out=o.labelformat;
    std::smatch m;
    const std::regex sub("#sourcelabel:([0-9]+):([0-9]+|\\.)#");
    while (std::regex_search(out,m,sub)) {
        size_t start=std::stoull(m[1].str()), count=m[2].str()=="." ? std::string::npos : std::stoull(m[2].str());
        std::vector<size_t> pos;
        for (size_t i=0;i<source_label.size();++i)
            if ((static_cast<unsigned char>(source_label[i])&192)!=128) pos.push_back(i);
        pos.push_back(source_label.size());
        const size_t a=start ? std::min(start-1,pos.size()-1) : 0;
        const size_t b=count==std::string::npos ? pos.size()-1 : std::min(a+count,pos.size()-1);
        out.replace(static_cast<size_t>(m.position()),static_cast<size_t>(m.length()),source_label.substr(pos[a],pos[b]-pos[a]));
    }
    replace_all(out,"#sourcelabel#",source_label);
    return render(out);
}
}

std::string View::collapse_extended(const std::vector<CollapseSpec> &input,
                                   const std::vector<std::string> &by,
                                   const CollapseOptions &o,bool statamissing) {
    static const std::set<std::string> ordinary={"mean","sum","rawsum","sd","semean","sebinomial","sepoisson",
        "median","iqr","count","percent","min","max","first","last","firstnm","lastnm","unique","nunique"};
    bool legacy=o.weight.empty() && o.first.empty() && o.last.empty() && !o.cw && !o.merge &&
        !o.replace && !o.noinit && !o.unsorted && !o.double_targets && !o.sumcheck &&
        !o.wildparse && !o.nods && o.freq.empty() && o.labelformat.empty() && !o.missing && o.rawstat.empty();
    if (legacy) {
        for (const auto &sp:input) {
            double p;
            legacy &= (ordinary.count(sp.stat) || percentile(sp.stat,&p)) && sp.source!="_all" &&
                sp.source.find_first_of("*?#-")==std::string::npos;
        }
        for (const auto &g:by) legacy &= g.empty() || (g[0]!='+' && g[0]!='-');
    }
    if (legacy) {
        if (!o.ifexpr.empty()) { const auto error=filter(o.ifexpr,false,statamissing); if (!error.empty()) return error; }
        return collapse(input,by);
    }
    std::map<std::string,std::string> destination_aliases;
    auto destination=[&](std::string name) {
        if (o.merge && !name.empty()) for (const auto &c:cols_)
            if (name==c.name || name==c.exposed()) { destination_aliases[c.name]=c.stata; return c.name; }
        return name;
    };
    const std::string frequency=destination(o.freq);
    std::vector<CollapseSpec> specs;
    for (const auto &s:input) {
        std::vector<std::string> sources;
        std::string error;
        if (o.wildparse && s.source.find('#')!=std::string::npos) {
            for (const auto &c:cols_)
                if (!wild_target(s.source,s.target.empty() ? s.source : s.target,c.exposed()).empty()) sources.push_back(c.name);
            if (sources.empty()) return "collapse: source pattern "+s.source+" matched no variables";
        } else error=expand_patterns({s.source},&sources);
        if (!error.empty()) return error;
        for (const auto &source:sources) {
            std::string target=s.target;
            if (o.wildparse && !target.empty() && target.find_first_of("*?#")!=std::string::npos) {
                target=wild_target(s.source,target,cols_[col_index(source)].exposed());
                if (target.empty()) return "collapse: incompatible source/target wildcard patterns";
            } else if (!target.empty() && sources.size()!=1)
                return "collapse: several source variables need wildparse or individual targets";
            specs.push_back({statistic(s.stat),destination(target),source});
        }
    }
    if (specs.empty()) return "collapse: nothing to compute";
    std::vector<std::string> groups;
    std::vector<bool> desc;
    std::vector<std::string> bytokens;
    for (const auto &key:by) {
        if (!o.nods) { bytokens.push_back(key); continue; }
        size_t start=0;
        for (size_t i=1;i<key.size();++i) if (key[i]=='+' || key[i]=='-') {
            bytokens.push_back(key.substr(start,i-start)); start=i;
        }
        bytokens.push_back(key.substr(start));
    }
    for (auto key:bytokens) {
        const bool reverse=!key.empty() && key[0]=='-';
        if (!key.empty() && (key[0]=='-' || key[0]=='+')) key.erase(0,1);
        std::vector<std::string> expanded;
        const auto e=expand_patterns({key},&expanded); if (!e.empty()) return e;
        for (const auto &name:expanded) {
            if (std::find(groups.begin(),groups.end(),name)!=groups.end()) return "collapse: repeated by variable "+name;
            groups.push_back(name); desc.push_back(reverse);
        }
    }
    bool simple=o.weight.empty() && o.first.empty() && o.last.empty() && !o.cw && !o.merge &&
        !o.replace && !o.noinit && !o.unsorted && !o.double_targets && !o.sumcheck &&
        o.freq.empty() && o.labelformat.empty() && !o.missing && o.rawstat.empty();
    for (const auto &sp:specs) {
        double p;
        simple &= ordinary.count(sp.stat) || percentile(sp.stat,&p);
    }
    for (bool d:desc) simple &= !d;
    if (simple) {
        if (!o.ifexpr.empty()) { const auto e=filter(o.ifexpr,false,statamissing); if (!e.empty()) return e; }
        return collapse(specs,groups);
    }
    const auto original_cols=cols_;
    const ExprSchema bindings=expr_schema();
    const auto original_sort=sort_;
    const std::string original_order=order_by_sql();
    if (o.replace && !o.merge) return "collapse: replace requires merge";
    if (o.noinit && !o.merge) return "collapse: noinit requires merge";
    if (o.weight.empty()!=o.weight_type.empty()) return "collapse: weight expression and type must be given together";
    if (!o.weight_type.empty() && o.weight_type!="fweight" && o.weight_type!="aweight" &&
        o.weight_type!="iweight" && o.weight_type!="pweight") return "collapse: invalid weight type";
    std::set<std::string> outputs;
    for (const auto &g:groups) outputs.insert(g);
    for (const auto &sp:specs) {
        const std::string target=sp.target.empty() ? sp.source : sp.target;
        for (const auto &other:outputs) {
            std::string a=target,b=other;
            std::transform(a.begin(),a.end(),a.begin(),[](unsigned char c){return std::tolower(c);});
            std::transform(b.begin(),b.end(),b.begin(),[](unsigned char c){return std::tolower(c);});
            if (a==b) return "collapse: duplicate output variable "+target;
        }
        outputs.insert(target);
        if (o.merge) { const auto error=ci_guard(target); if (!error.empty()) return error; }
        if (o.merge && col_index(target)>=0 && !o.replace)
            return "collapse: target "+target+" already exists; specify merge replace";
    }
    std::set<std::string> exposed;
    for (const auto &g:groups) exposed.insert(cols_[col_index(g)].exposed());
    for (const auto &sp:specs) {
        const std::string target=sp.target.empty() ? cols_[col_index(sp.source)].exposed() :
            destination_aliases.count(sp.target) && !destination_aliases.at(sp.target).empty() ? destination_aliases.at(sp.target) : sp.target;
        if (!exposed.insert(target).second) return "collapse: duplicate Stata output name "+target;
    }
    if (!frequency.empty()) {
        for (const auto &name:outputs) if (fold_name(name)==fold_name(frequency)) return "collapse: freq() conflicts with an output variable";
        if (exposed.count(o.freq)) return "collapse: freq() conflicts with a Stata output name";
        outputs.insert(frequency);
        if (o.merge) { const auto error=ci_guard(frequency); if (!error.empty()) return error; }
    }
    if (o.merge && !frequency.empty() && col_index(frequency)>=0 && !o.replace)
        return "collapse: freq target already exists; specify merge replace";
    std::set<std::string> raw_targets;
    if (!o.rawstat.empty()) {
        std::vector<std::string> targets,aliases;
        for (const auto &sp:specs) {
            targets.push_back(sp.target.empty() ? sp.source : sp.target);
            aliases.push_back(sp.target.empty() ? cols_[col_index(sp.source)].stata :
                destination_aliases.count(sp.target) ? destination_aliases.at(sp.target) : "");
        }
        std::vector<size_t> picked;
        const std::string error=resolve_varlist(o.rawstat,targets,aliases,varabbrev_,&picked);
        if (!error.empty()) return "collapse rawstat: "+error;
        for (size_t i:picked) raw_targets.insert(targets[i]);
    }

    auto helper=[&](const std::string &hint) { return fresh_helper(hint,outputs); };
    // User expressions bind to the original schema. Temporary plan columns
    // must neither resolve an unknown name nor change an abbreviation.
    auto generate=[&](const std::string &name,const std::string &expr,const std::string &type) {
        const auto r=translate_expression(expr,bindings,statamissing);
        if (!r.ok) return r.error;
        if (r.kind=='s') return std::string("collapse: a numeric expression is required");
        std::string sql=r.sql;
        const std::string prev="__parqit_s"+std::to_string(stages_.size());
        const std::string from=rowctx_wrap({{&sql,&r.windows}},prev,{});
        if (type=="weight") sql="parqit_finite(__parqit_double("+sql+"))";
        if (type=="byte") sql="CAST("+sql+" AS TINYINT)";
        const std::string body="SELECT "+stage_list()+", "+sql+" AS "+quote_ident(name)+" FROM "+from;
        ViewCol c; c.name=name; c.kind='n'; c.normalized=true;
        c.meta_type=type=="byte" ? "byte" : "double";
        c.physical_type=type=="byte" ? "TINYINT" : type=="weight" ? "DOUBLE" : "BIGINT";
        cols_.push_back(c); push_stage(body,"collapse expression",true);
        return std::string();
    };
    const std::string rn=helper("crn"), sample=helper("csample"), weight=helper("cw");
    auto e=generate(rn,"_n","index"); if (!e.empty()) return e;
    std::string predicate="TRUE", qualified_predicate="TRUE";
    if (!o.ifexpr.empty()) {
        e=generate(sample,"cond(("+o.ifexpr+"),1,0)","byte"); if (!e.empty()) return e;
        predicate=quote_ident(sample)+" = 1";
        qualified_predicate="o."+quote_ident(sample)+" = 1";
    }
    if (!o.first.empty() || !o.last.empty()) {
        const std::string total=helper("cn");
        e=generate(total,"_N","index"); if (!e.empty()) return e;
        auto bound=[&](const std::string &s,bool last,std::string *out) {
            if (s.empty() || s=="f") { *out=last && s.empty() ? quote_ident(total) : "1"; return true; }
            if (s=="l") { *out=quote_ident(total); return true; }
            size_t used=0; long long n=0;
            try { n=std::stoll(s,&used); } catch (...) { return false; }
            if (used!=s.size() || !n) return false;
            *out=n>0 ? std::to_string(n) : "("+quote_ident(total)+"+"+std::to_string(n)+"+1)";
            return true;
        };
        std::string first,last;
        if (!bound(o.first,false,&first) || !bound(o.last,true,&last)) return "collapse: invalid in range";
        std::string range="(CASE WHEN "+first+"<1 OR "+last+"<"+first+" OR "+last+">"+quote_ident(total)+
            " THEN error('collapse: observation number out of range') ELSE "+quote_ident(rn)+" BETWEEN "+first+" AND "+last+" END)";
        predicate+=" AND "+range;
        replace_all(range,quote_ident(total),"o."+quote_ident(total));
        replace_all(range,quote_ident(rn),"o."+quote_ident(rn));
        qualified_predicate+=" AND "+range;
    }
    if (o.cw) for (const auto &sp:specs) {
        predicate+=" AND "+key_missing_fold_sql(quote_ident(sp.source),cols_[col_index(sp.source)].kind)+" IS NOT NULL";
        qualified_predicate+=" AND "+key_missing_fold_sql("o."+quote_ident(sp.source),cols_[col_index(sp.source)].kind)+" IS NOT NULL";
    }
    if (!o.weight.empty()) {
        e=generate(weight,o.weight,"weight"); if (!e.empty()) return e;
    }
    const std::string qweight=o.weight.empty() ? "1.0" : "__parqit_double("+quote_ident(weight)+")";
    const std::string checked=helper("cweight"), base=helper("cbase"), admitted=helper("cadmit");
    const std::string qchecked=quote_ident(checked), qrn=quote_ident(rn);
    std::string validated=qweight;
    if (!o.weight.empty()) validated="CASE WHEN ("+predicate+") THEN CASE WHEN "+qweight+"<0"+
        (o.weight_type=="fweight" ? " OR "+qweight+"<>trunc("+qweight+")" : "")+
        " THEN error('collapse: weights must be nonnegative"+(o.weight_type=="fweight" ? std::string(" integers") : std::string())+
        "') ELSE "+qweight+" END ELSE NULL END";
    const std::string prev="__parqit_s"+std::to_string(stages_.size());
    std::string ctes="WITH "+base+" AS "+(o.merge ? "MATERIALIZED " : "")+"(SELECT *, "+validated+" AS "+qchecked+" FROM "+prev+"), "+
        admitted+" AS (SELECT * FROM "+base+" WHERE ("+predicate+") AND "+qchecked+">0)";
    std::string part;
    for (const auto &g:groups) part+=(part.empty() ? "" : ", ")+key_missing_fold_sql(quote_ident(g),cols_[col_index(g)].kind);
    const std::string over=part.empty() ? "" : "PARTITION BY "+part;
    struct Rank { std::string value,rn,n,cum,total,scale,low; bool weighted=false; };
    std::map<std::string,Rank> ranks;
    std::vector<std::string> rankkeys;
    std::vector<std::string> selection_flags(specs.size());
    std::vector<std::string> gini_values(specs.size());
    std::vector<std::pair<std::string,std::string>> percent_mass(specs.size());
    std::string windows;
    for (size_t i=0;i<specs.size();++i) if (specs[i].stat=="percent" && !o.weight.empty()) {
        const auto &sp=specs[i]; const std::string target=sp.target.empty() ? sp.source : sp.target;
        if (raw_targets.count(target)) continue;
        const auto &source=cols_[col_index(sp.source)];
        const std::string value="CASE WHEN "+key_missing_fold_sql(quote_ident(sp.source),source.kind)+" IS NOT NULL THEN "+qchecked+" END";
        const auto m=helper("cpm"), exponent=helper("cpe");
        windows+=", (__parqit_mass_parts("+value+") OVER ()).mantissa AS "+quote_ident(m);
        windows+=", (__parqit_mass_parts("+value+") OVER ()).exponent AS "+quote_ident(exponent);
        percent_mass[i]={m,exponent};
    }
    bool firstlast=false;
    for (const auto &sp:specs) {
        const std::string target=sp.target.empty() ? sp.source : sp.target;
        const bool raw=raw_targets.count(target) || sp.stat=="rawsum" || sp.stat=="rawnansum";
        double p; long long k; bool rs;
        if (gini_stat(sp.stat)) {
            const std::string ref=quote_ident(sp.source);
            const std::string value=sp.stat=="gini" ? "CASE WHEN "+ref+" IS NULL THEN NULL ELSE greatest("+ref+",0) END" :
                sp.stat=="gini_dropneg" ? "CASE WHEN "+ref+">=0 THEN "+ref+" END" : ref;
            const std::string w=!o.weight.empty() && !raw ? qchecked : "1.0";
            const std::string name=helper("cgini");
            const std::string ordered=(over.empty() ? "" : over+" ")+"ORDER BY ("+value+") NULLS LAST, "+qrn+
                " ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING";
            windows+=", (__parqit_gini("+value+","+w+") OVER ("+ordered+")).value AS "+quote_ident(name);
            gini_values[rankkeys.size()]=name; rankkeys.push_back("");
            continue;
        }
        const bool ranked=percentile(sp.stat,&p) || sp.stat=="iqr" || selection(sp.stat,&k,&rs);
        firstlast |= sp.stat=="first" || sp.stat=="last" || sp.stat=="firstnm" || sp.stat=="lastnm";
        if (!ranked) { rankkeys.push_back(""); continue; }
        const std::string mode="rank";
        const std::string ref=quote_ident(sp.source);
        const bool weighted=!o.weight.empty() && !raw && !(selection(sp.stat,&k,&rs) && rs);
        const std::string key=sp.source+"/"+mode+(weighted ? "/w" : "/n");
        rankkeys.push_back(key);
        if (selection(sp.stat,&k,&rs) && weighted) {
            const std::string flag=helper("cselect");
            const std::string ordered=(over.empty() ? "" : over+" ")+"ORDER BY "+ref+" NULLS LAST, "+qrn+
                (k>0 ? " ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW" : " ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING");
            windows+=", (__parqit_mass_ge(CASE WHEN "+ref+" IS NOT NULL THEN "+qchecked+" END,"+std::to_string(std::abs(k))+") OVER ("+ordered+")).value AS "+quote_ident(flag);
            selection_flags[rankkeys.size()-1]=flag;
        }
        if (ranks.count(key)) continue;
        Rank h; h.weighted=weighted;
        h.value=mode=="gini" ? "CASE WHEN "+ref+" IS NULL THEN NULL ELSE greatest("+ref+",0) END" :
                mode=="gini_dropneg" ? "CASE WHEN "+ref+">=0 THEN "+ref+" END" : ref;
        h.rn=helper("crank"); h.n=helper("ccount"); h.cum=helper("ccum");
        h.total=helper("ctotal"); h.scale=helper("cscale"); h.low=helper("clow");
        const std::string w=weighted ? qchecked : "1.0";
        const std::string validw="CASE WHEN ("+h.value+") IS NOT NULL THEN "+w+" END";
        const std::string sorted=(over.empty() ? "" : over+" ")+"ORDER BY ("+h.value+") NULLS LAST, "+qrn+" ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW";
        windows+=", CASE WHEN ("+h.value+") IS NULL THEN NULL ELSE row_number() OVER ("+sorted+") END AS "+quote_ident(h.rn);
        windows+=", count("+h.value+") OVER ("+over+") AS "+quote_ident(h.n);
        if (weighted) {
            const std::string prefix="(__parqit_mass_parts("+validw+") OVER ("+sorted+"))";
            const std::string whole="(__parqit_mass_parts("+validw+") OVER ("+over+"))";
            windows+=", __parqit_mass_share("+prefix+".mantissa,"+prefix+".exponent,"+whole+".mantissa,"+whole+".exponent)/100.0 AS "+quote_ident(h.cum);
            windows+=", 1.0 AS "+quote_ident(h.total);
        }
        windows+=", greatest(abs(min("+h.value+") OVER ("+over+")),abs(max("+h.value+") OVER ("+over+"))) AS "+quote_ident(h.scale);
        windows+=", min("+h.value+") OVER ("+over+") AS "+quote_ident(h.low);
        ranks[key]=h;
    }
    const std::string first_rn=helper("cfirst");
    if (firstlast) {
        std::string ord=original_order;
        if (ord.empty()) for (const auto &c:original_cols) ord+=(ord.empty() ? " ORDER BY " : ", ")+quote_ident(c.name)+" NULLS LAST";
        windows+=", row_number() OVER ("+ord+") AS "+quote_ident(first_rn);
    }
    const std::string ranked=helper("cwindow");
    ctes+=", "+ranked+" AS (SELECT *"+windows+" FROM "+admitted+")";
    std::string sel;
    std::vector<ViewCol> result;
    for (const auto &g:groups) {
        if (!sel.empty()) sel+=", ";
        const auto &c=cols_[col_index(g)];
        sel+=key_missing_fold_sql(quote_ident(g),c.kind)+" AS "+quote_ident(g);
        result.push_back(c); if (c.kind=='s') result.back().normalized=false;
    }
    for (size_t i=0;i<specs.size();++i) {
        const auto &sp=specs[i]; const auto &source=cols_[col_index(sp.source)];
        const std::string target=sp.target.empty() ? sp.source : sp.target, ref=quote_ident(sp.source);
        const bool raw=raw_targets.count(target) || sp.stat=="rawsum" || sp.stat=="rawnansum";
        const bool weighted=!o.weight.empty() && !raw;
        const std::string w=weighted ? qchecked : "1.0";
        const std::string nonmissing=key_missing_fold_sql(ref,source.kind);
        const bool cardinality=sp.stat=="count" || sp.stat=="nmissing" || sp.stat=="unique" || sp.stat=="nunique";
        long long select_k; bool raw_select;
        const bool value_stat=sp.stat=="min" || sp.stat=="max" || sp.stat=="first" || sp.stat=="last" || sp.stat=="firstnm" || sp.stat=="lastnm" || selection(sp.stat,&select_k,&raw_select);
        const bool string_value=sp.stat=="first" || sp.stat=="last" || sp.stat=="firstnm" || sp.stat=="lastnm";
        if (source.kind=='s' && !cardinality && !string_value) return "collapse: "+sp.stat+" needs a numeric variable";
        const bool standard=sp.stat=="sd" || sp.stat=="variance" || sp.stat=="cv" || sp.stat=="semean";
        if (!o.weight.empty() && ((o.weight_type=="pweight" && standard) ||
            ((o.weight_type=="pweight" || o.weight_type=="iweight") && sp.stat=="semean") ||
            (o.weight_type!="fweight" && (sp.stat=="sebinomial" || sp.stat=="sepoisson"))))
            return "collapse: "+sp.stat+" is not allowed with "+o.weight_type;
        const bool detail=sp.stat=="skewness" || sp.stat=="kurtosis";
        const std::string state=weighted ? "("+std::string(detail ? "__parqit_wdetail" : standard ? "__parqit_wstats" : "__parqit_wtotal")+"("+ref+","+w+"))" :
            "("+std::string(detail ? "__parqit_detail" : standard ? "__parqit_stats" : "__parqit_total")+"("+ref+"))";
        const std::string n=weighted ? state+".mass" : state+".n";
        const std::string sd=state+(weighted && o.weight_type=="aweight" ? ".sd_aw" : ".sd");
        std::string agg;
        if (sp.stat=="mean") agg=state+".mean";
        else if (sp.stat=="sum" || sp.stat=="rawsum" || sp.stat=="nansum" || sp.stat=="rawnansum") {
            agg=state+".sum";
            if (weighted && o.weight_type=="aweight") agg=state+".sum_aw";
            if (sp.stat=="nansum" || sp.stat=="rawnansum" || o.missing) agg="CASE WHEN "+state+".n=0 THEN NULL ELSE "+agg+" END";
        } else if (sp.stat=="sd") agg=sd;
        else if (sp.stat=="variance") agg=state+(weighted && o.weight_type=="aweight" ? ".variance_aw" : ".variance");
        else if (sp.stat=="cv") agg=sd+"/nullif("+state+".mean,0)";
        else if (sp.stat=="skewness" || sp.stat=="kurtosis") agg=state+"."+sp.stat;
        else if (sp.stat=="semean") agg=sd+"/sqrt("+(weighted && o.weight_type=="aweight" ? state+".n" : n)+")";
        else if (sp.stat=="sebinomial" || sp.stat=="sepoisson") agg=weighted ?
            "(__parqit_werrors("+ref+","+w+"))."+sp.stat : "(__parqit_"+sp.stat+"("+ref+")).value";
        else if (sp.stat=="min" || sp.stat=="max") agg=sp.stat+"("+ref+")";
        else if (sp.stat=="count" || sp.stat=="nmissing" || sp.stat=="percent") {
            const std::string pred=nonmissing+(sp.stat=="nmissing" ? " IS NULL" : " IS NOT NULL");
            if (weighted && o.weight_type=="fweight" && sp.stat!="percent")
                agg="CAST(coalesce(sum(CAST(CASE WHEN "+pred+" THEN "+w+" END AS HUGEINT)),0) AS DECIMAL(38,0))";
            else if (weighted && (o.weight_type!="aweight" || sp.stat=="percent")) agg="(__parqit_total(CASE WHEN "+pred+" THEN "+w+" END)).sum";
            else agg="count(*) FILTER (WHERE "+pred+")";
            if (sp.stat=="percent") {
                if (weighted) {
                    const std::string mass="(__parqit_mass_parts(CASE WHEN "+pred+" THEN "+w+" END))";
                    agg="__parqit_mass_share("+mass+".mantissa,"+mass+".exponent,min("+quote_ident(percent_mass[i].first)+"),min("+quote_ident(percent_mass[i].second)+"))";
                } else agg="(CAST("+agg+" AS DOUBLE)/nullif(sum("+agg+") OVER (),0))*100";
            }
        } else if (sp.stat=="unique" || sp.stat=="nunique")
            agg="count(DISTINCT row("+nonmissing+"))";
        else if (sp.stat=="geomean") {
            const std::string logs="ln(nullif("+ref+",0))";
            const std::string meanlog=weighted ? "(__parqit_wtotal("+logs+","+w+")).mean" : "(__parqit_total("+logs+")).mean";
            agg="CASE WHEN min("+ref+")<0 THEN NULL WHEN min("+ref+")=0 THEN 0 ELSE exp("+meanlog+") END";
            replace_all(agg,logs,"ln(CASE WHEN "+ref+">0 THEN "+ref+" END)");
        } else if (sp.stat=="range") agg="__parqit_difference(max("+ref+"),min("+ref+"))";
        else if (gini_stat(sp.stat)) agg="max("+quote_ident(gini_values[i])+")";
        else if (sp.stat=="first" || sp.stat=="last" || sp.stat=="firstnm" || sp.stat=="lastnm") {
            const bool first=sp.stat.rfind("first",0)==0, nm=sp.stat=="firstnm" || sp.stat=="lastnm";
            agg=std::string(first ? "arg_min" : "arg_max")+"("+(nm ? nonmissing : "row("+ref+")")+","+quote_ident(first_rn)+")"+(nm ? "" : "[1]");
        } else {
            double p; long long k; bool rs;
            if (rankkeys[i].empty()) return "collapse: unknown statistic ("+sp.stat+")";
            const Rank &h=ranks.at(rankkeys[i]);
            const std::string cr=quote_ident(h.rn), total=quote_ident(h.total), cum=quote_ident(h.cum);
            if (selection(sp.stat,&k,&rs)) {
                if (h.weighted) {
                    agg=std::string(k>0 ? "min" : "max")+"("+ref+") FILTER (WHERE "+quote_ident(selection_flags[i])+")";
                } else agg="max(CASE WHEN "+cr+"="+(k>0 ? std::to_string(k) : quote_ident(h.n)+"+"+std::to_string(k)+"+1")+" THEN "+ref+" END)";
            } else if (percentile(sp.stat,&p) || sp.stat=="iqr") {
                auto quantile=[&](double q) {
                    if (!h.weighted) return pct_window_sql(ref,h.rn,h.n,q);
                    const std::string normalized="("+cum+"/"+total+"*"+quote_ident(h.n)+")";
                    const std::string cut="("+quote_ident(h.n)+"*"+number(q)+"/100.0)";
                    const std::string below=normalized+"<"+cut+"+1e-6";
                    const std::string left="max("+ref+") FILTER (WHERE "+below+")", right="min("+ref+") FILTER (WHERE NOT ("+below+"))";
                    return "CASE WHEN abs(max("+normalized+") FILTER (WHERE "+below+")-max("+cut+"))<=1e-6 THEN __parqit_midpoint("+left+",coalesce("+right+","+left+")) ELSE coalesce("+right+","+left+") END";
                };
                agg=sp.stat=="iqr" ? "__parqit_difference("+quantile(75)+","+quantile(25)+")" : quantile(p);
                if (source.is_float()) agg="CAST(("+agg+") AS FLOAT)";
            } else return "collapse: unknown statistic ("+sp.stat+")";
        }
        const bool numeric=!value_stat || source.kind=='n';
        if (numeric && !value_stat && !cardinality) agg="parqit_finite("+agg+")";
        sel+=(sel.empty() ? "" : ", ")+agg+" AS "+quote_ident(target);
        ViewCol c; c.name=target; if (sp.target.empty()) c.stata=source.stata;
        if (destination_aliases.count(target)) c.stata=destination_aliases.at(target);
        c.kind=numeric ? 'n' : 's'; c.fmt=numeric && source.kind=='s' ? "%8.0g" : source.fmt;
        c.varlab=label_for(sp,source,o);
        if (value_stat) { c.meta_type=source.meta_type; c.vallab=source.vallab; c.physical_type=source.physical_type; }
        else if (cardinality && (!weighted || o.weight_type=="aweight" || o.weight_type=="fweight" || sp.stat=="unique" || sp.stat=="nunique")) {
            c.meta_type="long"; c.physical_type=weighted && o.weight_type=="fweight" && sp.stat!="unique" && sp.stat!="nunique" ? "HUGEINT" : "BIGINT";
        } else { c.meta_type="double"; c.physical_type="DOUBLE"; }
        const bool sum_stat=sp.stat=="sum" || sp.stat=="rawsum" || sp.stat=="nansum" || sp.stat=="rawnansum";
        const bool integer_source=source.meta_type=="byte" || source.meta_type=="int" || source.meta_type=="long" ||
            source.physical_type=="TINYINT" || source.physical_type=="SMALLINT" || source.physical_type=="INTEGER" || source.physical_type=="BIGINT";
        c.integer_pack=o.sumcheck && sum_stat && integer_source && !o.double_targets;
        c.exact_integer=cardinality && (!weighted || o.weight_type=="fweight" || o.weight_type=="aweight" || sp.stat=="unique" || sp.stat=="nunique");
        if (c.integer_pack) c.meta_type.clear();
        if (o.double_targets && c.kind=='n') { c.meta_type="double"; c.physical_type="DOUBLE"; }
        c.normalized=c.kind=='n'; result.push_back(c);
    }
    if (!frequency.empty()) {
        sel+=(sel.empty() ? "" : ", ")+std::string("count(*) AS ")+quote_ident(frequency);
        ViewCol f; f.name=frequency; f.kind='n'; f.meta_type=o.double_targets ? "double" : "long";
        if (destination_aliases.count(frequency)) f.stata=destination_aliases.at(frequency);
        f.physical_type="BIGINT"; f.fmt="%12.0g"; f.normalized=true; f.exact_integer=true; f.varlab="Group size";
        if (!o.labelformat.empty()) f.varlab=label_for({"freq",frequency,""},ViewCol{},o);
        while (!f.varlab.empty() && std::isspace(static_cast<unsigned char>(f.varlab.back()))) f.varlab.pop_back();
        result.push_back(f);
    }
    const std::string aggregate=helper("caggregate");
    ctes+=", "+aggregate+" AS (SELECT "+sel+" FROM "+ranked;
    if (!part.empty()) ctes+=" GROUP BY "+part;
    else ctes+=" HAVING count(*)>0";
    ctes+=") ";
    std::string body;
    if (o.merge) {
        body=ctes+"SELECT ";
        std::vector<ViewCol> merged;
        std::map<std::string,ViewCol> targets;
        for (size_t i=groups.size();i<result.size();++i) targets[result[i].name]=result[i];
        auto append_target=[&](const ViewCol &c,bool existed) {
            const std::string fallback=o.noinit && existed ? "o."+quote_ident(c.name) : "NULL";
            body+=std::string(merged.empty() ? "" : ", ")+"CASE WHEN ("+qualified_predicate+") AND o."+qchecked+">0 THEN a."+quote_ident(c.name)+" ELSE "+fallback+" END AS "+quote_ident(c.name);
            merged.push_back(c);
        };
        for (const auto &c:original_cols) {
            const auto found=targets.find(c.name);
            if (found!=targets.end()) { append_target(found->second,true); targets.erase(found); continue; }
            body+=std::string(merged.empty() ? "" : ", ")+"o."+quote_ident(c.name); merged.push_back(c);
        }
        for (size_t i=groups.size();i<result.size();++i) {
            const auto &c=result[i];
            if (targets.count(c.name)) append_target(c,false);
        }
        body+=", o."+qrn+" AS "+qrn+" FROM "+base+" o LEFT JOIN "+aggregate+" a ON ";
        if (groups.empty()) body+="TRUE";
        for (size_t i=0;i<groups.size();++i) body+=(i ? " AND " : "")+
            key_missing_fold_sql("o."+quote_ident(groups[i]),cols_[col_index(groups[i])].kind)+" IS NOT DISTINCT FROM a."+quote_ident(groups[i]);
        cols_=std::move(merged);
        order_cols_={qrn}; order_live_=true; order_baked_=true; sort_=original_sort;
        for (const auto &sp:specs) {
            const std::string target=sp.target.empty() ? sp.source : sp.target;
            for (const auto &key:original_sort)
                if (key==quote_ident(target) || key==quote_ident(target)+" DESC") sort_.clear();
            if (target==ts_time_ || target==ts_panel_) clear_tsset();
        }
    } else { body=ctes+"SELECT * FROM "+aggregate; cols_=std::move(result); order_live_=false; order_baked_=false; sort_.clear(); }
    push_stage(body,o.merge ? "collapse, merge" : "collapse",o.merge);
    if (!o.merge && !o.unsorted) for (size_t i=0;i<groups.size();++i) sort_.push_back(quote_ident(groups[i])+(desc[i] ? " DESC" : ""));
    return "";
}
} // namespace parqit
