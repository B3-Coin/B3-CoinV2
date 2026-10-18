// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_UPDATE_FILESTORE_H
#define B3COIN_UPDATE_FILESTORE_H

#include <update/manager.h>
#include <util/fs.h>

#include <optional>
#include <string>
#include <string_view>

namespace update {

/** Rollback state in one small file, for hosts without Qt settings. The
 *  record is the Qt client's: "v1:<floor>:none" or
 *  "v1:<floor>:<sequence>:<payload hash>", plus a final LF. A missing file
 *  is a fresh install (floor 0); anything else unreadable is an error, so a
 *  damaged file can never quietly lower the floor. Writes go to a temporary
 *  file that is flushed and then renamed over the record. */
class FileSequenceStore : public SequenceStore
{
public:
    explicit FileSequenceStore(fs::path path) : m_path{std::move(path)} {}
    bool Load(SequenceState& state, std::string& error) override;
    bool Store(const SequenceState& state, std::string& error) override;

private:
    fs::path m_path;
};

std::string EncodeSequenceRecord(const SequenceState& state);
std::optional<SequenceState> DecodeSequenceRecord(std::string_view record);

} // namespace update

#endif // B3COIN_UPDATE_FILESTORE_H
