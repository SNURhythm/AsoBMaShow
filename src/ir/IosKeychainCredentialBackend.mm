#include "IosKeychainCredentialBackend.h"

#include "IrCredentialStore.h"
#include "IrProfileSettings.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include <string>
#include <utility>

namespace ir {
namespace {

NSString *credentialService() {
  return @"com.SNURhythm.AsoBMaShow.ir-credentials";
}

NSString *accountName(std::string_view profileId,
                      std::string_view providerId) {
  const std::string account =
      std::string(profileId) + ":" + std::string(providerId);
  return [[NSString alloc] initWithBytes:account.data()
                                 length:account.size()
                               encoding:NSUTF8StringEncoding];
}

NSDictionary *itemQuery(std::string_view profileId,
                        std::string_view providerId) {
  return @{
    (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
    (__bridge id)kSecAttrService : credentialService(),
    (__bridge id)kSecAttrAccount : accountName(profileId, providerId),
  };
}

IrCredentialBackendReadResult failedRead() {
  return {.status = IrCredentialBackendReadStatus::Failed,
          .diagnostic = "iOS Keychain credential could not be read"};
}

IrCredentialBackendWriteResult failedWrite(std::string diagnostic) {
  return {.succeeded = false, .diagnostic = std::move(diagnostic)};
}

bool validIdentity(std::string_view profileId,
                   std::string_view providerId) noexcept {
  return isValidCredentialProfileId(profileId) &&
         isValidProviderId(providerId);
}

class IosKeychainCredentialBackend final : public IrCredentialBackend {
public:
  bool requiresLegacyFileMigration() const noexcept override { return true; }

  IrCredentialBackendReadResult
  load(std::string_view profileId,
       std::string_view providerId) noexcept override {
    @autoreleasepool {
      if (!validIdentity(profileId, providerId)) {
        return failedRead();
      }
      NSMutableDictionary *query =
          [itemQuery(profileId, providerId) mutableCopy];
      query[(__bridge id)kSecReturnData] = @YES;
      query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
      CFTypeRef copied = nullptr;
      const OSStatus status = SecItemCopyMatching(
          (__bridge CFDictionaryRef)query, &copied);
      if (status == errSecItemNotFound) {
        return {.status = IrCredentialBackendReadStatus::Missing};
      }
      if (status != errSecSuccess || copied == nullptr ||
          CFGetTypeID(copied) != CFDataGetTypeID()) {
        if (copied != nullptr) {
          CFRelease(copied);
        }
        return failedRead();
      }
      const auto data = static_cast<CFDataRef>(copied);
      const CFIndex length = CFDataGetLength(data);
      std::string apiKey;
      if (length > 0) {
        const auto *bytes = CFDataGetBytePtr(data);
        apiKey.assign(reinterpret_cast<const char *>(bytes),
                      static_cast<std::size_t>(length));
      }
      CFRelease(copied);
      if (!IrCredentialStore::isApiKeyFormatValid(apiKey)) {
        return failedRead();
      }
      return {.status = IrCredentialBackendReadStatus::Loaded,
              .apiKey = std::move(apiKey)};
    }
  }

  IrCredentialBackendWriteResult
  replace(std::string_view profileId, std::string_view providerId,
          std::string_view apiKey) noexcept override {
    @autoreleasepool {
      if (!validIdentity(profileId, providerId) ||
          !IrCredentialStore::isApiKeyFormatValid(apiKey)) {
        return failedWrite("iOS Keychain credential is invalid");
      }
      NSData *data = [NSData dataWithBytes:apiKey.data()
                                     length:apiKey.size()];
      NSDictionary *attributes = @{
        (__bridge id)kSecValueData : data,
        (__bridge id)kSecAttrAccessible :
            (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
      };
      NSDictionary *query = itemQuery(profileId, providerId);
      OSStatus status = SecItemUpdate((__bridge CFDictionaryRef)query,
                                      (__bridge CFDictionaryRef)attributes);
      if (status == errSecItemNotFound) {
        NSMutableDictionary *item = [query mutableCopy];
        [item addEntriesFromDictionary:attributes];
        status = SecItemAdd((__bridge CFDictionaryRef)item, nullptr);
        if (status == errSecDuplicateItem) {
          status = SecItemUpdate((__bridge CFDictionaryRef)query,
                                 (__bridge CFDictionaryRef)attributes);
        }
      }
      return status == errSecSuccess
                 ? IrCredentialBackendWriteResult{.succeeded = true}
                 : failedWrite("iOS Keychain credential could not be saved");
    }
  }

  IrCredentialBackendWriteResult
  remove(std::string_view profileId,
         std::string_view providerId) noexcept override {
    @autoreleasepool {
      if (!validIdentity(profileId, providerId)) {
        return failedWrite("iOS Keychain credential identity is invalid");
      }
      const OSStatus status = SecItemDelete(
          (__bridge CFDictionaryRef)itemQuery(profileId, providerId));
      return status == errSecSuccess || status == errSecItemNotFound
                 ? IrCredentialBackendWriteResult{.succeeded = true}
                 : failedWrite(
                       "iOS Keychain credential could not be removed");
    }
  }

  IrCredentialBackendWriteResult
  removeProfile(std::string_view profileId) noexcept override {
    @autoreleasepool {
      if (!isValidCredentialProfileId(profileId)) {
        return failedWrite("iOS Keychain profile identity is invalid");
      }
      NSDictionary *query = @{
        (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService : credentialService(),
        (__bridge id)kSecReturnAttributes : @YES,
        (__bridge id)kSecMatchLimit : (__bridge id)kSecMatchLimitAll,
      };
      CFTypeRef copied = nullptr;
      const OSStatus status = SecItemCopyMatching(
          (__bridge CFDictionaryRef)query, &copied);
      if (status == errSecItemNotFound) {
        return {.succeeded = true};
      }
      if (status != errSecSuccess || copied == nullptr) {
        if (copied != nullptr) {
          CFRelease(copied);
        }
        return failedWrite(
            "iOS Keychain profile credentials could not be read");
      }

      NSArray<NSDictionary *> *items = nil;
      if (CFGetTypeID(copied) == CFArrayGetTypeID()) {
        items = (__bridge NSArray<NSDictionary *> *)copied;
      } else if (CFGetTypeID(copied) == CFDictionaryGetTypeID()) {
        items = @[ (__bridge NSDictionary *)copied ];
      }
      if (items == nil) {
        CFRelease(copied);
        return failedWrite("iOS Keychain profile credentials are malformed");
      }

      const std::string prefix = std::string(profileId) + ":";
      bool succeeded = true;
      for (NSDictionary *item in items) {
        NSString *account = item[(__bridge id)kSecAttrAccount];
        if (![account isKindOfClass:[NSString class]]) {
          continue;
        }
        const char *utf8 = account.UTF8String;
        if (utf8 == nullptr || !std::string_view(utf8).starts_with(prefix)) {
          continue;
        }
        NSDictionary *deleteQuery = @{
          (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
          (__bridge id)kSecAttrService : credentialService(),
          (__bridge id)kSecAttrAccount : account,
        };
        const OSStatus deleteStatus = SecItemDelete(
            (__bridge CFDictionaryRef)deleteQuery);
        succeeded = succeeded &&
                    (deleteStatus == errSecSuccess ||
                     deleteStatus == errSecItemNotFound);
      }
      CFRelease(copied);
      return succeeded
                 ? IrCredentialBackendWriteResult{.succeeded = true}
                 : failedWrite(
                       "iOS Keychain profile credentials could not be removed");
    }
  }
};

} // namespace

std::unique_ptr<IrCredentialBackend> CreateIosKeychainCredentialBackend() {
  return std::make_unique<IosKeychainCredentialBackend>();
}

} // namespace ir
