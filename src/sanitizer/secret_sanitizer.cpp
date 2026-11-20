#include "agmesh/sanitizer/secret_sanitizer.hpp"
#include "agmesh/core/hlc_crdt.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <regex>
#include <utility>

namespace agmesh::sanitizer {

namespace {

struct PatternSpec {
    const char* secret_type;
    std::regex pattern;
    int capture_group; // 0 = full match, 1 = inner secret group
};

const std::vector<PatternSpec>& known_patterns() {
    static const std::vector<PatternSpec> kPatterns = {
        {"AWS_ACCESS_KEY", std::regex(R"((?:AKIA|ASIA|AROA|AIDA)[0-9A-Z]{16})"), 0},
        {"AWS_SECRET_KEY", std::regex(R"((?:aws_secret_access_key|AWS_SECRET_ACCESS_KEY)\s*[:=]\s*['"]?([A-Za-z0-9/+=]{40})['"]?)"), 1},
        {"GITHUB_TOKEN", std::regex(R"(\b(?:ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9]{20,255}\b|github_pat_[A-Za-z0-9_]{22,255})"), 0},
        {"OPENAI_KEY", std::regex(R"(\bsk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{20,160}\b)"), 0},
        {"ANTHROPIC_KEY", std::regex(R"(\bsk-ant-[A-Za-z0-9_-]{20,160}\b)"), 0},
        {"GOOGLE_API_KEY", std::regex(R"(\bAIza[0-9A-Za-z_-]{35}\b)"), 0},
        {"TELEGRAM_BOT_TOKEN", std::regex(R"(\b[0-9]{8,12}:AA[A-Za-z0-9_-]{30,40}\b)"), 0},
        {"JWT", std::regex(R"(\beyJ[A-Za-z0-9_-]{10,}\.eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\b)"), 0},
        {"SLACK_TOKEN", std::regex(R"(\bxox[baprs]-[0-9A-Za-z-]{10,64}\b)"), 0},
        {"BEARER_TOKEN", std::regex(R"((?:Authorization\s*:\s*Bearer\s+)([A-Za-z0-9._-]{20,}))"), 1},
    };
    return kPatterns;
}

bool is_token_char(char ch) noexcept {
    const auto uch = static_cast<unsigned char>(ch);
    return std::isalnum(uch) || ch == '_' || ch == '-' || ch == '+' || ch == '/' || ch == '=' || ch == '.';
}

bool looks_like_file_path_or_hash(const std::string& tok) {
    // Do not flag pure lowercase/hex SHA-256 digests or file paths with multiple dots/slashes.
    if (tok.find('/') != std::string::npos && tok.find("://") == std::string::npos) {
        std::size_t slash_count = static_cast<std::size_t>(std::count(tok.begin(), tok.end(), '/'));
        if (slash_count >= 2) return true;
    }
    if (tok.rfind("agmesh://", 0) == 0 || tok.rfind("file://", 0) == 0) return true;
    // Pure hex strings of exact SHA-256 length (64 chars) inside commit references should not be flagged as secrets
    // unless prefixed with secret context.
    if (tok.size() == 64 && std::all_of(tok.begin(), tok.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) {
        return true;
    }
    return false;
}

} // namespace

SecretSanitizer::SecretSanitizer(SanitizerOptions options)
    : options_(options) {}

double SecretSanitizer::shannon_entropy(const std::string& token) {
    if (token.empty()) return 0.0;
    std::array<std::size_t, 256> counts{};
    for (unsigned char ch : token) {
        ++counts[ch];
    }
    double entropy = 0.0;
    const double len = static_cast<double>(token.size());
    for (std::size_t c : counts) {
        if (c == 0) continue;
        const double p = static_cast<double>(c) / len;
        entropy -= p * std::log2(p);
    }
    return entropy;
}

std::string SecretSanitizer::format_marker(const std::string& secret_type,
                                           const std::string& raw_secret) {
    const std::string digest = core::CrdtResolver::sha256_hex(raw_secret);
    std::string hash4 = digest.size() >= 4 ? digest.substr(0, 4) : "0000";
    for (char& ch : hash4) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return "[REDACTED:" + secret_type + ":" + hash4 + "]";
}

SanitizeResult SecretSanitizer::sanitize(const std::string& input) const {
    SanitizeResult result;
    result.sanitized_text = input;
    if (!options_.enabled || input.empty()) {
        return result;
    }

    struct SpanReplacement {
        std::size_t start;
        std::size_t end;
        std::string secret_type;
        std::string raw;
    };
    std::vector<SpanReplacement> spans;

    // 1. Multi-line PEM private keys (SSH, RSA, EC, OpenSSH, PGP)
    if (options_.redact_private_keys) {
        static const std::regex kPemRegex(
            R"(-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----[\s\S]*?-----END [A-Z0-9 ]*PRIVATE KEY-----)");
        auto it = std::sregex_iterator(input.begin(), input.end(), kPemRegex);
        auto end_it = std::sregex_iterator();
        for (; it != end_it; ++it) {
            const auto pos = static_cast<std::size_t>(it->position());
            const auto len = static_cast<std::size_t>(it->length());
            spans.push_back({pos, pos + len, "SSH_PRIVATE_KEY", it->str()});
        }
    }

    // 2. Database connection strings with embedded credentials
    if (options_.redact_connection_strings) {
        static const std::regex kDbUriRegex(
            R"(\b(?:postgres|postgresql|mysql|mariadb|mongodb(?:\+srv)?|redis|rediss|amqp|mssql)://[^:\s/@]+:([^@\s]+)@[^\s'"]+)");
        auto it = std::sregex_iterator(input.begin(), input.end(), kDbUriRegex);
        auto end_it = std::sregex_iterator();
        for (; it != end_it; ++it) {
            if (it->size() > 1) {
                const auto pos = static_cast<std::size_t>(it->position(1));
                const auto len = static_cast<std::size_t>(it->length(1));
                spans.push_back({pos, pos + len, "DB_PASSWORD", it->str(1)});
            }
        }
    }

    // 3. Known regex secret signatures
    for (const auto& spec : known_patterns()) {
        auto it = std::sregex_iterator(input.begin(), input.end(), spec.pattern);
        auto end_it = std::sregex_iterator();
        for (; it != end_it; ++it) {
            const int grp = spec.capture_group;
            if (grp >= 0 && static_cast<std::size_t>(grp) < it->size()) {
                const auto pos = static_cast<std::size_t>(it->position(grp));
                const auto len = static_cast<std::size_t>(it->length(grp));
                spans.push_back({pos, pos + len, spec.secret_type, it->str(grp)});
            }
        }
    }

    // 4. High-entropy unknown token scan (skips ranges already matched)
    auto overlaps_existing = [&](std::size_t s, std::size_t e) {
        return std::any_of(spans.begin(), spans.end(), [&](const SpanReplacement& sp) {
            return !(e <= sp.start || s >= sp.end);
        });
    };

    std::size_t idx = 0;
    while (idx < input.size()) {
        if (!is_token_char(input[idx])) {
            ++idx;
            continue;
        }
        const std::size_t tok_start = idx;
        while (idx < input.size() && is_token_char(input[idx])) {
            ++idx;
        }
        const std::size_t tok_len = idx - tok_start;
        if (tok_len < options_.entropy_min_length || tok_len > 256) {
            continue;
        }
        if (overlaps_existing(tok_start, idx)) {
            continue;
        }
        const std::string token = input.substr(tok_start, tok_len);
        if (looks_like_file_path_or_hash(token)) {
            continue;
        }
        // Require character class diversity (digits + letters) to avoid redacting long camelCase identifiers.
        bool has_upper = false;
        bool has_lower = false;
        bool has_digit = false;
        for (unsigned char c : token) {
            if (std::isupper(c)) has_upper = true;
            if (std::islower(c)) has_lower = true;
            if (std::isdigit(c)) has_digit = true;
        }
        if (!has_digit || (!has_upper && !has_lower)) {
            continue;
        }
        if (shannon_entropy(token) >= options_.entropy_threshold) {
            spans.push_back({tok_start, idx, "HIGH_ENTROPY_SECRET", token});
        }
    }

    if (spans.empty()) {
        return result;
    }

    // Sort and merge non-overlapping spans (preferring earlier / more specific regex matches)
    std::sort(spans.begin(), spans.end(), [](const SpanReplacement& a, const SpanReplacement& b) {
        if (a.start != b.start) return a.start < b.start;
        return a.end > b.end;
    });

    std::vector<SpanReplacement> filtered;
    for (const auto& sp : spans) {
        if (filtered.empty() || sp.start >= filtered.back().end) {
            filtered.push_back(sp);
        }
    }

    // Build final redacted text
    std::string out;
    out.reserve(input.size());
    std::size_t cursor = 0;
    for (const auto& sp : filtered) {
        if (sp.start > cursor) {
            out.append(input, cursor, sp.start - cursor);
        }
        const std::string marker = format_marker(sp.secret_type, sp.raw);
        result.events.push_back({sp.secret_type, marker, sp.start, sp.end - sp.start});
        out.append(marker);
        cursor = sp.end;
    }
    if (cursor < input.size()) {
        out.append(input, cursor, input.size() - cursor);
    }

    result.sanitized_text = std::move(out);
    return result;
}

void SecretSanitizer::update_options(SanitizerOptions options) {
    options_ = options;
}

SanitizerOptions SecretSanitizer::options() const noexcept {
    return options_;
}

} // namespace agmesh::sanitizer
