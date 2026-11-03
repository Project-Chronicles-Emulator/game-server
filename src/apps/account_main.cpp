#include "auth/auth.hpp"
#include "persistence/character_repositories.hpp"

#include <openssl/crypto.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::string database_connection() {
  const char *value = std::getenv("CHRONICLES_DATABASE_URL");
  if (value == nullptr || *value == '\0') {
    throw std::invalid_argument("CHRONICLES_DATABASE_URL is required");
  }

  return value;
}

std::string read_password() {
  std::string password;
  if (!std::getline(std::cin, password)) {
    throw std::runtime_error("Expected one password line on standard input");
  }

  if (!password.empty() && password.back() == '\r') {
    password.pop_back();
  }

  return password;
}

struct PasswordClearer {
  std::string &password;

  ~PasswordClearer() {
    OPENSSL_cleanse(password.data(), password.size());
  }
};

constexpr std::string_view kUsage =
    "Usage: chronicles-account create|verify ACCOUNT\n"
    "       chronicles-account role ACCOUNT player|gm\n"
    "create and verify read one password line from standard input.\n"
    "role changes an existing account's privilege; it applies at the next login.\n";

int change_role(chronicles::AuthService &service, const std::string &account,
                std::string_view requested_role) {
  const auto role = chronicles::parse_account_role(requested_role);
  if (!role) {
    std::cerr << "Role must be player or gm.\n";
    return 2;
  }

  if (!service.set_account_role(account, *role)) {
    std::cerr << "Account does not exist. No role was changed.\n";
    return 1;
  }

  std::cout << "Account role set to " << chronicles::account_role_name(*role) << ".\n";
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  const std::string_view operation_name = argc >= 2 ? argv[1] : "";
  const bool password_operation =
      argc == 3 && (operation_name == "create" || operation_name == "verify");
  const bool role_operation = argc == 4 && operation_name == "role";
  if (!password_operation && !role_operation) {
    std::cerr << kUsage;
    return 2;
  }

  try {
    auto repositories =
        chronicles::make_postgres_character_repositories(database_connection());
    chronicles::AuthService service(std::move(repositories));
    if (role_operation) {
      return change_role(service, argv[2], argv[3]);
    }

    auto password = read_password();
    PasswordClearer clear_password{password};

    if (operation_name == "create") {
      const bool created = service.create_account(argv[2], password);
      if (!created) {
        std::cerr << "Account already exists. Its password was not changed.\n";
        return 1;
      }

      std::cout << "Account created.\n";
      return 0;
    }

    const auto result = service.login(argv[2], password);
    if (result.ec() != x5::ec::Success) {
      std::cerr << "Account or password did not match.\n";
      return 1;
    }

    service.logout(result.access_token());
    std::cout << "Account verified.\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Account operation failed: " << error.what() << '\n';
    return 1;
  }
}