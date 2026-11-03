#include "services/news/news.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
      std::cout << "chronicles-news put ARTICLE.json | withdraw ID | list\n"
                   "Uses CHRONICLES_DATABASE_URL; apply the server migration first.\n"
                   "put creates/updates by id; published:false saves a draft.\n";
      return 0;
    }
    const std::string command = argc > 1 ? argv[1] : "";
    if ((command == "list" && argc != 2) ||
        ((command == "put" || command == "withdraw") && argc != 3) ||
        (command != "list" && command != "put" && command != "withdraw")) {
      throw std::invalid_argument("Use --help for news commands");
    }
    const auto *connection = std::getenv("CHRONICLES_DATABASE_URL");
    if (!connection || std::string_view(connection).empty()) {
      throw std::invalid_argument("Set CHRONICLES_DATABASE_URL to the server database");
    }
    chronicles::NewsStore news(connection);
    if (command == "list") {
      std::cout << news.list(true).dump(2) << '\n';
    } else if (command == "put") {
      constexpr std::uintmax_t kMaximumArticleBytes = 100000;
      if (std::filesystem::file_size(argv[2]) > kMaximumArticleBytes) {
        throw std::invalid_argument("Article file exceeds 100000 bytes");
      }
      std::ifstream input(argv[2]);
      if (!input) {
        throw std::invalid_argument("Article file cannot be opened");
      }
      news.put(nlohmann::json::parse(input));
      std::cout << "Article saved.\n";
    } else {
      if (!news.withdraw(argv[2])) {
        throw std::invalid_argument("Article id does not exist");
      }
      std::cout << "Article withdrawn; draft retained.\n";
    }
    return 0;
  } catch (const std::invalid_argument &error) {
    std::cerr << error.what() << '\n';
    return 1;
  } catch (const nlohmann::json::exception &) {
    std::cerr
        << "Invalid article JSON. Required fields: id,category,title,summary,body,art,published.\n";
    return 1;
  } catch (const std::exception &) {
    // Database exceptions may contain connection details; do not disclose credentials.
    std::cerr << "News command failed. Check article fields, database access and migrations.\n";
    return 1;
  }
}
