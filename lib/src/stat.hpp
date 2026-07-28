#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

// -----------------------------------------------------------------------------
// Statistics over sequence files (`mosm stat` / `mosm cstat`).
//
// Two families of input:
//   * length stats operate on [(Str, U64)] = (header, length) pairs.
//   * composition stats operate on [Vector 256 U64] = one per-sequence byte
//     histogram each, indexed by byte value.
//
// The display functions (statsProfile, statsSummary, ...) are `render`
// terminal actions: they consume the collected data and write a human-readable
// report to stdout, returning () (void). The nexus suppresses their () return.
//
// morloc signatures (see stat.loc):
//   charHist        :: Vector U8       -> Vector 256 U64
//   addHist         :: Vector n U64    -> Vector n U64 -> Vector n U64
//   caseInsensitive :: Vector 256 U64  -> Vector 256 U64
//   statsProfile    :: [Vector 256 U64] -> <IO,Err> ()
//   statsProportion :: [Vector 256 U64] -> <IO,Err> ()
//   statsCountTable :: [Vector 256 U64] -> <IO,Err> ()
//   cstatsSummary   :: [Vector 256 U64] -> <IO,Err> ()
//   statsSummary    :: [(Str, U64)]    -> <IO,Err> ()
//   statsHist       :: [(Str, U64)]    -> <IO,Err> ()
//   statsLogHist    :: [(Str, U64)]    -> <IO,Err> ()
// -----------------------------------------------------------------------------

namespace mosm_stat {

// Total number of characters a byte histogram accounts for (i.e. the length of
// the sequence it was built from).
inline std::uint64_t histTotal(const std::vector<std::uint64_t>& h) {
    std::uint64_t t = 0;
    for (std::uint64_t c : h) t += c;
    return t;
}

// Sum a list of per-sequence histograms into one 256-bucket total.
inline std::vector<std::uint64_t> aggregate(
    const std::vector<std::vector<std::uint64_t>>& hs) {
    std::vector<std::uint64_t> tot(256, 0);
    for (const auto& h : hs) {
        const std::size_t n = std::min<std::size_t>(h.size(), 256);
        for (std::size_t i = 0; i < n; ++i) tot[i] += h[i];
    }
    return tot;
}

// The per-sequence lengths implied by a list of byte histograms.
inline std::vector<std::uint64_t> histLengths(
    const std::vector<std::vector<std::uint64_t>>& hs) {
    std::vector<std::uint64_t> lengths;
    lengths.reserve(hs.size());
    for (const auto& h : hs) lengths.push_back(histTotal(h));
    return lengths;
}

// Linear-interpolated quantile of an already-sorted, non-empty vector.
inline double quantileSorted(const std::vector<std::uint64_t>& xs, double q) {
    if (xs.empty()) return 0.0;
    if (xs.size() == 1) return static_cast<double>(xs[0]);
    const double pos  = q * static_cast<double>(xs.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(std::floor(pos));
    const std::size_t hi = static_cast<std::size_t>(std::ceil(pos));
    const double frac = pos - static_cast<double>(lo);
    return static_cast<double>(xs[lo]) * (1.0 - frac) +
           static_cast<double>(xs[hi]) * frac;
}

// N50: the length such that half of all characters lie in sequences of at least
// that length (the standard assembly statistic).
inline std::uint64_t n50(std::vector<std::uint64_t> xs) {
    if (xs.empty()) return 0;
    std::sort(xs.begin(), xs.end(), std::greater<std::uint64_t>());
    std::uint64_t total = 0;
    for (std::uint64_t x : xs) total += x;
    std::uint64_t acc = 0;
    for (std::uint64_t x : xs) {
        acc += x;
        if (acc * 2 >= total) return x;
    }
    return xs.back();
}

inline double meanOf(const std::vector<std::uint64_t>& xs) {
    if (xs.empty()) return 0.0;
    double s = 0.0;
    for (std::uint64_t x : xs) s += static_cast<double>(x);
    return s / static_cast<double>(xs.size());
}

// Sample standard deviation (n-1). Zero for fewer than two observations.
inline double sdOf(const std::vector<std::uint64_t>& xs, double m) {
    if (xs.size() < 2) return 0.0;
    double ss = 0.0;
    for (std::uint64_t x : xs) {
        const double d = static_cast<double>(x) - m;
        ss += d * d;
    }
    return std::sqrt(ss / static_cast<double>(xs.size() - 1));
}

inline std::uint64_t roundU(double x) {
    if (x < 0.0) return 0;
    return static_cast<std::uint64_t>(std::llround(x));
}

// True when it is worth emitting ANSI color (stdout is an interactive tty).
inline bool useColor() {
#if defined(__unix__) || defined(__APPLE__)
    return isatty(STDOUT_FILENO) != 0;
#else
    return false;
#endif
}

// A short printable label for a byte value.
inline std::string charRepr(int b) {
    switch (b) {
        case '\n': return "\\n";
        case '\r': return "\\r";
        case '\t': return "\\t";
        case ' ':  return "' '";
    }
    if (b >= 33 && b <= 126) return std::string(1, static_cast<char>(b));
    char buf[8];
    std::snprintf(buf, sizeof(buf), "\\x%02X", b & 0xFF);
    return std::string(buf);
}

// Print the five header lines shared by the summary/profile reports.
inline void printLengthSummary(std::ostream& os, std::vector<std::uint64_t> lengths) {
    const std::uint64_t nseq = static_cast<std::uint64_t>(lengths.size());
    std::uint64_t nchars = 0;
    for (std::uint64_t l : lengths) nchars += l;

    os << "nseq:      " << nseq << "\n";
    os << "nchars:    " << nchars << "\n";

    if (lengths.empty()) {
        os << "5sum:      (no sequences)\n";
        return;
    }

    std::sort(lengths.begin(), lengths.end());
    const std::uint64_t mn  = lengths.front();
    const std::uint64_t mx  = lengths.back();
    const std::uint64_t q1  = roundU(quantileSorted(lengths, 0.25));
    const std::uint64_t med = roundU(quantileSorted(lengths, 0.50));
    const std::uint64_t q3  = roundU(quantileSorted(lengths, 0.75));
    const double m  = meanOf(lengths);
    const double sd = sdOf(lengths, m);

    os << "5sum:      " << mn << " " << q1 << " " << med << " " << q3 << " "
       << mx << "\n";
    os << "mean(sd):  " << roundU(m) << " (" << roundU(sd) << ")\n";
    os << "N50:       " << n50(lengths) << "\n";
}

// Render labelled counts as right-aligned labels + scaled horizontal bars.
inline void printBars(std::ostream& os,
                      const std::vector<std::string>& labels,
                      const std::vector<std::uint64_t>& counts,
                      std::size_t width = 50) {
    std::uint64_t maxc = 0;
    std::size_t   lw   = 0;
    for (std::uint64_t c : counts) maxc = std::max(maxc, c);
    for (const std::string& s : labels) lw = std::max(lw, s.size());

    for (std::size_t i = 0; i < counts.size(); ++i) {
        const std::size_t bar =
            (maxc == 0) ? 0
                        : static_cast<std::size_t>(
                              static_cast<double>(counts[i]) /
                                  static_cast<double>(maxc) *
                                  static_cast<double>(width) +
                              0.5);
        os << std::string(lw - labels[i].size(), ' ') << labels[i] << " | "
           << std::string(bar, '#') << ' ' << counts[i] << "\n";
    }
}

// -- protein composition profile ---------------------------------------------

// Column order and amino-acid grouping used by the profile chart.
inline const char* aaOrder() { return "LVGAIPFYWSEKDRTNQHMCU"; }

// ANSI color for an amino acid, grouped by chemistry (empty if ungrouped).
inline const char* aaColor(char aa) {
    if (std::strchr("LVGAIP", aa)) return "\033[31m";     // hydrophobic - red
    if (std::strchr("FYW", aa))    return "\033[35m";     // aromatic    - purple
    if (std::strchr("SEKDRTNQH", aa)) return "\033[34m";  // hydrophilic - blue
    if (std::strchr("MCU", aa))    return "\033[33m";     // sulfurous   - yellow
    return "";
}

// A vertical ASCII bar chart of amino-acid composition, tallest column filled.
inline void printProfileChart(std::ostream& os,
                              const std::vector<std::uint64_t>& tot) {
    const std::string order = aaOrder();
    const int         HEIGHT = 10;
    const char*       RAMP   = ".:'";  // partial top-cell glyphs, low -> high
    const bool        color  = useColor();
    const char*       reset  = color ? "\033[0m" : "";

    // Fold both cases so the profile is correct regardless of upstream folding.
    std::vector<std::uint64_t> counts;
    counts.reserve(order.size());
    for (char c : order) {
        const unsigned up = static_cast<unsigned char>(c);
        const unsigned lo = static_cast<unsigned char>(std::tolower(up));
        std::uint64_t n = tot[up];
        if (lo != up) n += tot[lo];
        counts.push_back(n);
    }

    std::uint64_t maxc = 0;
    for (std::uint64_t n : counts) maxc = std::max(maxc, n);

    for (int row = HEIGHT - 1; row >= 0; --row) {
        std::string line;
        for (std::size_t i = 0; i < order.size(); ++i) {
            const double h = (maxc == 0)
                                 ? 0.0
                                 : static_cast<double>(counts[i]) /
                                       static_cast<double>(maxc) *
                                       static_cast<double>(HEIGHT);
            char glyph;
            if (h >= row + 1) {
                glyph = '|';
            } else if (h > row) {
                int lvl = static_cast<int>((h - row) * 3.0);
                if (lvl > 2) lvl = 2;
                glyph = RAMP[lvl];
            } else {
                glyph = ' ';
            }

            if (glyph == ' ') {
                line.push_back(' ');
            } else if (color) {
                line += aaColor(order[i]);
                line.push_back(glyph);
                line += reset;
            } else {
                line.push_back(glyph);
            }
        }
        os << line << "\n";
    }

    // Label row: the amino-acid letters, colored by group.
    std::string labels;
    for (char c : order) {
        if (color) {
            labels += aaColor(c);
            labels.push_back(c);
            labels += reset;
        } else {
            labels.push_back(c);
        }
    }
    os << labels << "\n";
}

// -- character tables ---------------------------------------------------------

// (byte, count) pairs with a positive count, sorted by descending count.
inline std::vector<std::pair<int, std::uint64_t>> nonzeroCounts(
    const std::vector<std::uint64_t>& tot) {
    std::vector<std::pair<int, std::uint64_t>> rows;
    for (int b = 0; b < 256; ++b) {
        if (tot[b] > 0) rows.emplace_back(b, tot[b]);
    }
    std::sort(rows.begin(), rows.end(),
              [](const std::pair<int, std::uint64_t>& a,
                 const std::pair<int, std::uint64_t>& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });
    return rows;
}

}  // namespace mosm_stat

// =============================================================================
// Exported (sourced) functions.
// =============================================================================

// psmap: forward-only map over a stream of reads. Same callback shape as
// psfilter (mosm.hpp): `next` pulls the next batch (empty at EOF), `write_out`
// forwards a batch. Each read is transformed by `f`; input batching is
// preserved (each non-empty input batch yields one output batch).
//
// morloc signature (see stat.loc):
//   psmap :: (a -> b) -> (<IO,Err> [a]) -> ([b] -> <IO,Err> ()) -> <IO,Err> ()
template <class Fn, class Next, class Sink>
inline void psmap(Fn f, Next next, Sink write_out) {
    using NextResult = std::invoke_result_t<Next>;
    using In         = typename NextResult::value_type;
    using Out        = std::decay_t<std::invoke_result_t<Fn, In&>>;

    for (;;) {
        auto batch = next();
        if (batch.empty()) break;

        std::vector<Out> out;
        out.reserve(batch.size());
        for (auto& r : batch) out.emplace_back(f(r));

        if (!out.empty()) write_out(std::move(out));
    }
}

// charHist: histogram of byte values in one sequence. Always 256 buckets.
inline std::vector<std::uint64_t> charHist(const std::vector<std::uint8_t>& seq) {
    std::vector<std::uint64_t> counts(256, 0);
    for (std::uint8_t b : seq) counts[b]++;
    return counts;
}

// addHist: element-wise sum of two equal-length histograms. Tolerates unequal
// lengths by treating the shorter as zero-padded.
inline std::vector<std::uint64_t> addHist(const std::vector<std::uint64_t>& a,
                                          const std::vector<std::uint64_t>& b) {
    const std::size_t n = std::max(a.size(), b.size());
    std::vector<std::uint64_t> out(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = (i < a.size() ? a[i] : 0) + (i < b.size() ? b[i] : 0);
    }
    return out;
}

// caseInsensitive: fold each lowercase letter's count into its uppercase
// counterpart, zeroing the lowercase bucket, so case-variant letters merge.
inline std::vector<std::uint64_t> caseInsensitive(std::vector<std::uint64_t> h) {
    h.resize(256, 0);
    for (int c = 'a'; c <= 'z'; ++c) {
        h[c - 'a' + 'A'] += h[c];
        h[c] = 0;
    }
    return h;
}

// statsSummary: five-number summary of sequence lengths.
inline void statsSummary(
    const std::vector<std::tuple<std::string, std::uint64_t>>& xs) {
    std::vector<std::uint64_t> lengths;
    lengths.reserve(xs.size());
    for (const auto& t : xs) lengths.push_back(std::get<1>(t));
    mosm_stat::printLengthSummary(std::cout, std::move(lengths));
}

// cstatsSummary: same summary, with lengths derived from byte histograms.
inline void cstatsSummary(const std::vector<std::vector<std::uint64_t>>& hs) {
    mosm_stat::printLengthSummary(std::cout, mosm_stat::histLengths(hs));
}

// statsHist: horizontal ASCII histogram of sequence lengths.
inline void statsHist(
    const std::vector<std::tuple<std::string, std::uint64_t>>& xs) {
    std::vector<std::uint64_t> lengths;
    lengths.reserve(xs.size());
    for (const auto& t : xs) lengths.push_back(std::get<1>(t));

    if (lengths.empty()) {
        std::cout << "(no sequences)\n";
        return;
    }

    const std::uint64_t mn = *std::min_element(lengths.begin(), lengths.end());
    const std::uint64_t mx = *std::max_element(lengths.begin(), lengths.end());
    const std::uint64_t range = mx - mn + 1;

    const std::uint64_t TARGET_BINS = 20;
    const std::uint64_t binw =
        std::max<std::uint64_t>(1, (range + TARGET_BINS - 1) / TARGET_BINS);
    const std::size_t nbins =
        static_cast<std::size_t>((range + binw - 1) / binw);

    std::vector<std::uint64_t> counts(nbins, 0);
    for (std::uint64_t l : lengths) {
        std::size_t bin = static_cast<std::size_t>((l - mn) / binw);
        if (bin >= nbins) bin = nbins - 1;
        counts[bin]++;
    }

    std::vector<std::string> labels(nbins);
    for (std::size_t i = 0; i < nbins; ++i) {
        const std::uint64_t lo = mn + static_cast<std::uint64_t>(i) * binw;
        const std::uint64_t hi = std::min(mx, lo + binw - 1);
        labels[i] = std::to_string(lo) + "-" + std::to_string(hi);
    }

    mosm_stat::printBars(std::cout, labels, counts);
}

// statsLogHist: horizontal ASCII histogram of log2(length), one bucket per
// power-of-two length interval [2^k, 2^(k+1)).
inline void statsLogHist(
    const std::vector<std::tuple<std::string, std::uint64_t>>& xs) {
    std::vector<std::uint64_t> lengths;
    lengths.reserve(xs.size());
    for (const auto& t : xs) lengths.push_back(std::get<1>(t));

    if (lengths.empty()) {
        std::cout << "(no sequences)\n";
        return;
    }

    // Bucket each length by its power-of-two magnitude; zero-length is dropped.
    int kmin = -1, kmax = -1;
    std::vector<int> ks;
    ks.reserve(lengths.size());
    for (std::uint64_t l : lengths) {
        if (l == 0) continue;
        int k = 0;
        while ((static_cast<std::uint64_t>(1) << (k + 1)) <= l) ++k;
        ks.push_back(k);
        if (kmin < 0 || k < kmin) kmin = k;
        if (kmax < 0 || k > kmax) kmax = k;
    }

    if (kmin < 0) {
        std::cout << "(no sequences with positive length)\n";
        return;
    }

    const std::size_t nbins = static_cast<std::size_t>(kmax - kmin + 1);
    std::vector<std::uint64_t> counts(nbins, 0);
    for (int k : ks) counts[static_cast<std::size_t>(k - kmin)]++;

    std::vector<std::string> labels(nbins);
    for (std::size_t i = 0; i < nbins; ++i) {
        const int k = kmin + static_cast<int>(i);
        labels[i] = "2^" + std::to_string(k);
    }

    mosm_stat::printBars(std::cout, labels, counts);
}

// statsProportion: proportion each byte contributes to the total character
// count, most frequent first.
inline void statsProportion(const std::vector<std::vector<std::uint64_t>>& hs) {
    const std::vector<std::uint64_t> tot = mosm_stat::aggregate(hs);
    const std::uint64_t grand = mosm_stat::histTotal(tot);

    if (grand == 0) {
        std::cout << "(no characters)\n";
        return;
    }

    const auto rows = mosm_stat::nonzeroCounts(tot);
    for (const auto& r : rows) {
        std::ostringstream frac;
        frac << std::fixed << std::setprecision(4)
             << (static_cast<double>(r.second) / static_cast<double>(grand));
        std::cout << mosm_stat::charRepr(r.first) << "\t" << frac.str() << "\n";
    }
}

// statsCountTable: raw count of each byte, most frequent first.
inline void statsCountTable(const std::vector<std::vector<std::uint64_t>>& hs) {
    const std::vector<std::uint64_t> tot = mosm_stat::aggregate(hs);
    const std::uint64_t grand = mosm_stat::histTotal(tot);

    if (grand == 0) {
        std::cout << "(no characters)\n";
        return;
    }

    const auto rows = mosm_stat::nonzeroCounts(tot);
    for (const auto& r : rows) {
        std::cout << mosm_stat::charRepr(r.first) << "\t" << r.second << "\n";
    }
}

// statsProfile: length summary followed by a colored vertical bar chart of
// amino-acid composition.
inline void statsProfile(const std::vector<std::vector<std::uint64_t>>& hs) {
    mosm_stat::printLengthSummary(std::cout, mosm_stat::histLengths(hs));
    const std::vector<std::uint64_t> tot = mosm_stat::aggregate(hs);
    mosm_stat::printProfileChart(std::cout, tot);
}
