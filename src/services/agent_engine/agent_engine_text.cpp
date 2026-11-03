#include "services/agent_engine/agent_engine_text.hpp"

#include "world/features/guild/guild.hpp"

#include <algorithm>
#include <array>

namespace chronicles::agent_engine {
namespace {

constexpr std::string_view kEllipsis = "...";

// Words too common to carry meaning in a keyword match or a knowledge query.
constexpr std::array<std::string_view, 48> kStopWords{
    "a",    "an",   "and",  "are",   "as",    "at",   "be",   "but",   "by",   "can", "do",   "for",
    "from", "have", "how",  "i",     "in",    "is",   "it",   "me",    "my",   "of",  "on",   "or",
    "our",  "so",   "that", "the",   "their", "them", "then", "there", "this", "to",  "was",  "we",
    "were", "what", "when", "where", "which", "who",  "why",  "will",  "with", "you", "your", "am"};

bool is_ascii_word_byte(unsigned char byte) {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9');
}

bool is_non_ascii_byte(unsigned char byte) {
  return byte >= 0x80;
}

bool is_stop_word(std::string_view word) {
  return std::ranges::find(kStopWords, word) != kStopWords.end();
}

bool is_continuation_byte(unsigned char byte) {
  return (byte & 0xC0) == 0x80;
}

// Largest prefix length <= limit that ends on a code point boundary.
std::size_t code_point_boundary(std::string_view text, std::size_t limit) {
  auto length = std::min(limit, text.size());
  while (length > 0 && length < text.size() &&
         is_continuation_byte(static_cast<unsigned char>(text[length]))) {
    --length;
  }
  return length;
}

} // namespace

std::string ascii_lower(std::string_view text) {
  std::string lowered(text);
  for (auto &character : lowered) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return lowered;
}

namespace {

std::vector<std::string> word_tokens(std::string_view text, bool drop_stop_words) {
  std::vector<std::string> tokens;
  std::size_t position = 0;
  while (position < text.size()) {
    const auto byte = static_cast<unsigned char>(text[position]);
    const bool ascii_word = is_ascii_word_byte(byte);
    const bool non_ascii_word = is_non_ascii_byte(byte);
    if (!ascii_word && !non_ascii_word) {
      ++position;
      continue;
    }

    // A token is a run of the same byte class, so "Seren's" yields "seren" and "s".
    const auto start = position;
    while (position < text.size()) {
      const auto next = static_cast<unsigned char>(text[position]);
      const bool same_class = ascii_word ? is_ascii_word_byte(next) : is_non_ascii_byte(next);
      if (!same_class) {
        break;
      }
      ++position;
    }

    auto token = ascii_lower(text.substr(start, position - start));
    if (drop_stop_words && (token.size() < 2 || is_stop_word(token))) {
      continue;
    }
    tokens.push_back(std::move(token));
  }
  return tokens;
}

} // namespace

std::vector<std::string> keyword_tokens(std::string_view text) {
  return word_tokens(text, true);
}

bool has_keyword(std::string_view text, std::string_view word) {
  const auto lowered_word = ascii_lower(word);
  const auto tokens = word_tokens(text, false);
  return std::ranges::find(tokens, lowered_word) != tokens.end();
}

std::string_view trim(std::string_view text) {
  constexpr std::string_view kWhitespace = " \t\r\n";
  const auto first = text.find_first_not_of(kWhitespace);
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(kWhitespace);
  return text.substr(first, last - first + 1);
}

std::string truncate_utf8(std::string_view text, std::size_t maximum_bytes) {
  if (text.size() <= maximum_bytes) {
    return std::string(text);
  }
  if (maximum_bytes <= kEllipsis.size()) {
    return std::string(text.substr(0, code_point_boundary(text, maximum_bytes)));
  }

  const auto kept = code_point_boundary(text, maximum_bytes - kEllipsis.size());
  std::string result(trim(text.substr(0, kept)));
  result += kEllipsis;
  return result;
}

std::string first_sentence(std::string_view text, std::size_t maximum_bytes) {
  const auto trimmed = trim(text);
  const auto end = trimmed.find_first_of(".!?");
  const auto sentence = end == std::string_view::npos ? trimmed : trimmed.substr(0, end + 1);
  return truncate_utf8(sentence, maximum_bytes);
}

bool is_clean_utf8(std::string_view text) {
  if (!utf8_code_points(text)) {
    return false;
  }
  return std::ranges::none_of(text, [](char character) {
    const auto byte = static_cast<unsigned char>(character);
    const bool control = byte < 0x20 && byte != '\t' && byte != '\n';
    return control || byte == 0x7F;
  });
}

} // namespace chronicles::agent_engine
