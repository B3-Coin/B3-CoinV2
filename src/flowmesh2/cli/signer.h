// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CLI_SIGNER_H
#define B3_FLOWMESH2_CLI_SIGNER_H

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/result.h>
#include <key.h>
#include <pubkey.h>
#include <util/fs.h>

#include <string_view>
#include <vector>

/** The client's BIP340 key (M5-CLI-01, ARCHITECTURE.md O-106). A 32-byte
 * secp256k1 secret, hex in a 0600 file the client wrote (`fm2-cli keygen`;
 * Load refuses a file readable by group or others, KEY_MODE); the account is
 * AccountId::FromKey of its x-only key. SignAction sets the action's signer
 * to this key and signs the action's Id with fresh auxiliary randomness; the
 * caller stores the exact bytes in the outbox before they leave and never
 * signs the same action twice (DUR-4). Signing needs an ECC_Context. Real
 * key custody (hardware, a wallet) is not part of this tool; the secret's
 * copies outside CKey are cleansed. Guard rule 7 does not name `SignSchnorr`,
 * which the stage 2 report flags.
 */
namespace flowmesh2::cli {

inline constexpr std::string_view KEY_FILE{"the key file is not 32 bytes of hex"};           //!< (DECODE)
inline constexpr std::string_view KEY_INVALID{"the key file holds no valid secp256k1 secret"}; //!< (INVALID_INPUT)
inline constexpr std::string_view KEY_MODE{"the key file is readable by others; chmod 600 it"};  //!< (INVALID_INPUT)
inline constexpr std::string_view KEY_EXISTS{"the key file exists; not overwriting a key"};    //!< (INVALID_INPUT)
inline constexpr std::string_view KEY_WRITE{"the key file could not be written"};             //!< (STORAGE_FATAL)
inline constexpr std::string_view KEY_SIGN{"BIP340 signing failed"};                          //!< (INVARIANT)

class ClientKey
{
public:
    //! A fresh key written to `file` (0600; refuses an existing file).
    static Result<ClientKey> Generate(const fs::path& file);
    static Result<ClientKey> Load(const fs::path& file);

    const AccountId& Account() const { return m_account; }
    const XOnlyPubKey& Key() const { return m_xonly; }
    //! The exact bytes of `action` signed by this key (signer set to it).
    Result<std::vector<unsigned char>> SignAction(ActionV2 action) const;

private:
    explicit ClientKey(CKey key);

    CKey m_key;
    XOnlyPubKey m_xonly;
    AccountId m_account;
};

} // namespace flowmesh2::cli

#endif // B3_FLOWMESH2_CLI_SIGNER_H
