#include "services/portal_storage/portal_store.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
      std::cout << "chronicles-content put store|community ENTRY.json\n"
                   "chronicles-content withdraw store|community ID\n"
                   "chronicles-content list store|community\n"
                   "Uses CHRONICLES_DATABASE_URL. Apply server migrations first.\n";
      return 0;
    }
    const std::string command = argc > 1 ? argv[1] : "";
    if ((command == "list" && argc != 3) ||
        ((command == "put" || command == "withdraw") && argc != 4) ||
        (command != "put" && command != "withdraw" && command != "list")) {
      throw std::invalid_argument("Use --help for content commands");
    }
    const auto *connection = std::getenv("CHRONICLES_DATABASE_URL");
    if (!connection || std::string_view(connection).empty()) {
      throw std::invalid_argument("Set CHRONICLES_DATABASE_URL to the backend database");
    }
    chronicles::PortalStore content(connection);
    if (command == "list") {
      std::cout << content.list(argv[2], true).dump(2) << '\n';
    } else if (command == "put") {
      if (std::filesystem::file_size(argv[3]) > 100000) {
        throw std::invalid_argument("Content file exceeds 100000 bytes");
      }
      std::ifstream input(argv[3]);
      if (!input) {
        throw std::invalid_argument("Content file cannot be opened");
      }
      content.put(argv[2], nlohmann::json::parse(input));
      std::cout << "Content saved.\n";
    } else {
      if (!content.withdraw(argv[2], argv[3])) {
        throw std::invalid_argument("Content id does not exist");
      }
      std::cout << "Content withdrawn; draft retained.\n";
    }
    return 0;
  } catch (const std::invalid_argument &error) {
    std::cerr << error.what() << '\n';
  } catch (const nlohmann::json::exception &) {
    std::cerr << "Invalid content JSON. Check the supplied templates and field types.\n";
  } catch (const std::exception &) {
    std::cerr << "Content command failed. Check database access and migrations.\n";
  }
  return 1;
}
