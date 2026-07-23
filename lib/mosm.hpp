#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// -----------------------------------------------------------------------------
// cutStream / accStream: forward-only slice and index operations over a stream
// of reads.
//
// Design shape (same as sfile.hpp's smapoWrap): the caller supplies two
// callbacks that hide the IStream/OStream handles:
//   * Next: pulls the next batch. Returns an empty container at EOF.
//   * Sink: consumes one batch, forwarding it to the OStream.
// Morloc wires these from `@next ifile` and `@write z ofile` respectively.
//
// morloc signatures (see main.loc):
//   cutStream :: ?I64 -> ?I64 -> ?I64
//             -> (<IO> [Read a])
//             -> ([Read a] -> <IO> ())
//             -> <IO> ()
//   accStream :: I64
//             -> (<IO> [Read a])
//             -> ([Read a] -> <IO> ())
//             -> <IO> ()
//
// Streaming constraints:
//   * Negative bounds are not supported (would require materialising the
//     whole stream to compute the length).
//   * `step` must be positive. Zero and negative steps require random access.
// -----------------------------------------------------------------------------

// cutStream: [start:stop:step]. Missing start defaults to 0; missing stop
// defaults to end-of-stream; missing step defaults to 1. Preserves the
// source's batching: each input batch produces at most one output batch,
// containing the selected reads from that batch.
template <class Next, class Sink>
inline void cutStream(std::optional<std::int64_t> start_opt,
                      std::optional<std::int64_t> stop_opt,
                      std::optional<std::int64_t> step_opt,
                      Next next,
                      Sink write_out) {
    using NextResult = std::invoke_result_t<Next>;
    using Read       = typename NextResult::value_type;

    const std::int64_t start = start_opt.value_or(0);
    const std::int64_t step  = step_opt.value_or(1);
    const bool         bounded = stop_opt.has_value();
    const std::int64_t stop  = stop_opt.value_or(0);

    if (step <= 0) {
        throw std::runtime_error(
            "cutStream: step must be positive when streaming "
            "(zero and negative steps require random access)");
    }
    if (start < 0 || (bounded && stop < 0)) {
        throw std::runtime_error(
            "cutStream: negative bounds not supported when streaming");
    }
    if (bounded && stop <= start) {
        return;  // empty selection
    }

    std::int64_t idx       = 0;      // absolute record index in the stream
    std::int64_t next_pick = start;  // absolute index of the next record to keep
    std::vector<Read> out;

    for (;;) {
        auto batch = next();
        if (batch.empty()) break;

        for (auto& r : batch) {
            const std::int64_t here = idx++;
            if (bounded && here >= stop) {
                if (!out.empty()) write_out(std::move(out));
                return;
            }
            if (here == next_pick) {
                out.emplace_back(std::move(r));
                next_pick += step;
            }
        }

        if (!out.empty()) {
            write_out(std::move(out));
            out.clear();
        }
    }

    if (!out.empty()) write_out(std::move(out));
}

// accStream: pluck one read by absolute index, forward it as a one-element
// batch. Negative indices are rejected (streams are forward-only). An index
// beyond the end of the stream raises.
template <class Next, class Sink>
inline void accStream(std::int64_t idx, Next next, Sink write_out) {
    using NextResult = std::invoke_result_t<Next>;
    using Read       = typename NextResult::value_type;

    if (idx < 0) {
        throw std::runtime_error(
            "accStream: negative index not supported when streaming");
    }

    std::int64_t here = 0;
    for (;;) {
        auto batch = next();
        if (batch.empty()) break;

        const std::int64_t batch_start = here;
        const std::int64_t batch_end   =
            here + static_cast<std::int64_t>(batch.size());

        if (idx >= batch_start && idx < batch_end) {
            std::vector<Read> out;
            out.emplace_back(std::move(batch[idx - batch_start]));
            write_out(std::move(out));
            // Drain remaining batches so the source can close cleanly.
            // The caller (nexus/pool) owns the handle lifecycle; we just
            // stop pulling.
            return;
        }
        here = batch_end;
    }

    throw std::runtime_error(
        "accStream: index out of range (stream is shorter than requested)");
}
