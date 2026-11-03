#include "services/agent_engine/agent_engine_knowledge.hpp"

#include "services/agent_engine/agent_engine_text.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace chronicles::agent_engine {
namespace {

// Keyword coverage dominates; trigram overlap rescues inflections and partial words
// ("houses" against "housemate") that whole-token matching misses.
constexpr double kKeywordWeight = 0.7;
constexpr double kTrigramWeight = 0.3;
constexpr std::size_t kTrigramLength = 3;

// Lowercased text where every run of non-word bytes becomes one space.
std::string normalized_for_trigrams(std::string_view text) {
  std::string normalized;
  normalized.reserve(text.size());
  bool previous_space = true;
  for (const char character : ascii_lower(text)) {
    const auto byte = static_cast<unsigned char>(character);
    const bool word_byte =
        (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte >= 0x80;
    if (word_byte) {
      normalized.push_back(character);
      previous_space = false;
    } else if (!previous_space) {
      normalized.push_back(' ');
      previous_space = true;
    }
  }
  return normalized;
}

std::unordered_set<std::string> trigrams_of(std::string_view text) {
  const auto normalized = normalized_for_trigrams(text);
  std::unordered_set<std::string> trigrams;
  if (normalized.size() < kTrigramLength) {
    return trigrams;
  }
  for (std::size_t start = 0; start + kTrigramLength <= normalized.size(); ++start) {
    trigrams.insert(normalized.substr(start, kTrigramLength));
  }
  return trigrams;
}

std::unordered_set<std::string> terms_of(std::string_view text) {
  const auto tokens = keyword_tokens(text);
  return {tokens.begin(), tokens.end()};
}

} // namespace

KnowledgeIndex::KnowledgeIndex(std::vector<std::string> documents) {
  documents_.reserve(documents.size());
  for (auto &text : documents) {
    if (trim(text).empty()) {
      throw std::invalid_argument("Knowledge entries must not be empty");
    }
    if (text.find(kDistanceSeparator) != std::string::npos) {
      throw std::invalid_argument("Knowledge entries must not contain the distance separator");
    }
    if (!is_clean_utf8(text)) {
      throw std::invalid_argument("Knowledge entries must be clean UTF-8");
    }

    Document document;
    document.terms = terms_of(text);
    document.trigrams = trigrams_of(text);
    document.text = std::move(text);
    for (const auto &term : document.terms) {
      ++document_frequency_[term];
    }
    documents_.push_back(std::move(document));
  }
}

double KnowledgeIndex::inverse_document_frequency(const std::string &term) const {
  const auto found = document_frequency_.find(term);
  const double frequency = found == document_frequency_.end() ? 0.0 : double(found->second);
  // A term absent from every document still has weight, so unknown topics lower coverage.
  return std::log((double(documents_.size()) + 1.0) / (frequency + 0.5));
}

std::vector<KnowledgeHit> KnowledgeIndex::search(std::string_view query, std::size_t limit) const {
  const auto query_terms = terms_of(query);
  const auto query_trigrams = trigrams_of(query);
  if (limit == 0 || (query_terms.empty() && query_trigrams.empty())) {
    return {};
  }

  double total_weight = 0.0;
  for (const auto &term : query_terms) {
    total_weight += inverse_document_frequency(term);
  }

  struct Scored {
    std::size_t index{};
    double score{};
  };
  std::vector<Scored> scored;
  for (std::size_t index = 0; index < documents_.size(); ++index) {
    const auto &document = documents_[index];

    double matched_weight = 0.0;
    for (const auto &term : query_terms) {
      if (document.terms.contains(term)) {
        matched_weight += inverse_document_frequency(term);
      }
    }
    const double coverage = total_weight > 0.0 ? matched_weight / total_weight : 0.0;

    std::size_t shared_trigrams = 0;
    for (const auto &trigram : query_trigrams) {
      if (document.trigrams.contains(trigram)) {
        ++shared_trigrams;
      }
    }
    const double containment =
        query_trigrams.empty() ? 0.0 : double(shared_trigrams) / double(query_trigrams.size());

    const double score = kKeywordWeight * coverage + kTrigramWeight * containment;
    if (score > 0.0) {
      scored.push_back({index, score});
    }
  }

  std::ranges::stable_sort(
      scored, [](const Scored &left, const Scored &right) { return left.score > right.score; });
  if (scored.size() > limit) {
    scored.resize(limit);
  }

  std::vector<KnowledgeHit> hits;
  hits.reserve(scored.size());
  for (const auto &entry : scored) {
    const double distance = std::clamp(1.0 - entry.score, 0.0, 1.0);
    hits.push_back({documents_[entry.index].text, distance});
  }
  return hits;
}

std::size_t KnowledgeIndex::size() const {
  return documents_.size();
}

std::string format_knowledge_result(const KnowledgeHit &hit) {
  std::ostringstream text;
  text.imbue(std::locale::classic());
  text << hit.text << kDistanceSeparator << std::fixed << std::setprecision(4) << hit.distance;
  return text.str();
}

} // namespace chronicles::agent_engine
