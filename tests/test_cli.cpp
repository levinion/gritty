#include <doctest/doctest.h>

#include "config/app_config.hpp"
#include "config/cli.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace cfg = gritty::config;

namespace {

std::vector<std::string> argv_of(std::vector<std::string> args) {
    args.insert(args.begin(), "gritty");
    return args;
}

std::vector<const char*> raw_argv(const std::vector<std::string>& args) {
    std::vector<const char*> result;
    result.reserve(args.size());
    for (const auto& arg : args) result.push_back(arg.c_str());
    return result;
}

} // namespace

TEST_CASE("CLI parses Alacritty-compatible options in both value forms") {
    const auto args = argv_of({
        "--config-file=theme.toml", "--working-directory=/tmp", "--cols=160",
        "--rows=48", "-T", "Title", "--class=general,instance", "--hold",
        "-f", "Monaco", "-s", "13.5",
    });
    const auto raw = raw_argv(args);
    const auto parsed = cfg::parse_cli(static_cast<int>(raw.size()), raw.data());
    REQUIRE(parsed.has_value());

    const auto& cli = parsed.value();
    CHECK(cli.action == cfg::CliAction::Run);
    CHECK(cli.config_path == "theme.toml");
    CHECK(cli.working_directory == "/tmp");
    CHECK(cli.cols == 160);
    CHECK(cli.rows == 48);
    CHECK(cli.title == "Title");
    CHECK(cli.app_id == "general");
    CHECK(cli.hold);
    CHECK(cli.font_family == "Monaco");
    CHECK(cli.font_size == 13.5);
}

TEST_CASE("CLI keeps legacy short options and help/version actions") {
    const auto legacy_args = argv_of({"-c", "legacy.toml", "--config", "second.toml",
                                      "--cwd", "/work", "-t", "Legacy", "-v"});
    const auto legacy_raw = raw_argv(legacy_args);
    const auto parsed = cfg::parse_cli(static_cast<int>(legacy_raw.size()), legacy_raw.data());
    REQUIRE(parsed.has_value());
    CHECK(parsed->config_path == "second.toml");
    CHECK(parsed->working_directory == "/work");
    CHECK(parsed->title == "Legacy");
    CHECK(parsed->action == cfg::CliAction::Version);

    for (const auto* option : {"-h", "--help", "-V", "--version"}) {
        const auto args = argv_of({option});
        const auto raw = raw_argv(args);
        const auto result = cfg::parse_cli(static_cast<int>(raw.size()), raw.data());
        REQUIRE(result.has_value());
        const auto expected = std::string_view(option) == "-V" || std::string_view(option) == "--version"
            ? cfg::CliAction::Version : cfg::CliAction::Help;
        CHECK(result->action == expected);
    }
}

TEST_CASE("CLI command passthrough stops option parsing immediately") {
    const std::vector<std::vector<std::string>> forms = {
        {"-e", "sh", "-c", "echo hi", "-V", "--help"},
        {"--exec", "sh", "-c", "echo hi", "--hold"},
        {"--command=sh", "-c", "echo hi", "--cols=160"},
        {"--", "sh", "-c", "echo hi", "-V"},
        {"sh", "-c", "echo hi", "--help"},
    };
    for (const auto& form : forms) {
        const auto args = argv_of(form);
        const auto raw = raw_argv(args);
        const auto parsed = cfg::parse_cli(static_cast<int>(raw.size()), raw.data());
        REQUIRE(parsed.has_value());
        CHECK(parsed->command == "sh");
        REQUIRE(parsed->command_args.size() >= 3);
        CHECK(parsed->command_args[0] == "-c");
        CHECK(parsed->command_args[1] == "echo hi");
        CHECK_FALSE(parsed->hold);
    }
}

TEST_CASE("CLI collects and applies option overrides") {
    const auto args = argv_of({
        "-o", "main.title=Override", "-omain.app_id=com.gritty", "-ofont.size=11.5",
        "-omain.osc52=PASTE-ONLY",
        "--option=colors.background=\"#112233\"",
        "--option=cursor.blink=true",
        "--option=window.dimensions.columns=120",
        "--option=window.dimensions.lines=40",
    });
    const auto raw = raw_argv(args);
    const auto parsed = cfg::parse_cli(static_cast<int>(raw.size()), raw.data());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->config_overrides.size() == 8);

    cfg::AppConfig config;
    size_t cols = 0;
    size_t rows = 0;
    for (const auto& override : parsed->config_overrides) {
        const auto result = config.apply_override(override, &cols, &rows);
        REQUIRE(result.has_value());
    }
    CHECK(config.main.title == "Override");
    CHECK(config.main.app_id == "com.gritty");
    CHECK(config.main.osc52 == cfg::Osc52Mode::PasteOnly);
    CHECK(config.font.size == 11.5);
    CHECK(config.colors.background == 0xff112233);
    CHECK(config.cursor.blink);
    CHECK(cols == 120);
    CHECK(rows == 40);
}

TEST_CASE("CLI and AppConfig reject malformed value options") {
    const std::vector<std::vector<std::string>> malformed = {
        {"-o"},
        {"--option", "font.size"},
        {"--option", "=12"},
        {"--cols"},
        {"--rows=0"},
        {"--size=0"},
        {"--hold=true"},
    };
    for (const auto& form : malformed) {
        const auto args = argv_of(form);
        const auto raw = raw_argv(args);
        CHECK_FALSE(cfg::parse_cli(static_cast<int>(raw.size()), raw.data()).has_value());
    }

    cfg::AppConfig config;
    CHECK_FALSE(config.apply_override("unknown.key=1").has_value());
    CHECK_FALSE(config.apply_override("font.size=0").has_value());
    CHECK_FALSE(config.apply_override("colors.alpha=2").has_value());
    CHECK_FALSE(config.apply_override("colors.regular8=#000000").has_value());
    CHECK_FALSE(config.apply_override("cursor.blink=maybe").has_value());
}

TEST_CASE("split_shell_command and resolve_working_directory handle flags and LaunchServices cwd") {
    const auto zsh = cfg::split_shell_command("/bin/zsh -l");
    CHECK(zsh.executable == "/bin/zsh");
    REQUIRE(zsh.args.size() == 1);
    CHECK(zsh.args[0] == "-l");

    const auto quoted = cfg::split_shell_command("tmux new -As 'my session'");
    CHECK(quoted.executable == "tmux");
    REQUIRE(quoted.args.size() == 3);
    CHECK(quoted.args[0] == "new");
    CHECK(quoted.args[1] == "-As");
    CHECK(quoted.args[2] == "my session");

    const auto blank = cfg::split_shell_command("   ");
    CHECK(blank.executable.empty());
    CHECK(blank.args.empty());

    CHECK(cfg::resolve_working_directory("", "/", "/Users/test") == "/Users/test");
    CHECK(cfg::resolve_working_directory("/", "/", "/Users/test") == "/");
    CHECK(cfg::resolve_working_directory("", "/Users/test/work", "/Users/test").empty());

    const char* standalone_argv[] = {"gritty", "--standalone", "-e", "true"};
    const auto standalone = cfg::parse_cli(4, standalone_argv);
    REQUIRE(standalone.has_value());
    CHECK(standalone->standalone);
    CHECK(standalone->command == "true");

    const char* bad_standalone_argv[] = {"gritty", "--standalone=1"};
    CHECK_FALSE(cfg::parse_cli(2, bad_standalone_argv).has_value());
}
