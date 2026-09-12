#include "haf/core/version.hpp"

#include <cctype>
#include <charconv>
#include <cstddef>
#include <string>
#include <vector>

namespace haf {
namespace {

[[nodiscard]] bool is_identifier_char(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-';
}

[[nodiscard]] bool is_numeric_identifier(const std::string& text) noexcept {
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_identifiers(const std::string& text) noexcept {
    if (text.empty()) {
        return false;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = text.find('.', start);
        const std::string part = text.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (part.empty()) {
            return false;
        }
        for (const char c : part) {
            if (!is_identifier_char(c)) {
                return false;
            }
        }
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    return true;
}

[[nodiscard]] bool parse_u32(std::string_view text, std::uint32_t& out) noexcept {
    if (text.empty() || text.size() > 10) {
        return false;
    }
    std::uint32_t value = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const std::from_chars_result result = std::from_chars(begin, end, value);
    if (result.ec != std::errc() || result.ptr != end) {
        return false;
    }
    out = value;
    return true;
}

/// Semantic Versioning 2.0.0 precedence comparison of prerelease strings.
[[nodiscard]] std::strong_ordering compare_prerelease(const std::string& a, const std::string& b) noexcept {
    if (a.empty() && b.empty()) {
        return std::strong_ordering::equal;
    }
    if (a.empty()) {
        return std::strong_ordering::greater;  // release outranks prerelease
    }
    if (b.empty()) {
        return std::strong_ordering::less;
    }
    std::size_t ai = 0;
    std::size_t bi = 0;
    while (ai < a.size() || bi < b.size()) {
        const std::size_t adot = a.find('.', ai);
        const std::size_t bdot = b.find('.', bi);
        const std::string ap = a.substr(ai, adot == std::string::npos ? std::string::npos : adot - ai);
        const std::string bp = b.substr(bi, bdot == std::string::npos ? std::string::npos : bdot - bi);
        const bool an = is_numeric_identifier(ap);
        const bool bn = is_numeric_identifier(bp);
        if (an && bn) {
            std::uint32_t av = 0;
            std::uint32_t bv = 0;
            static_cast<void>(parse_u32(ap, av));
            static_cast<void>(parse_u32(bp, bv));
            if (av != bv) {
                return av < bv ? std::strong_ordering::less : std::strong_ordering::greater;
            }
        } else if (an != bn) {
            return an ? std::strong_ordering::less : std::strong_ordering::greater;
        } else if (ap != bp) {
            return ap < bp ? std::strong_ordering::less : std::strong_ordering::greater;
        }
        if (adot == std::string::npos && bdot == std::string::npos) {
            break;
        }
        ai = adot == std::string::npos ? a.size() : adot + 1;
        bi = bdot == std::string::npos ? b.size() : bdot + 1;
        if (ai >= a.size() && bi >= b.size()) {
            break;
        }
        if (ai > a.size() || bi > b.size()) {
            // One side ran out of identifiers: the longer list has higher precedence.
            if (ai > a.size()) {
                return std::strong_ordering::less;
            }
            return std::strong_ordering::greater;
        }
    }
    return std::strong_ordering::equal;
}

[[nodiscard]] std::string trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

}  // namespace

std::optional<SemanticVersion> SemanticVersion::parse(std::string_view text) noexcept {
    try {
        if (text.empty()) {
            return std::nullopt;
        }
        std::string body(text);
        std::string build;
        const std::size_t plus = body.find('+');
        if (plus != std::string::npos) {
            build = body.substr(plus + 1);
            body = body.substr(0, plus);
            if (!valid_identifiers(build)) {
                return std::nullopt;
            }
        }
        std::string pre;
        const std::size_t dash = body.find('-');
        if (dash != std::string::npos) {
            pre = body.substr(dash + 1);
            body = body.substr(0, dash);
            if (!valid_identifiers(pre)) {
                return std::nullopt;
            }
        }
        std::uint32_t major = 0;
        std::uint32_t minor = 0;
        std::uint32_t patch = 0;
        const std::size_t first = body.find('.');
        if (first == std::string::npos) {
            return std::nullopt;
        }
        const std::size_t second = body.find('.', first + 1);
        if (second == std::string::npos) {
            return std::nullopt;
        }
        if (body.find('.', second + 1) != std::string::npos) {
            return std::nullopt;
        }
        if (!parse_u32(std::string_view(body).substr(0, first), major) ||
            !parse_u32(std::string_view(body).substr(first + 1, second - first - 1), minor) ||
            !parse_u32(std::string_view(body).substr(second + 1), patch)) {
            return std::nullopt;
        }
        SemanticVersion version(major, minor, patch);
        version.prerelease_ = pre;
        version.build_metadata_ = build;
        return version;
    } catch (...) {
        return std::nullopt;
    }
}

std::string SemanticVersion::to_string() const {
    std::string out = std::to_string(major_) + "." + std::to_string(minor_) + "." + std::to_string(patch_);
    if (!prerelease_.empty()) {
        out += "-";
        out += prerelease_;
    }
    return out;
}

std::string SemanticVersion::to_string_full() const {
    std::string out = to_string();
    if (!build_metadata_.empty()) {
        out += "+";
        out += build_metadata_;
    }
    return out;
}

bool operator==(const SemanticVersion& a, const SemanticVersion& b) noexcept {
    return (a <=> b) == std::strong_ordering::equal;
}

std::strong_ordering operator<=>(const SemanticVersion& a, const SemanticVersion& b) noexcept {
    if (a.major_ != b.major_) {
        return a.major_ < b.major_ ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    if (a.minor_ != b.minor_) {
        return a.minor_ < b.minor_ ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    if (a.patch_ != b.patch_) {
        return a.patch_ < b.patch_ ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    return compare_prerelease(a.prerelease_, b.prerelease_);
}

bool SemanticVersion::is_valid(std::string_view text) noexcept { return parse(text).has_value(); }

Result<VersionRange> VersionRange::parse(std::string_view text) {
    VersionRange range;
    std::string normalized;
    normalized.reserve(text.size());
    for (const char c : text) {
        normalized.push_back(c == ',' ? ' ' : c);
    }
    std::size_t index = 0;
    while (index < normalized.size()) {
        while (index < normalized.size() && std::isspace(static_cast<unsigned char>(normalized[index])) != 0) {
            ++index;
        }
        if (index >= normalized.size()) {
            break;
        }
        const std::size_t start = index;
        while (index < normalized.size() && std::isspace(static_cast<unsigned char>(normalized[index])) == 0) {
            ++index;
        }
        const std::string token = normalized.substr(start, index - start);
        if (token == "*" || token == "any") {
            range.clauses_.push_back(VersionClause{VersionOperator::Any, SemanticVersion{}});
            continue;
        }
        VersionOperator op = VersionOperator::Equal;
        std::size_t consumed = 0;
        if (token.rfind(">=", 0) == 0) {
            op = VersionOperator::GreaterEqual;
            consumed = 2;
        } else if (token.rfind("<=", 0) == 0) {
            op = VersionOperator::LessEqual;
            consumed = 2;
        } else if (token.rfind("!=", 0) == 0) {
            op = VersionOperator::NotEqual;
            consumed = 2;
        } else if (token.rfind("==", 0) == 0) {
            op = VersionOperator::Equal;
            consumed = 2;
        } else if (token[0] == '>') {
            op = VersionOperator::Greater;
            consumed = 1;
        } else if (token[0] == '<') {
            op = VersionOperator::Less;
            consumed = 1;
        } else if (token[0] == '=') {
            op = VersionOperator::Equal;
            consumed = 1;
        } else if (token[0] == '^') {
            op = VersionOperator::Compatible;
            consumed = 1;
        } else if (token[0] == '~') {
            op = VersionOperator::Tilde;
            consumed = 1;
        }
        const std::string version_text = token.substr(consumed);
        const std::optional<SemanticVersion> version = SemanticVersion::parse(version_text);
        if (!version.has_value()) {
            return Status(ErrorCode::MalformedVersionRange,
                          "version range clause is not a semantic version: '" + token + "'");
        }
        range.clauses_.push_back(VersionClause{op, *version});
    }
    return range;
}

bool VersionRange::matches(const SemanticVersion& version) const noexcept {
    for (const VersionClause& clause : clauses_) {
        const std::strong_ordering order = version <=> clause.version;
        bool satisfied = false;
        switch (clause.op) {
            case VersionOperator::Any: satisfied = true; break;
            case VersionOperator::Equal: satisfied = order == std::strong_ordering::equal; break;
            case VersionOperator::NotEqual: satisfied = order != std::strong_ordering::equal; break;
            case VersionOperator::Less: satisfied = order == std::strong_ordering::less; break;
            case VersionOperator::LessEqual: satisfied = order != std::strong_ordering::greater; break;
            case VersionOperator::Greater: satisfied = order == std::strong_ordering::greater; break;
            case VersionOperator::GreaterEqual: satisfied = order != std::strong_ordering::less; break;
            case VersionOperator::Compatible:
                satisfied = order != std::strong_ordering::less && version.major() == clause.version.major();
                break;
            case VersionOperator::Tilde:
                satisfied = order != std::strong_ordering::less && version.major() == clause.version.major() &&
                            version.minor() == clause.version.minor();
                break;
        }
        if (!satisfied) {
            return false;
        }
    }
    return true;
}

std::string VersionRange::to_string() const {
    if (clauses_.empty()) {
        return "*";
    }
    std::string out;
    for (std::size_t i = 0; i < clauses_.size(); ++i) {
        if (i != 0) {
            out += " ";
        }
        if (clauses_[i].op == VersionOperator::Any) {
            out += "*";
            continue;
        }
        switch (clauses_[i].op) {
            case VersionOperator::Equal: out += "="; break;
            case VersionOperator::NotEqual: out += "!="; break;
            case VersionOperator::Less: out += "<"; break;
            case VersionOperator::LessEqual: out += "<="; break;
            case VersionOperator::Greater: out += ">"; break;
            case VersionOperator::GreaterEqual: out += ">="; break;
            case VersionOperator::Compatible: out += "^"; break;
            case VersionOperator::Tilde: out += "~"; break;
        }
        out += clauses_[i].version.to_string();
    }
    return out;
}

}  // namespace haf
