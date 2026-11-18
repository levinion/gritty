#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#ifndef GRITTY_BENCH_SOURCE_DIR
#define GRITTY_BENCH_SOURCE_DIR "."
#endif

namespace gritty::bench {

using CaseFn = void (*)();

struct Sample {
    double us{0};
    uint64_t bytes{0};
};

struct Result {
    std::string name;
    size_t iterations{0};
    uint64_t bytes_per_iteration{0};
    std::vector<double> us;
    double min_us{0}, p50_us{0}, p95_us{0}, p99_us{0}, max_us{0}, mean_us{0};
    double mb_s{0};
};

class Registry final {
public:
    static Registry& instance() {
        static Registry registry;
        return registry;
    }

    void add(std::string name, CaseFn fn) { cases_.push_back({std::move(name), fn}); }
    void report(Result result) { results_.push_back(std::move(result)); }
    [[nodiscard]] const auto& cases() const noexcept { return cases_; }
    [[nodiscard]] const auto& results() const noexcept { return results_; }

private:
    struct Case {
        std::string name;
        CaseFn fn;
    };
    std::vector<Case> cases_;
    std::vector<Result> results_;
};

class Registrar final {
public:
    Registrar(std::string_view name, CaseFn fn) {
        Registry::instance().add(std::string(name), fn);
    }
};

class Runner final {
public:
    explicit Runner(int warmup = 2) : warmup_(warmup) {}

    template<class Fn>
    void report(std::string name, uint64_t bytes_per_iteration, size_t iterations, Fn&& fn) {
        Result result;
        result.name = std::move(name);
        result.iterations = iterations;
        result.bytes_per_iteration = bytes_per_iteration;
        result.us.reserve(iterations);
        const int warmup = std::min(warmup_, static_cast<int>(iterations));
        for (int i = 0; i < warmup; ++i) fn(static_cast<size_t>(i));

        const auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < iterations; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            fn(i);
            const auto t1 = std::chrono::steady_clock::now();
            result.us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        const auto end = std::chrono::steady_clock::now();
        summarize(result);
        const double seconds = std::chrono::duration<double>(end - start).count();
        if (seconds > 0 && bytes_per_iteration > 0)
            result.mb_s = static_cast<double>(bytes_per_iteration) * iterations / (1024.0 * 1024.0) / seconds;
        Registry::instance().report(std::move(result));
    }

    static void print_table(const std::vector<Result>& results) {
        std::cout << std::left << std::setw(40) << "case" << std::right << std::setw(8) << "iters"
                  << std::setw(12) << "min(us)" << std::setw(12) << "p50(us)"
                  << std::setw(12) << "p95(us)" << std::setw(12) << "p99(us)"
                  << std::setw(12) << "max(us)" << std::setw(12) << "mean(us)"
                  << std::setw(12) << "MB/s" << '\n';
        std::cout << std::string(132, '-') << '\n';
        for (const auto& r : results) {
            std::cout << std::left << std::setw(40) << r.name << std::right << std::setw(8) << r.iterations
                      << std::setw(12) << fixed2(r.min_us) << std::setw(12) << fixed2(r.p50_us)
                      << std::setw(12) << fixed2(r.p95_us) << std::setw(12) << fixed2(r.p99_us)
                      << std::setw(12) << fixed2(r.max_us) << std::setw(12) << fixed2(r.mean_us)
                      << std::setw(12) << fixed2(r.mb_s) << '\n';
        }
    }

    static void print_json(const std::vector<Result>& results) {
        std::cout << "{\n  \"results\": [\n";
        for (size_t i = 0; i < results.size(); ++i) {
            const auto& r = results[i];
            std::cout << "    {\"name\":\"" << json_escape(r.name) << "\",\"iterations\":" << r.iterations
                      << ",\"bytes_per_iteration\":" << r.bytes_per_iteration
                      << ",\"min_us\":" << r.min_us << ",\"p50_us\":" << r.p50_us
                      << ",\"p95_us\":" << r.p95_us << ",\"p99_us\":" << r.p99_us
                      << ",\"max_us\":" << r.max_us << ",\"mean_us\":" << r.mean_us
                      << ",\"mb_s\":" << r.mb_s << "}";
            std::cout << (i + 1 < results.size() ? ",\n" : "\n");
        }
        std::cout << "  ]\n}\n";
    }

private:
    static std::string json_escape(std::string_view value) {
        std::string out;
        for (const char c : value) {
            if (c == '"' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        return out;
    }
    static std::string fixed2(double value) {
        if (!std::isfinite(value)) return "0";
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.2f", value);
        return buffer;
    }
    static void summarize(Result& result) {
        if (result.us.empty()) return;
        std::sort(result.us.begin(), result.us.end());
        result.min_us = result.us.front();
        result.max_us = result.us.back();
        auto at = [&](double fraction) {
            const double position = fraction * static_cast<double>(result.us.size() - 1);
            const size_t lo = static_cast<size_t>(std::floor(position));
            const size_t hi = static_cast<size_t>(std::ceil(position));
            const double weight = position - static_cast<double>(lo);
            return result.us[lo] * (1.0 - weight) + result.us[hi] * weight;
        };
        result.p50_us = at(0.50);
        result.p95_us = at(0.95);
        result.p99_us = at(0.99);
        const double sum = std::accumulate(result.us.begin(), result.us.end(), 0.0);
        result.mean_us = sum / static_cast<double>(result.us.size());
    }
    int warmup_;
};

inline std::string find_monospace_font() {
    if (const char* env = std::getenv("GRITTY_TEST_FONT")) {
        if (std::filesystem::exists(env)) return env;
    }
    const std::vector<std::string> candidates = {
        "/System/Library/Fonts/Monaco.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    };
    for (const auto& path : candidates)
        if (std::filesystem::exists(path)) return path;
    return {};
}

} // namespace gritty::bench

#define GRITTY_BENCH_CASE(name) \
    static void name(); \
    static gritty::bench::Registrar registrar_##name(#name, &name); \
    static void name()
