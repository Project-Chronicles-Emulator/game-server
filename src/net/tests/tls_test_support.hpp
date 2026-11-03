#pragma once

#include "net/world_tls_listener.hpp"

#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <array>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

namespace chronicles::net_test {

inline std::string fixture_suffix() {
  std::array<unsigned char, 16> random_bytes{};
  if (RAND_bytes(random_bytes.data(), static_cast<int>(random_bytes.size())) != 1) {
    throw std::runtime_error("Cannot generate the test fixture directory suffix");
  }

  constexpr char kHexDigits[] = "0123456789abcdef";
  std::string suffix;
  suffix.reserve(random_bytes.size() * 2);
  for (const auto byte : random_bytes) {
    suffix.push_back(kHexDigits[byte >> 4]);
    suffix.push_back(kHexDigits[byte & 0x0f]);
  }
  return suffix;
}

class TestDirectory {
public:
  TestDirectory() {
    const auto root = std::filesystem::absolute(LISTENER_TEST_FIXTURE_ROOT).lexically_normal();
    std::filesystem::create_directories(root);
    path_ = (root / fixture_suffix()).lexically_normal();
    if (path_.parent_path() != root || !std::filesystem::create_directory(path_)) {
      throw std::runtime_error("Cannot create a private listener test directory");
    }
  }

  ~TestDirectory() {
    // The checked path is a newly created direct child of the build fixture directory.
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path &path() const {
    return path_;
  }

private:
  std::filesystem::path path_;
};

class TemporaryTlsFiles {
public:
  TemporaryTlsFiles()
      : certificate_(directory_.path() / "certificate.pem"),
        private_key_(directory_.path() / "private-key.pem") {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> key_context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    if (!key_context || EVP_PKEY_keygen_init(key_context.get()) != 1 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(key_context.get(), 2048) != 1) {
      throw std::runtime_error("Cannot prepare a temporary listener test key");
    }

    EVP_PKEY *generated_key = nullptr;
    if (EVP_PKEY_keygen(key_context.get(), &generated_key) != 1) {
      throw std::runtime_error("Cannot generate a temporary listener test key");
    }
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(generated_key, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
    if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0) ||
        !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) ||
        X509_set_pubkey(certificate.get(), key.get()) != 1) {
      throw std::runtime_error("Cannot prepare a temporary listener test certificate");
    }

    auto *subject = X509_get_subject_name(certificate.get());
    if (!subject ||
        X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char *>("localhost"), -1, -1,
                                   0) != 1 ||
        X509_set_issuer_name(certificate.get(), subject) != 1 ||
        X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) {
      throw std::runtime_error("Cannot sign a temporary listener test certificate");
    }

    std::unique_ptr<BIO, decltype(&BIO_free)> certificate_output(
        BIO_new_file(certificate_.string().c_str(), "w"), BIO_free);
    std::unique_ptr<BIO, decltype(&BIO_free)> key_output(
        BIO_new_file(private_key_.string().c_str(), "w"), BIO_free);
    if (!certificate_output || !key_output ||
        PEM_write_bio_X509(certificate_output.get(), certificate.get()) != 1 ||
        PEM_write_bio_PrivateKey(key_output.get(), key.get(), nullptr, nullptr, 0, nullptr,
                                 nullptr) != 1) {
      throw std::runtime_error("Cannot write the temporary listener TLS files");
    }
  }

  WorldTlsOptions options() const {
    return {certificate_, private_key_, false};
  }

  const std::filesystem::path &directory() const {
    return directory_.path();
  }

private:
  TestDirectory directory_;
  std::filesystem::path certificate_;
  std::filesystem::path private_key_;
};

} // namespace chronicles::net_test
