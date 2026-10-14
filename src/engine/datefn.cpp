#include "engine/datefn.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace parqit::datefn {

namespace {

/* a field of the text: a run of digits, a month name, an am/pm marker or any
 * other word; `frac` marks digits written right after "digits." (45.678: the
 * 678), the candidate fraction of a second */
struct Field {
    enum Kind { Digits, Month, Meridian, Word };
    Kind kind = Word;
    std::string text; /* the digits as written, or the word */
    int month = 0;    /* 1..12 for a month name */
    int meridian = 0; /* 1 am, 2 pm */
    bool frac = false;
};

const char *kMonthNames[12] = {"january", "february", "march", "april", "may",
                               "june", "july", "august", "september", "october",
                               "november", "december"};

/* a month name or an unambiguous prefix of at least three letters (sept) */
int month_of(const std::string &word) {
    if (word.size() < 3) return 0;
    int hit = 0;
    for (int m = 0; m < 12; m++) {
        const std::string full = kMonthNames[m];
        if (word.size() <= full.size() && full.compare(0, word.size(), word) == 0) {
            if (hit) return 0; /* ambiguous (ju-) */
            hit = m + 1;
        }
    }
    return hit;
}

/* cut the text into fields as native's readers do (oracles: native Stata,
 * nat extra/extra2/extra3): runs of digits, month names (three letters or
 * more, unambiguous), am/pm, and any other word, which only a # of the mask
 * may take; anything else separates. In a period reader a single letter that
 * a digit follows at once, after a field, is the unit's separator (2020q3,
 * 2020 m7, 2020j7 parse; x2020m7 and m7 2020 do not), and am/pm is a word. */
std::vector<Field> fields_of(const std::string &s, bool period) {
    std::vector<Field> out;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (std::isdigit(c)) {
            Field f;
            f.kind = Field::Digits;
            f.frac = i >= 2 && s[i - 1] == '.' && std::isdigit(static_cast<unsigned char>(s[i - 2]));
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) f.text.push_back(s[i++]);
            out.push_back(f);
        } else if (std::isalpha(c)) {
            std::string word;
            while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i])))
                word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(s[i++]))));
            if (period && word.size() == 1 && !out.empty() && i < s.size() &&
                std::isdigit(static_cast<unsigned char>(s[i])))
                continue; /* the unit letter of 2020q3 */
            Field f;
            if (!period && (word == "am" || word == "pm")) {
                f.kind = Field::Meridian;
                f.meridian = word == "am" ? 1 : 2;
            } else if (const int m = month_of(word)) {
                f.kind = Field::Month;
                f.month = m;
            } else {
                f.kind = Field::Word;
                f.text = word;
            }
            out.push_back(f);
        } else {
            i++;
        }
    }
    return out;
}

struct Mask {
    std::vector<char> letters; /* Y M D h m s # */
    int century = -1;          /* 19, 20 ... from "19Y"; -1 none */
};

bool read_mask(const std::string &mask, Mask *out, const std::string &allowed, std::string *err) {
    out->letters.clear();
    out->century = -1;
    size_t i = 0;
    while (i < mask.size()) {
        const char c = mask[i];
        if (c == ' ') { i++; continue; } /* native accepts e.g. DMY hms */
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t j = i;
            while (j < mask.size() && std::isdigit(static_cast<unsigned char>(mask[j]))) j++;
            if (j >= mask.size() || mask[j] != 'Y' || j - i != 2) {
                *err = "a number in the mask must be a two-digit century before Y, as in 20Y";
                return false;
            }
            out->century = (mask[i] - '0') * 10 + (mask[i + 1] - '0');
            i = j;
            continue;
        }
        if (allowed.find(c) == std::string::npos) {
            *err = std::string("the mask cannot contain '") + c + "' (allowed: " + allowed + ")";
            return false;
        }
        out->letters.push_back(c);
        i++;
    }
    return true;
}

int width_of(char letter) { return letter == 'Y' ? 4 : 2; }

/* the fields assigned to the mask letters in order, native's way: the first
 * am/pm sets the meridian and leaves the fields (a later one is a word); a
 * digit run longer than its letter is cut by the letters' widths, the
 * remainder serving the next letter and the last letter taking what is left
 * (2006125 with YMD: 2006, 12, 5); a month name takes M only; # takes exactly
 * one field of any kind, or, as the last letter, everything that remains
 * (date("15jan2020 10:30 am pm", "DMY#") is 15jan2020); a word anywhere else,
 * or fields left over, make the value missing. *slots holds the fields of the
 * letters other than #, in order. */
bool assign(const std::vector<Field> &fields, const Mask &mask, std::vector<Field> *slots,
            int *meridian) {
    slots->clear();
    *meridian = 0;
    std::vector<Field> q;
    bool seen = false;
    for (const auto &f : fields) {
        if (f.kind == Field::Meridian) {
            if (!seen) {
                *meridian = f.meridian;
                seen = true;
                continue;
            }
            Field w = f;
            w.kind = Field::Word;
            q.push_back(w);
            continue;
        }
        q.push_back(f);
    }
    size_t qi = 0;
    const size_t nl = mask.letters.size();
    for (size_t li = 0; li < nl; li++) {
        const char L = mask.letters[li];
        if (L == '#') {
            if (li + 1 == nl) return true; /* the rest, whatever it holds */
            if (qi >= q.size()) return false;
            qi++;
            continue;
        }
        if (qi >= q.size()) return false;
        const Field &f = q[qi];
        if (f.kind == Field::Word) return false;
        if (f.kind == Field::Month) {
            if (L != 'M') return false;
            slots->push_back(f);
            qi++;
            continue;
        }
        const size_t w = static_cast<size_t>(width_of(L));
        if (f.text.size() <= w || li + 1 == nl) {
            slots->push_back(f);
            qi++;
            continue;
        }
        Field part = f;
        part.text = f.text.substr(0, w);
        part.frac = false;
        slots->push_back(part);
        q[qi].text = f.text.substr(w);
        q[qi].frac = false;
    }
    return qi == q.size();
}

bool number_of(const Field &f, long long *v) {
    if (f.kind != Field::Digits) return false;
    if (f.text.size() > 9) return false;
    *v = std::atoll(f.text.c_str());
    return true;
}

bool year_of(const Field &f, const Mask &mask, long long topyear, long long *year) {
    long long y = 0;
    if (!number_of(f, &y)) return false;
    if (f.text.size() <= 2) {
        if (mask.century >= 0) y += 100LL * mask.century;
        else if (topyear > 0) {
            /* the latest year ending in those digits that is not after topyear */
            y = topyear - ((topyear - y) % 100 + 100) % 100;
        } else
            return false;
    }
    if (y < 100 || y > 9999) return false;
    *year = y;
    return true;
}

} // namespace

bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int days_in_month(int y, int m) {
    static const int dim[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) return 0;
    return m == 2 && is_leap(y) ? 29 : dim[m];
}

long long days_from_civil(int y, int m, int d) {
    long long yy = y;
    yy -= m <= 2;
    const long long era = (yy >= 0 ? yy : yy - 399) / 400;
    const long long yoe = yy - era * 400;
    const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468 + 3653;
}

bool civil_from_days(long long days, int *y, int *m, int *d) {
    long long z = days - 3653 + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long yy = yoe + era * 400;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    *d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    *m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    *y = static_cast<int>(yy + (*m <= 2));
    return *y >= 100 && *y <= 9999;
}

std::string check_mask(const std::string &mask, char kind) {
    Mask m;
    std::string err;
    std::string allowed;
    if (kind == 'd') allowed = "YMD#";
    else if (kind == 'c') allowed = "YMDhms#";
    else if (kind == 'm') allowed = "YM#";
    else if (kind == 'q') allowed = "YQ#";
    else if (kind == 'h') allowed = "YH#";
    else if (kind == 'w') allowed = "YW#";
    else allowed = "Y#";
    if (!read_mask(mask, &m, allowed, &err)) return err;
    int y = 0, mo = 0, d = 0, hh = 0, mi = 0, ss = 0;
    for (char c : m.letters) {
        if (c == 'Y') y++;
        else if (c == 'M' || c == 'Q' || c == 'H' || c == 'W') mo++;
        else if (c == 'D') d++;
        else if (c == 'h') hh++;
        else if (c == 'm') mi++;
        else if (c == 's') ss++;
    }
    if (y != 1) return "the mask must contain Y once";
    if (kind == 'd' && (mo != 1 || d != 1)) return "the mask must contain Y, M and D once each";
    if (kind == 'c' && (mo != 1 || d != 1 || hh > 1 || mi > 1 || ss > 1 || (ss && !mi) || (mi && !hh)))
        return "the mask must contain Y, M and D once each, then h, hm or hms";
    if ((kind == 'm' || kind == 'q' || kind == 'h' || kind == 'w') && mo != 1)
        return std::string("the mask must contain Y and ") +
               (kind == 'm' ? 'M' : kind == 'q' ? 'Q' : kind == 'h' ? 'H' : 'W') + " once each";
    if (kind == 'y' && mo != 0) return "the mask of yearly() is Y";
    return "";
}

namespace {

/* the common parse: *y, *mo, *d, and for a clock *h *mi *sec (*frac_ms the
 * fraction of a second); period readers read the unit's number into *mo */
bool parse_common(const std::string &s, const std::string &mask, const std::string &allowed,
                  long long topyear, long long *y, long long *mo, long long *d, long long *hh,
                  long long *mi, double *sec, bool *has_time, long long *frac_ms) {
    Mask m;
    std::string err;
    if (!read_mask(mask, &m, allowed, &err)) return false;
    /* the period readers (monthly ... yearly) have no day letter */
    const bool period = allowed.find('D') == std::string::npos;
    std::vector<Field> fields = fields_of(s, period);
    std::vector<Field> slots;
    int meridian = 0;
    *frac_ms = 0;
    if (!assign(fields, m, &slots, &meridian)) {
        /* a clock's fraction of a second (45.678, 103045.5): the last field,
         * digits written after "digits.", is the fraction when the fields
         * come out even without it (10:30.5 with hms is 10:30:05, as native);
         * more than three decimals is missing */
        const bool has_s = std::find(m.letters.begin(), m.letters.end(), 's') != m.letters.end();
        if (!has_s || fields.empty() || fields.back().kind != Field::Digits || !fields.back().frac)
            return false;
        const std::string frac = fields.back().text;
        fields.pop_back();
        if (!assign(fields, m, &slots, &meridian)) return false;
        if (frac.size() > 3) return false;
        std::string f3 = frac;
        while (f3.size() < 3) f3.push_back('0');
        *frac_ms = std::atoll(f3.c_str());
    }
    *y = 0; *mo = 0; *d = 1; *hh = 0; *mi = 0; *sec = 0; *has_time = false;
    bool got_year = false;
    size_t si = 0;
    for (char L : m.letters) {
        if (L == '#') continue;
        if (si >= slots.size()) return false;
        const Field &f = slots[si++];
        long long v = 0;
        switch (L) {
        case 'Y':
            if (!year_of(f, m, topyear, y)) return false;
            got_year = true;
            break;
        case 'M': case 'Q': case 'H': case 'W':
            if (f.kind == Field::Month) {
                if (L != 'M') return false;
                *mo = f.month;
            } else {
                if (!number_of(f, &v)) return false;
                *mo = v;
            }
            break;
        case 'D':
            if (!number_of(f, d)) return false;
            break;
        case 'h':
            if (!number_of(f, hh)) return false;
            *has_time = true;
            break;
        case 'm':
            if (!number_of(f, mi)) return false;
            break;
        case 's':
            if (!number_of(f, &v)) return false;
            *sec = static_cast<double>(v);
            break;
        default: return false;
        }
    }
    if (!got_year) return false;
    /* am/pm without an hour in the mask is ignored: date("15jan2020 am",
     * "DMY") is 15jan2020 in native */
    if (meridian && std::find(m.letters.begin(), m.letters.end(), 'h') != m.letters.end()) {
        if (*hh < 1 || *hh > 12) return false;
        if (meridian == 1 && *hh == 12) *hh = 0;
        if (meridian == 2 && *hh < 12) *hh += 12;
    }
    return true;
}

} // namespace

bool parse_date(const std::string &s, const std::string &mask, long long topyear, long long *days) {
    long long y, mo, d, hh, mi, frac;
    double sec;
    bool has_time;
    if (!parse_common(s, mask, "YMD#", topyear, &y, &mo, &d, &hh, &mi, &sec, &has_time, &frac)) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > days_in_month(static_cast<int>(y), static_cast<int>(mo))) return false;
    *days = days_from_civil(static_cast<int>(y), static_cast<int>(mo), static_cast<int>(d));
    return true;
}

bool parse_clock(const std::string &s, const std::string &mask, long long topyear, long long *ms) {
    long long y, mo, d, hh, mi, frac;
    double sec;
    bool has_time;
    if (!parse_common(s, mask, "YMDhms#", topyear, &y, &mo, &d, &hh, &mi, &sec, &has_time, &frac)) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > days_in_month(static_cast<int>(y), static_cast<int>(mo))) return false;
    if (hh < 0 || hh > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 59) return false;
    *ms = days_from_civil(static_cast<int>(y), static_cast<int>(mo), static_cast<int>(d)) * 86400000LL +
          hh * 3600000LL + mi * 60000LL + static_cast<long long>(sec) * 1000LL + frac;
    return true;
}

bool parse_period(const std::string &s, const std::string &mask, char unit, long long topyear,
                  long long *value) {
    const std::string allowed = unit == 'm' ? "YM#" : unit == 'q' ? "YQ#" : unit == 'h' ? "YH#"
                              : unit == 'w' ? "YW#" : "Y#";
    long long y, mo, d, hh, mi, frac;
    double sec;
    bool has_time;
    if (!parse_common(s, mask, allowed, topyear, &y, &mo, &d, &hh, &mi, &sec, &has_time, &frac)) return false;
    const long long per = unit == 'm' ? 12 : unit == 'q' ? 4 : unit == 'h' ? 2 : unit == 'w' ? 52 : 1;
    if (unit == 'y') {
        *value = y;
        return true;
    }
    if (mo < 1 || mo > per) return false;
    *value = (y - 1960) * per + (mo - 1);
    return true;
}

} // namespace parqit::datefn
