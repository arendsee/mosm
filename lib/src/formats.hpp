#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// streamSeq :: Str -> ([Read Str] -> <IO> ()) -> <IO> ()
//
// Streams a FASTA file to `write_out` in chunks of at least CHUNK_TARGET
// bytes of payload (header + sequence). A single record whose payload
// exceeds the target is emitted in its own chunk, which may be much larger
// than the target (e.g. a whole chromosome). Sequences may span many lines;
// CRLF endings and blank lines are handled.
//
// Sequence bytes are read directly from the file's streambuf into
// `std::vector<uint8_t>`, without a `std::string` intermediate. Headers
// (small, user-facing text) stay as `std::string`.
template <class Sink>
inline void streamSeq(const std::string& path, Sink write_out) {
    using Read   = std::tuple<std::string, std::vector<std::uint8_t>>;
    using traits = std::char_traits<char>;

    constexpr std::size_t STREAM_BUF   = 1u << 20;   // 1 MiB fstream buffer
    constexpr std::size_t CHUNK_TARGET = 1u << 20;   // flush at >=1 MiB payload

    std::vector<char> filebuf(STREAM_BUF);
    std::ifstream in;
    in.rdbuf()->pubsetbuf(filebuf.data(), filebuf.size());
    in.open(path, std::ios::binary);
    if (!in) throw std::runtime_error("streamSeq: cannot open " + path);

    auto* sb = in.rdbuf();

    std::vector<Read> chunk;
    std::string header;
    std::vector<std::uint8_t> seq;
    header.reserve(256);
    bool in_record = false;
    std::size_t chunk_bytes = 0;

    while (true) {
        int c = sb->sgetc();
        if (c == traits::eof()) break;

        // Skip blank lines and stray CRs between records.
        if (c == '\n' || c == '\r') {
            sb->sbumpc();
            continue;
        }

        if (c == '>') {
            if (in_record) {
                chunk_bytes += header.size() + seq.size();
                chunk.emplace_back(std::move(header), std::move(seq));
                if (chunk_bytes >= CHUNK_TARGET) {
                    write_out(std::move(chunk));
                    chunk.clear();
                    chunk_bytes = 0;
                }
            }

            sb->sbumpc();  // consume '>'
            int hc;
            while ((hc = sb->sbumpc()) != traits::eof() && hc != '\n') {
                if (hc != '\r') header.push_back(static_cast<char>(hc));
            }
            in_record = true;
        } else {
            if (!in_record) {
                throw std::runtime_error(
                    "streamSeq: sequence data before header in " + path);
            }
            int sc;
            while ((sc = sb->sbumpc()) != traits::eof() && sc != '\n') {
                if (sc != '\r') {
                    seq.push_back(static_cast<std::uint8_t>(sc));
                }
            }
        }
    }

    if (in_record) {
        chunk_bytes += header.size() + seq.size();
        chunk.emplace_back(std::move(header), std::move(seq));
    }
    if (!chunk.empty()) write_out(std::move(chunk));
}

inline std::string toFastaEntries(
    const std::vector<std::tuple<std::string, std::vector<std::uint8_t>>>& reads) {
    std::string out;

    std::size_t total = 0;
    for (const auto& r : reads) {
        total += std::get<0>(r).size() + std::get<1>(r).size() + 3;  // '>' + 2 '\n'
    }
    out.reserve(total);

    for (const auto& r : reads) {
        const std::string&              header = std::get<0>(r);
        const std::vector<std::uint8_t>& seq    = std::get<1>(r);

        out.push_back('>');
        out.append(header);
        out.push_back('\n');
        out.append(reinterpret_cast<const char*>(seq.data()), seq.size());
        out.push_back('\n');
    }

    return out;
}
