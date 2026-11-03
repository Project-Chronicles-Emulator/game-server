#pragma once

// World-knowledge retrieval for POST /search/knowledge-hybrid.
//
// The client (AAC_AIChat_FilterKnowledgeResults) splits every SearchKnowledgeRes
// query_result on ", Distance:" and keeps a result only when it has exactly two parts and the
// distance is a decimal number not above AIChatSearchKnowledgeDistanceThreshold (0.7). Smaller
// distances are better. The official service's ranking model is unknown. This index scores a
// document by IDF-weighted keyword coverage of the query plus character-trigram overlap (a
// lexical stand-in for the "hybrid" sparse/dense retrieval the route name suggests) and reports
// distance = 1 - score.

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace chronicles::agent_engine {

// Separator the client splits on. Knowledge text containing it is rejected at load time.
inline constexpr std::string_view kDistanceSeparator = ", Distance:";

struct KnowledgeHit {
  std::string text;
  // 0 is an exact match, 1 is unrelated.
  double distance{};
};

class KnowledgeIndex {
public:
  KnowledgeIndex() = default;

  // Throws std::invalid_argument for empty text, text containing kDistanceSeparator or text
  // that is not clean UTF-8.
  explicit KnowledgeIndex(std::vector<std::string> documents);

  // At most `limit` documents with a positive score, best first. Ties keep document order.
  std::vector<KnowledgeHit> search(std::string_view query, std::size_t limit) const;

  std::size_t size() const;

private:
  struct Document {
    std::string text;
    std::unordered_set<std::string> terms;
    std::unordered_set<std::string> trigrams;
  };

  double inverse_document_frequency(const std::string &term) const;

  std::vector<Document> documents_;
  std::unordered_map<std::string, std::size_t> document_frequency_;
};

// "text, Distance:0.1234": the wire form the client parses.
std::string format_knowledge_result(const KnowledgeHit &hit);

} // namespace chronicles::agent_engine
