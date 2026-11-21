#include "semver.h"

#include <cctype>
#include <cstdlib>
#include <vector>

namespace mocha {
namespace {

std::string trim(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

bool parse_number(std::string_view text, int& out, size_t& consumed) {
    if (text.empty() || !std::isdigit(static_cast<unsigned char>(text[0]))) {
        return false;
    }
    long value = 0;
    size_t index = 0;
    while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index]))) {
        value = value * 10 + (text[index] - '0');
        if (value > 1000000) {
            return false;
        }
        ++index;
    }
    out = static_cast<int>(value);
    consumed = index;
    return true;
}

std::vector<std::string> split_dots(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : text) {
        if (c == '.') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    return parts;
}

int compare_prerelease(const std::string& a, const std::string& b) {
    if (a.empty() && b.empty()) {
        return 0;
    }
    if (a.empty()) {
        return 1;
    }
    if (b.empty()) {
        return -1;
    }

    const auto left = split_dots(a);
    const auto right = split_dots(b);
    const size_t count = left.size() < right.size() ? left.size() : right.size();

    for (size_t i = 0; i < count; ++i) {
        const bool left_numeric = !left[i].empty() &&
            left[i].find_first_not_of("0123456789") == std::string::npos;
        const bool right_numeric = !right[i].empty() &&
            right[i].find_first_not_of("0123456789") == std::string::npos;

        if (left_numeric && right_numeric) {
            const long l = std::strtol(left[i].c_str(), nullptr, 10);
            const long r = std::strtol(right[i].c_str(), nullptr, 10);
            if (l != r) {
                return l < r ? -1 : 1;
            }
            continue;
        }
        if (left_numeric != right_numeric) {
            return left_numeric ? -1 : 1;
        }
        if (left[i] != right[i]) {
            return left[i] < right[i] ? -1 : 1;
        }
    }

    if (left.size() == right.size()) {
        return 0;
    }
    return left.size() < right.size() ? -1 : 1;
}

struct Comparator {
    enum Kind { Eq, Gt, Ge, Lt, Le, Caret, Tilde, Wildcard } kind = Eq;
    Version version;
    Version upper;
};

bool parse_comparator(std::string_view token, Comparator& out, std::string* error) {
    const std::string trimmed = trim(token);
    token = std::string_view(trimmed);
    if (token.empty()) {
        if (error) {
            *error = "empty version comparator";
        }
        return false;
    }

    Comparator result;
    std::string_view rest = token;
    bool has_operator = false;
    if (token.rfind(">=", 0) == 0) {
        result.kind = Comparator::Ge;
        rest = token.substr(2);
        has_operator = true;
    } else if (token.rfind("<=", 0) == 0) {
        result.kind = Comparator::Le;
        rest = token.substr(2);
        has_operator = true;
    } else if (token.rfind(">", 0) == 0) {
        result.kind = Comparator::Gt;
        rest = token.substr(1);
        has_operator = true;
    } else if (token.rfind("<", 0) == 0) {
        result.kind = Comparator::Lt;
        rest = token.substr(1);
        has_operator = true;
    } else if (token.rfind("=", 0) == 0) {
        result.kind = Comparator::Eq;
        rest = token.substr(1);
        has_operator = true;
    } else if (token.rfind("^", 0) == 0) {
        result.kind = Comparator::Caret;
        rest = token.substr(1);
        has_operator = true;
    } else if (token.rfind("~", 0) == 0) {
        result.kind = Comparator::Tilde;
        rest = token.substr(1);
        has_operator = true;
    }

    if (!has_operator && rest.find_first_of("xX*") != std::string_view::npos) {
        const auto parts = split_dots(std::string(rest));
        if (parts.size() > 3) {
            if (error) {
                *error = "invalid wildcard version '" + std::string(rest) + "'";
            }
            return false;
        }
        size_t wildcard = parts.size();
        int numbers[3] = {0, 0, 0};
        for (size_t i = 0; i < parts.size(); ++i) {
            if (parts[i] == "x" || parts[i] == "X" || parts[i] == "*") {
                wildcard = i;
                for (size_t j = i + 1; j < parts.size(); ++j) {
                    if (parts[j] != "x" && parts[j] != "X" && parts[j] != "*") {
                        if (error) {
                            *error = "wildcard must be the trailing component in '" +
                                     std::string(rest) + "'";
                        }
                        return false;
                    }
                }
                break;
            }
            size_t consumed = 0;
            if (!parse_number(parts[i], numbers[i], consumed) || consumed != parts[i].size()) {
                if (error) {
                    *error = "invalid wildcard version '" + std::string(rest) + "'";
                }
                return false;
            }
        }

        result.kind = Comparator::Wildcard;
        result.version = Version{numbers[0], numbers[1], numbers[2], "", true};
        if (wildcard == 0) {
            result.upper = Version{1000000, 0, 0, "", true};
        } else if (wildcard == 1) {
            result.upper = Version{numbers[0] + 1, 0, 0, "", true};
        } else {
            result.upper = Version{numbers[0], numbers[1] + 1, 0, "", true};
        }
        out = result;
        return true;
    }

    if (!parse_version(rest, result.version)) {
        if (error) {
            *error = "invalid version '" + std::string(rest) + "'";
        }
        return false;
    }
    out = result;
    return true;
}

bool matches(const Version& version, const Comparator& comparator) {
    const int order = compare_versions(version, comparator.version);
    Version upper;

    switch (comparator.kind) {
    case Comparator::Eq:
        return order == 0;
    case Comparator::Gt:
        return order > 0;
    case Comparator::Ge:
        return order >= 0;
    case Comparator::Lt:
        return order < 0;
    case Comparator::Le:
        return order <= 0;
    case Comparator::Caret:
        if (order < 0) {
            return false;
        }
        if (comparator.version.major > 0) {
            upper = Version{comparator.version.major + 1, 0, 0, "", true};
        } else if (comparator.version.minor > 0) {
            upper = Version{0, comparator.version.minor + 1, 0, "", true};
        } else {
            upper = Version{0, 0, comparator.version.patch + 1, "", true};
        }
        return compare_versions(version, upper) < 0;
    case Comparator::Tilde:
        if (order < 0) {
            return false;
        }
        upper = Version{comparator.version.major, comparator.version.minor + 1, 0, "", true};
        return compare_versions(version, upper) < 0;
    case Comparator::Wildcard:
        return order >= 0 && compare_versions(version, comparator.upper) < 0;
    }
    return false;
}

bool matches_all(const Version& version, std::string_view alternative, std::string* error) {
    std::string text(alternative);
    for (char& c : text) {
        if (c == ',') {
            c = ' ';
        }
    }

    size_t index = 0;
    bool saw_comparator = false;
    while (index < text.size()) {
        while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index]))) {
            ++index;
        }
        if (index >= text.size()) {
            break;
        }

        size_t end = index;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) {
            ++end;
        }

        std::string token = text.substr(index, end - index);
        index = end;

        if (token == "*" || token == "x" || token == "X") {
            saw_comparator = true;
            continue;
        }

        Comparator comparator;
        if (!parse_comparator(token, comparator, error)) {
            return false;
        }
        saw_comparator = true;
        if (!matches(version, comparator)) {
            return false;
        }
    }

    if (!saw_comparator) {
        return false;
    }
    return true;
}

} // namespace

bool parse_version(std::string_view text, Version& out) {
    const std::string value = trim(text);
    if (value.empty()) {
        return false;
    }

    std::string_view body(value);
    const auto plus = body.find('+');
    if (plus != std::string_view::npos) {
        body = body.substr(0, plus);
    }
    const auto dash = body.find('-');
    std::string prerelease;
    if (dash != std::string_view::npos) {
        prerelease = std::string(body.substr(dash + 1));
        body = body.substr(0, dash);
        if (prerelease.empty()) {
            return false;
        }
    }

    Version parsed;
    size_t index = 0;
    if (!parse_number(body.substr(index), parsed.major, index)) {
        return false;
    }
    int components = 1;
    while (index < body.size() && body[index] == '.') {
        ++index;
        int number = 0;
        size_t consumed = 0;
        if (!parse_number(body.substr(index), number, consumed)) {
            return false;
        }
        index += consumed;
        if (components == 1) {
            parsed.minor = number;
        } else if (components == 2) {
            parsed.patch = number;
        } else {
            return false;
        }
        ++components;
    }
    if (index != body.size()) {
        return false;
    }

    parsed.prerelease = prerelease;
    parsed.valid = true;
    out = parsed;
    return true;
}

int compare_versions(const Version& a, const Version& b) {
    if (a.major != b.major) {
        return a.major < b.major ? -1 : 1;
    }
    if (a.minor != b.minor) {
        return a.minor < b.minor ? -1 : 1;
    }
    if (a.patch != b.patch) {
        return a.patch < b.patch ? -1 : 1;
    }
    return compare_prerelease(a.prerelease, b.prerelease);
}

bool satisfies(const Version& version, std::string_view constraint, std::string* error) {
    if (!version.valid) {
        if (error) {
            *error = "mod version is not valid semver";
        }
        return false;
    }

    const std::string text = trim(constraint);
    if (text.empty() || text == "*") {
        return true;
    }

    size_t start = 0;
    while (start <= text.size()) {
        const size_t next = text.find("||", start);
        const std::string_view alternative = next == std::string::npos
            ? std::string_view(text).substr(start)
            : std::string_view(text).substr(start, next - start);
        if (matches_all(version, alternative, error)) {
            return true;
        }
        if (next == std::string::npos) {
            break;
        }
        start = next + 2;
    }

    if (error) {
        *error = "version does not satisfy '" + std::string(constraint) + "'";
    }
    return false;
}

} // namespace mocha
