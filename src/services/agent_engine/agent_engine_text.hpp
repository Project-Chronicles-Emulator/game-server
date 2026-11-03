#pragma once

// Text helpers shared by the agent engine's knowledge search, dialogue models and request
// validation. Agent-engine text is UTF-8; keyword matching only folds ASCII letters, so
// non-ASCII text is preserved and compared byte for byte.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace chronicles::agent_engine {

// ASCII lowercase copy. Bytes outside A-Z are unchanged.
std::string ascii_lower(std::string_view text);

// Lowercase word tokens: runs of ASCII letters/digits, or of non-ASCII bytes (so Hangul and
// other UTF-8 words still form tokens). Very common English function words are dropped.
std::vector<std::string> keyword_tokens(std::string_view text);

// True when `text` contains `word` as a whole keyword token (case-insensitive ASCII).
bool has_keyword(std::string_view text, std::string_view word);

// Removes leading/trailing ASCII whitespace.
std::string_view trim(std::string_view text);

// At most `maximum_bytes` bytes of valid UTF-8 `text`, never splitting a code point. When text
// is cut, the result ends with "..." inside the same byte budget.
std::string truncate_utf8(std::string_view text, std::size_t maximum_bytes);

// The first sentence of `text` (up to and including '.', '!' or '?'), trimmed and bounded.
std::string first_sentence(std::string_view text, std::size_t maximum_bytes);

// True for valid UTF-8 without control characters other than tab and newline.
bool is_clean_utf8(std::string_view text);

} // namespace chronicles::agent_engine
