// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_UPDATE_PIPE_H
#define B3COIN_UPDATE_PIPE_H

#include <update/downloader.h>

#include <cstdio>
#include <string>
#include <string_view>

/** Byte transport for b3-hive-update, the B3 Hive app's update helper
 *  (doc/design/b3-hive-update-system.md).
 *
 *  The helper runs the update manager; its parent process (the app) owns
 *  the HTTPS connection. Like the Qt transport, the parent performs no
 *  policy: it reports every redirect before following it and streams the
 *  body in frames the helper accepts one by one, so https-only, approved
 *  hosts, redirect limits, size bounds and digests stay enforced here.
 *
 *  Protocol on the helper's stdin/stdout, LF-terminated ASCII lines:
 *
 *    helper: FETCH <url>
 *    parent: REDIRECT <url>  ->  helper: ALLOW | DENY   (DENY ends the fetch)
 *    parent: DATA <n> + n raw bytes  ->  helper: MORE | STOP   (STOP ends it)
 *    parent: DONE                      (the whole body was delivered)
 *    parent: FAIL <token>              (network failure; ends the fetch)
 *
 *  A frame that breaks the protocol ends the fetch with
 *  "update-pipe-protocol" and breaks the transport for good: the stream
 *  position is unknown, so no later fetch may trust it.
 */
namespace update {

//! Largest body frame the parent may send.
inline constexpr size_t MAX_PIPE_CHUNK{64 * 1024};
//! Longest protocol line, URL included.
inline constexpr size_t MAX_PIPE_LINE{4096};

//! The helper's end of the channel to its parent. Calls block.
class PipeChannel
{
public:
    virtual ~PipeChannel() = default;
    //! One line without its LF; false at end of input or past MAX_PIPE_LINE.
    virtual bool ReadLine(std::string& line) = 0;
    //! Exactly `n` bytes; false at end of input.
    virtual bool ReadExact(size_t n, std::string& out) = 0;
    //! Writes and flushes; false on failure.
    virtual bool Write(std::string_view bytes) = 0;
};

//! A channel over C streams, e.g. stdin and stdout in binary mode.
class FilePipeChannel : public PipeChannel
{
public:
    FilePipeChannel(FILE* in, FILE* out) : m_in{in}, m_out{out} {}
    bool ReadLine(std::string& line) override;
    bool ReadExact(size_t n, std::string& out) override;
    bool Write(std::string_view bytes) override;

private:
    FILE* m_in;
    FILE* m_out;
};

class PipeTransport : public UpdateTransport
{
public:
    explicit PipeTransport(PipeChannel& channel) : m_channel{channel} {}

    bool Fetch(const std::string& url,
               const std::function<bool(std::string_view chunk)>& sink,
               const std::function<bool(const std::string& redirect_url)>& on_redirect,
               std::string& error) override;

    bool broken() const { return m_broken; }

private:
    PipeChannel& m_channel;
    bool m_broken{false};
};

//! True for text that may travel in one protocol line: printable ASCII
//! without spaces, at most MAX_PIPE_LINE - 16 bytes.
bool PipeSafe(std::string_view text);

} // namespace update

#endif // B3COIN_UPDATE_PIPE_H
