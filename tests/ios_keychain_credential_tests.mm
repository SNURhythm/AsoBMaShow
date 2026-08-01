#include "ir/IosKeychainCredentialBackend.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include <chrono>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

} // namespace

int main() {
  @autoreleasepool {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string profileId = "ios-keychain-test-" + std::to_string(nonce);
    auto backend = ir::CreateIosKeychainCredentialBackend();
    expect(backend != nullptr, "Keychain backend is created");
    if (!backend) {
      return 1;
    }
    (void)backend->removeProfile(profileId);

    expect(backend->load(profileId, "tachi").status ==
               ir::IrCredentialBackendReadStatus::Missing,
           "missing Keychain item is normal");
    expect(backend->replace(profileId, "tachi", "first-key").succeeded,
           "Keychain item is saved");
    expect(backend->load(profileId, "tachi").apiKey == "first-key",
           "Keychain item round trips");
    expect(backend->replace(profileId, "tachi", "replacement-key").succeeded,
           "Keychain item is replaceable");
    expect(backend->load(profileId, "tachi").apiKey == "replacement-key",
           "replacement becomes current");

    NSString *account = [NSString
        stringWithFormat:@"%s:tachi", profileId.c_str()];
    NSDictionary *query = @{
      (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
      (__bridge id)kSecAttrService :
          @"com.SNURhythm.AsoBMaShow.ir-credentials",
      (__bridge id)kSecAttrAccount : account,
      (__bridge id)kSecReturnAttributes : @YES,
      (__bridge id)kSecMatchLimit : (__bridge id)kSecMatchLimitOne,
    };
    CFTypeRef copied = nullptr;
    const OSStatus status = SecItemCopyMatching(
        (__bridge CFDictionaryRef)query, &copied);
    expect(status == errSecSuccess && copied != nullptr,
           "saved Keychain attributes are readable");
    if (status == errSecSuccess && copied != nullptr) {
      NSDictionary *attributes = (__bridge NSDictionary *)copied;
      expect([attributes[(__bridge id)kSecAttrAccessible]
                 isEqual:(__bridge id)
                             kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly],
             "Keychain item uses device-only after-first-unlock protection");
      CFRelease(copied);
    }

    expect(backend->remove(profileId, "tachi").succeeded,
           "Keychain item is removed");
    expect(backend->remove(profileId, "tachi").succeeded,
           "missing Keychain removal is idempotent");
    expect(backend->load(profileId, "tachi").status ==
               ir::IrCredentialBackendReadStatus::Missing,
           "removed Keychain item stays absent");
  }

  if (failures != 0) {
    std::cerr << failures << " iOS Keychain credential test(s) failed\n";
    return 1;
  }
  std::cout << "iOS Keychain credential tests passed\n";
  return 0;
}
