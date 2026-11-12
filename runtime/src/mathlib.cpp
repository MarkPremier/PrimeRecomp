/* MathLib (PowerPC Numerics), SANE decimal conversion, fixed-point helpers. */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <algorithm>

#include "pt/runtime.h"

using namespace pt;

/* double functions: arguments in f1/f2, result in f1 */
HLE(sin) { c->f[1] = std::sin(c->f[1]); }
HLE(cos) { c->f[1] = std::cos(c->f[1]); }
HLE(tan) { c->f[1] = std::tan(c->f[1]); }
HLE(asin) { c->f[1] = std::asin(c->f[1]); }
HLE(atan) { c->f[1] = std::atan(c->f[1]); }
HLE(pow) { c->f[1] = std::pow(c->f[1], c->f[2]); }

HLE(FixMul) {
    int64_t p = (int64_t)(int32_t)ARG(0) * (int32_t)ARG(1);
    RET((uint32_t)((p + 0x8000) >> 16));
}

/* SANE decimal record: sgn(1) unused(1) exp(2) sig: length(1) + up to 36 digits
 * decform: style(1) (0 floating, 1 fixed) unused(1) digits(2) */
HLE(num2dec) {
    /* void num2dec(const decform *f, double x, decimal *d) — x arrives in f1 */
    uint32_t f = ARG(0), d = ARG(1);
    double x = c->f[1];
    int style = ld8(f), digits = (int16_t)ld16(f + 2);
    st8(d, std::signbit(x) ? 1 : 0);
    st8(d + 1, 0);
    x = std::fabs(x);
    std::string sig;
    int exp = 0;
    if (std::isnan(x)) sig = "N";
    else if (std::isinf(x)) sig = "I";
    else if (x == 0) sig = "0";
    else if (style == 1) {
        /* fixed: `digits` digits after the point */
        if (digits < 0) digits = 0;
        char buf[400];
        snprintf(buf, sizeof buf, "%.*f", digits > 30 ? 30 : digits, x);
        std::string s(buf);
        size_t dot = s.find('.');
        std::string ip = s.substr(0, dot), fp = dot == std::string::npos ? "" : s.substr(dot + 1);
        sig = ip + fp;
        exp = -(int)fp.size();
        size_t nz = sig.find_first_not_of('0');
        sig = nz == std::string::npos ? "0" : sig.substr(nz);
    } else {
        if (digits < 1) digits = 1;
        if (digits > 36) digits = 36;
        char buf[64];
        snprintf(buf, sizeof buf, "%.*e", digits - 1, x);
        std::string s(buf);
        size_t e = s.find('e');
        std::string mant = s.substr(0, e);
        int ex = atoi(s.c_str() + e + 1);
        mant.erase(std::remove(mant.begin(), mant.end(), '.'), mant.end());
        sig = mant;
        exp = ex - (int)(mant.size() - 1);
    }
    if (sig.size() > 36) sig.resize(36);
    st16(d + 2, (uint16_t)(int16_t)exp);
    st8(d + 4, (uint8_t)sig.size());
    memcpy(g_mem + d + 5, sig.data(), sig.size());
}

HLE(dec2str) {
    /* void dec2str(const decform *f, const decimal *d, char *s) — writes a Pascal-format string */
    uint32_t f = ARG(0), d = ARG(1), out = ARG(2);
    int style = ld8(f), digits = (int16_t)ld16(f + 2);
    bool neg = ld8(d) != 0;
    int exp = (int16_t)ld16(d + 2);
    std::string sig((const char *)g_mem + d + 5, ld8(d + 4));
    std::string s;
    if (sig == "N") s = "NAN";
    else if (sig == "I") s = "INF";
    else if (style == 1) {
        /* fixed: integer and fraction parts from sig * 10^exp, then pad/trim to `digits` */
        std::string all = sig;
        int point = (int)all.size() + exp;
        if (point <= 0) { all = std::string(1 - point, '0') + all; point = 1; }
        if (point > (int)all.size()) all += std::string(point - all.size(), '0');
        std::string ip = all.substr(0, point), fp = all.substr(point);
        if (digits > 0) {
            if ((int)fp.size() < digits) fp += std::string(digits - fp.size(), '0');
            fp.resize(digits);
            s = ip + "." + fp;
        } else s = ip;
    } else {
        std::string m = sig.substr(0, 1);
        if (sig.size() > 1) m += "." + sig.substr(1);
        int e10 = exp + (int)sig.size() - 1;
        s = m + "e" + (e10 >= 0 ? "+" : "") + std::to_string(e10);
    }
    if (neg) s = "-" + s;
    write_pstr(out, s);
}

HLE(GetDCtlEntry) {
    /* DCtlHandle GetDCtlEntry(short refNum): a zeroed device control entry is enough */
    static uint32_t dce = 0;
    if (!dce) dce = handle_new(64, true);
    RET(dce);
}
