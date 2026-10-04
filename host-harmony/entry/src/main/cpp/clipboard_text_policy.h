#ifndef HRD_CLIPBOARD_TEXT_POLICY_H
#define HRD_CLIPBOARD_TEXT_POLICY_H
#include <cstring>
namespace clipboard_text_policy {
constexpr unsigned int MAX_TYPES = 32;
enum class Rejection { None, MissingTypes, TooManyTypes, UnknownRepresentation, MissingPlainText, AmbiguousRecords };
struct Shape {
    unsigned int typeCount = 0, recordCount = 0;
    Rejection rejection = Rejection::None;
};
inline unsigned int CappedCount(unsigned int count) { return count > MAX_TYPES ? MAX_TYPES + 1 : count; }
inline const char* Category(Rejection value)
{
    switch (value) {
        case Rejection::None: return "none";
        case Rejection::MissingTypes: return "missing_types";
        case Rejection::TooManyTypes: return "too_many_types";
        case Rejection::UnknownRepresentation: return "unsupported_representation";
        case Rejection::MissingPlainText: return "missing_plain_text";
        case Rejection::AmbiguousRecords: return "ambiguous_records";
    }
    return "unknown";
}
inline Rejection Check(const char* const* types, unsigned int count, unsigned int records)
{
    // Multiple records can be independent text segments. A primary-only read
    // must not silently discard the remainder. Allow alternate representations
    // only when they belong to a single clipboard record.
    if (records != 1) return Rejection::AmbiguousRecords;
    if (!types || count == 0) return Rejection::MissingTypes;
    if (count > MAX_TYPES) return Rejection::TooManyTypes;
    bool hasPlain = false;
    for (unsigned int i = 0; i < count; ++i) {
        if (!types[i]) return Rejection::MissingTypes;
        if (std::strcmp(types[i], "general.plain-text") == 0) hasPlain = true;
        else if (std::strcmp(types[i], "general.text") != 0 && std::strcmp(types[i], "general.html") != 0 &&
            std::strcmp(types[i], "general.xhtml") != 0 && std::strcmp(types[i], "general.rich-text") != 0)
            return Rejection::UnknownRepresentation;
    }
    return hasPlain ? Rejection::None : Rejection::MissingPlainText;
}
}
#endif
