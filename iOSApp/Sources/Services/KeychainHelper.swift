import Foundation
import Security

/// Keychain access helper for securely storing admin tokens.
/// Replaces UserDefaults storage for sensitive credentials.
enum KeychainHelper {
    private static let service = "com.lorenzo.bugbuster.tokens"
    
    /// Store a token for a device MAC address.
    static func saveToken(_ token: String, forMAC mac: String) -> Bool {
        guard let tokenData = token.data(using: .utf8) else { return false }
        
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: mac,
            kSecAttrAccessible as String: kSecAttrAccessibleWhenUnlockedThisDeviceOnly,
            kSecValueData as String: tokenData
        ]
        
        // Delete any existing item first
        SecItemDelete(query as CFDictionary)
        
        let status = SecItemAdd(query as CFDictionary, nil)
        return status == errSecSuccess
    }
    
    /// Retrieve a token for a device MAC address.
    static func loadToken(forMAC mac: String) -> String? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: mac,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne
        ]
        
        var result: AnyObject?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        
        guard status == errSecSuccess,
              let data = result as? Data,
              let token = String(data: data, encoding: .utf8) else {
            return nil
        }
        
        return token
    }
    
    /// Delete a token for a device MAC address.
    static func deleteToken(forMAC mac: String) -> Bool {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: mac
        ]
        
        let status = SecItemDelete(query as CFDictionary)
        return status == errSecSuccess || status == errSecItemNotFound
    }
    
    /// Migrate tokens from UserDefaults to Keychain.
    /// Called once on app launch to migrate existing installations.
    static func migrateFromUserDefaults() {
        let key = "bugbuster_tokens"
        guard let oldTokens = UserDefaults.standard.dictionary(forKey: key) as? [String: String] else {
            return
        }
        
        var migrated = 0
        for (mac, token) in oldTokens {
            if saveToken(token, forMAC: mac) {
                migrated += 1
            }
        }
        
        if migrated > 0 {
            // Clear from UserDefaults after successful migration
            UserDefaults.standard.removeObject(forKey: key)
            print("[Keychain] Migrated \(migrated) token(s) from UserDefaults to Keychain")
        }
    }
    
    /// Get all stored MAC addresses (for listing paired devices).
    static func allStoredMACs() -> [String] {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecReturnAttributes as String: true,
            kSecMatchLimit as String: kSecMatchLimitAll
        ]
        
        var result: AnyObject?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        
        guard status == errSecSuccess,
              let items = result as? [[String: Any]] else {
            return []
        }
        
        return items.compactMap { $0[kSecAttrAccount as String] as? String }
    }
}
