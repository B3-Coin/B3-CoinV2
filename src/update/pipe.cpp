// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <update/pipe.h>

#include <util/strencodings.h>

#include <algorithm>

namespace update {
namespace {

constexpr std::string_view FAIL_PREFIX{"update-fetch-"};

//! The parent names its own failures; only a well-formed fetch token is
//! passed on, so nothing it sends can masquerade as another error.
std::string FailToken(std::string_view token)
{
    const bool well_formed{token.size() > FAIL_PREFIX.size() && token.size() <= 64 &&
                           token.starts_with(FAIL_PREFIX) &&
                           std::all_of(token.begin(), token.end(), [](char c) {
                               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
                           })};
    return well_formed ? std::string{token} : "update-fetch-failed";
}

} // namespace

bool PipeSafe(std::string_view text)
{
    return !text.empty() && text.size() <= MAX_PIPE_LINE - 16 &&
           std::all_of(text.begin(), text.end(), [](char c) { return c > ' ' && c < 0x7f; });
}

bool FilePipeChannel::ReadLine(std::string& line)
{
    line.clear();
    while (true) {
        const int c{std::fgetc(m_in)};
        if (c == EOF) return false;
        if (c == '\n') return true;
        if (line.size() >= MAX_PIPE_LINE) return false;
        line.push_back(static_cast<char>(c));
    }
}

bool FilePipeChannel::ReadExact(size_t n, std::string& out)
{
    out.resize(n);
    size_t got{0};
    while (got < n) {
        const size_t r{std::fread(out.data() + got, 1, n - got, m_in)};
        if (r == 0) return false;
        got += r;
    }
    return true;
}

bool FilePipeChannel::Write(std::string_view bytes)
{
    return std::fwrite(bytes.data(), 1, bytes.size(), m_out) == bytes.size() &&
           std::fflush(m_out) == 0;
}

bool PipeTransport::Fetch(const std::string& url,
                          const std::function<bool(std::string_view chunk)>& sink,
                          const std::function<bool(const std::string& redirect_url)>& on_redirect,
                          std::string& error)
{
    error.clear();
    if (m_broken) {
        error = "update-pipe-protocol";
        return false;
    }
    if (!PipeSafe(url)) {
        error = "update-fetch-url";
        return false;
    }
    auto broken = [&] {
        m_broken = true;
        error = "update-pipe-protocol";
        return false;
    };
    if (!m_channel.Write("FETCH " + url + "\n")) return broken();

    std::string line, chunk;
    while (true) {
        if (!m_channel.ReadLine(line)) return broken();
        if (line == "DONE") return true;
        if (line.starts_with("FAIL ")) {
            error = FailToken(std::string_view{line}.substr(5));
            return false;
        }
        if (line.starts_with("REDIRECT ")) {
            const std::string target{line.substr(9)};
            const bool allow{PipeSafe(target) && on_redirect(target)};
            if (!m_channel.Write(allow ? "ALLOW\n" : "DENY\n")) return broken();
            if (!allow) {
                error = "update-fetch-redirect";
                return false;
            }
            continue;
        }
        if (line.starts_with("DATA ")) {
            const auto size{ToIntegral<size_t>(std::string_view{line}.substr(5))};
            if (!size || *size == 0 || *size > MAX_PIPE_CHUNK) return broken();
            if (!m_channel.ReadExact(*size, chunk)) return broken();
            const bool more{sink(chunk)};
            if (!m_channel.Write(more ? "MORE\n" : "STOP\n")) return broken();
            // A refused chunk ends the fetch; the sink's owner names why.
            if (!more) return false;
            continue;
        }
        return broken();
    }
}

} // namespace update
