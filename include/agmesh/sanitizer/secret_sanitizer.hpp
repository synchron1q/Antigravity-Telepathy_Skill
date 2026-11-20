#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace agmesh::sanitizer {

struct RedactionEvent {
    std::string secret_type;
    std::string marker;
    std::size_t offset{0};
    std::size_t length{0};
};

struct SanitizeResult {
    std::string sanitized_text;
    std::vector<RedactionEvent> events;

    [[nodiscard]] bool had_redactions() const noexcept { return !events.empty(); }
};

struct SanitizerOptions {
    bool enabled{true};
    double entropy_threshold{4.5};
    std::size_t entropy_min_length{20};
    bool redact_private_keys{true};
    bool redact_connection_strings{true};
};

class SecretSanitizer {
public:
    explicit SecretSanitizer(SanitizerOptions options = {});

    [[nodiscard]] SanitizeResult sanitize(const std::string& input) const;

    // Computes Shannon entropy in bits per character.
    [[nodiscard]] static double shannon_entropy(const std::string& token);

    // Formats deterministic marker [REDACTED:SECRET_TYPE:HASH4].
    [[nodiscard]] static std::string format_marker(const std::string& secret_type,
                                                   const std::string& raw_secret);

    void update_options(SanitizerOptions options);
    [[nodiscard]] SanitizerOptions options() const noexcept;

private:
    SanitizerOptions options_;
};

} // namespace agmesh::sanitizer
