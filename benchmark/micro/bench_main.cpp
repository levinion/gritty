#include "bench.hpp"
#include <string>

namespace gritty::bench {
void register_vt();
void register_renderer();
void register_latency();
} // namespace gritty::bench

int main(int argc, char* argv[]) {
    std::string filter;
    bool json = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--filter" && i + 1 < argc) filter = argv[++i];
        else if (arg == "--json") json = true;
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: gritty-bench [--filter <substr>] [--json]\n";
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return 2;
        }
    }

    gritty::bench::register_vt();
    gritty::bench::register_renderer();
    gritty::bench::register_latency();

    for (const auto& item : gritty::bench::Registry::instance().cases()) {
        if (!filter.empty() && item.name.find(filter) == std::string::npos) continue;
        item.fn();
    }
    const auto& results = gritty::bench::Registry::instance().results();
    if (json) gritty::bench::Runner::print_json(results);
    else gritty::bench::Runner::print_table(results);
    return 0;
}
