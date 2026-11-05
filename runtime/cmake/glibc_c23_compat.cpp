// glibc < 2.38 compatibility for the prebuilt Dawn library (MKW_GLIBC_C23_COMPAT, Linux only).
//
// Dawn's prebuilt libwebgpu_dawn.a was compiled against glibc >= 2.38, whose headers redirect
// strtol & co. to the C23 entry points __isoc23_strto*. The only behavioural difference is that
// the C23 versions also accept a "0b"/"0B" prefix for base 0 and 2. Defining them here lets a
// release build link against glibc 2.35 (Ubuntu 22.04) and run on every newer distribution.
// Enabled only for such builds: on a newer glibc, libc's own definitions are used.
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>

namespace {

// Binary-prefix handling on top of the classic function. A negative binary literal that is
// exactly the type's minimum reports ERANGE; nothing in Dawn parses binary literals.
template <typename T, typename Classic>
T ParseC23(const char* text, char** end, int base, Classic classic, bool isSigned, T minimum, T maximum) {
    if (base == 0 || base == 2) {
        const char* p = text;
        while (std::isspace(static_cast<unsigned char>(*p))) ++p;
        bool negative = false;
        if (*p == '+' || *p == '-') negative = *p++ == '-';
        if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B') && (p[2] == '0' || p[2] == '1')) {
            T value = classic(p + 2, end, 2);
            if (!negative) return value;
            if (isSigned && value == maximum && errno == ERANGE) return minimum;
            return static_cast<T>(0) - value;
        }
    }
    return classic(text, end, base);
}

} // namespace

extern "C" {

long __isoc23_strtol(const char* text, char** end, int base) {
    return ParseC23<long>(text, end, base, std::strtol, true, LONG_MIN, LONG_MAX);
}

long long __isoc23_strtoll(const char* text, char** end, int base) {
    return ParseC23<long long>(text, end, base, std::strtoll, true, LLONG_MIN, LLONG_MAX);
}

unsigned long __isoc23_strtoul(const char* text, char** end, int base) {
    return ParseC23<unsigned long>(text, end, base, std::strtoul, false, 0, ULONG_MAX);
}

unsigned long long __isoc23_strtoull(const char* text, char** end, int base) {
    return ParseC23<unsigned long long>(text, end, base, std::strtoull, false, 0, ULLONG_MAX);
}

} // extern "C"
