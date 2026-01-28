#pragma once

#include <cstdint>
#include <string>


enum class HashType { MD5, SHA256 };

class HashUtils {
public:
  // Calculate MD5 hash of a file
  static std::string CalculateMD5(const std::string &filePath);

  // Calculate SHA256 hash of a file
  static std::string CalculateSHA256(const std::string &filePath);

  // Calculate hash of file with specified type
  static std::string CalculateHash(const std::string &filePath, HashType type);

  // Verify file against expected hash
  static bool VerifyHash(const std::string &filePath,
                         const std::string &expectedHash, HashType type);

  // Convert hash bytes to hex string
  static std::string BytesToHex(const unsigned char *bytes, size_t length);

  // Parse hash type from string (e.g., "MD5", "SHA256")
  static HashType ParseHashType(const std::string &typeStr);

  // Get string representation of hash type
  static std::string HashTypeToString(HashType type);
};
