#include "HashUtils.h"
#include <Windows.h>
#include <algorithm>
#include <bcrypt.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

// Buffer size for file reading (64KB chunks)
constexpr size_t HASH_BUFFER_SIZE = 65536;

std::string HashUtils::CalculateMD5(const std::string &filePath) {
  return CalculateHash(filePath, HashType::MD5);
}

std::string HashUtils::CalculateSHA256(const std::string &filePath) {
  return CalculateHash(filePath, HashType::SHA256);
}

std::string HashUtils::CalculateHash(const std::string &filePath,
                                     HashType type) {
  // Select algorithm
  LPCWSTR algorithm =
      (type == HashType::MD5) ? BCRYPT_MD5_ALGORITHM : BCRYPT_SHA256_ALGORITHM;

  BCRYPT_ALG_HANDLE hAlg = NULL;
  BCRYPT_HASH_HANDLE hHash = NULL;
  NTSTATUS status = ((NTSTATUS)0xC0000001L); // STATUS_UNSUCCESSFUL
  DWORD hashLength = 0;
  DWORD resultLength = 0;
  std::string result;

  // Open algorithm provider
  status = BCryptOpenAlgorithmProvider(&hAlg, algorithm, NULL, 0);
  if (!BCRYPT_SUCCESS(status)) {
    std::cerr << "Failed to open algorithm provider" << std::endl;
    return "";
  }

  // Get hash length
  status = BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PBYTE)&hashLength,
                             sizeof(DWORD), &resultLength, 0);
  if (!BCRYPT_SUCCESS(status)) {
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return "";
  }

  // Allocate hash buffer
  std::vector<unsigned char> hashBuffer(hashLength);

  // Create hash object
  status = BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0);
  if (!BCRYPT_SUCCESS(status)) {
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return "";
  }

  // Open file and hash contents
  std::ifstream file(filePath, std::ios::binary);
  if (!file.is_open()) {
    std::cerr << "Failed to open file for hashing: " << filePath << std::endl;
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return "";
  }

  std::vector<char> buffer(HASH_BUFFER_SIZE);
  while (file.read(buffer.data(), HASH_BUFFER_SIZE) || file.gcount() > 0) {
    status = BCryptHashData(hHash, (PBYTE)buffer.data(),
                            static_cast<ULONG>(file.gcount()), 0);
    if (!BCRYPT_SUCCESS(status)) {
      file.close();
      BCryptDestroyHash(hHash);
      BCryptCloseAlgorithmProvider(hAlg, 0);
      return "";
    }
  }
  file.close();

  // Finish hash
  status = BCryptFinishHash(hHash, hashBuffer.data(), hashLength, 0);
  if (BCRYPT_SUCCESS(status)) {
    result = BytesToHex(hashBuffer.data(), hashLength);
  }

  // Cleanup
  BCryptDestroyHash(hHash);
  BCryptCloseAlgorithmProvider(hAlg, 0);

  return result;
}

bool HashUtils::VerifyHash(const std::string &filePath,
                           const std::string &expectedHash, HashType type) {
  if (expectedHash.empty()) {
    return true; // No expected hash, consider verified
  }

  std::string calculatedHash = CalculateHash(filePath, type);
  if (calculatedHash.empty()) {
    return false; // Failed to calculate hash
  }

  // Case-insensitive comparison
  std::string expectedLower = expectedHash;
  std::string calculatedLower = calculatedHash;
  std::transform(expectedLower.begin(), expectedLower.end(),
                 expectedLower.begin(), ::tolower);
  std::transform(calculatedLower.begin(), calculatedLower.end(),
                 calculatedLower.begin(), ::tolower);

  return expectedLower == calculatedLower;
}

std::string HashUtils::BytesToHex(const unsigned char *bytes, size_t length) {
  std::stringstream ss;
  ss << std::hex << std::setfill('0');
  for (size_t i = 0; i < length; ++i) {
    ss << std::setw(2) << static_cast<int>(bytes[i]);
  }
  return ss.str();
}

HashType HashUtils::ParseHashType(const std::string &typeStr) {
  std::string lower = typeStr;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

  if (lower == "md5") {
    return HashType::MD5;
  } else if (lower == "sha256" || lower == "sha-256") {
    return HashType::SHA256;
  }

  // Default to SHA256
  return HashType::SHA256;
}

std::string HashUtils::HashTypeToString(HashType type) {
  switch (type) {
  case HashType::MD5:
    return "MD5";
  case HashType::SHA256:
    return "SHA256";
  default:
    return "Unknown";
  }
}
