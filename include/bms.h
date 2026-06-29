#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace otoworm::bms {

enum class channel {
    bgm,
    meter,
    bpm,
    bga_base,
    bga_poor,
    bga_layer,
    ex_bpm,
    stops,
    bga_layer_2,
    speeds,
    scrolls,
    other,
};

enum class play_side {
    p1,
    p2,
};

enum class note_channel {
    visible,
    long_note,
    invisible,
    mine,
    none,
};

struct channel_value {
    channel kind{channel::other};
    int value{};

    bool operator<(const channel_value& other) const
    {
        if (kind != other.kind)
            return kind < other.kind;
        return value < other.value;
    }

    bool operator==(const channel_value& other) const
    {
        return kind == other.kind && value == other.value;
    }
};

struct playable_channel_info {
    play_side side{play_side::p1};
    note_channel kind{note_channel::none};
    int value{};
    int start{};
    int relative{};
    bool valid{};
};

enum class scope_type {
    root,
    random,
    if_root,
    if_scope,
    else_if_scope,
    else_scope,
    switch_scope,
    switch_case,
    switch_default,
};

enum class command_type {
    raw,
    events,
    empty,
};

struct command {
    std::string name;
    std::string content;
    std::string raw_line;
    size_t line_number{};
    command_type type{command_type::empty};
    int measure{};
    channel_value event_channel{};
    std::vector<uint16_t> events;
};

enum class parse_error_type {
    disallowed_random,
    mismatched_else_if,
    mismatched_else,
    mismatched_end_if,
    mismatched_switch_case,
    switch_content_not_in_case,
    mismatched_switch_break,
    mismatched_switch_def,
    mismatched_switch_end,
    out_of_range,
    unbound_if,
    could_not_parse,
};

struct parse_error {
    parse_error_type type;
    size_t line_number{};
    int value{};
    std::string text;
};

struct scope;

struct child {
    enum class type {
        line,
        scope,
        switch_break,
    };

    type kind{type::line};
    command line;
    std::shared_ptr<scope> child_scope;
};

struct scope {
    scope_type type{scope_type::root};
    int activation_value{};
    int max_roll_value{};
    std::vector<child> children;
    std::weak_ptr<scope> parent;
};

class tree {
public:
    std::shared_ptr<scope> root;

    static std::optional<tree> from_string(const std::string& data, parse_error* error = nullptr);
    static std::optional<tree> from_file(const std::filesystem::path& path, parse_error* error = nullptr);

    std::vector<command> evaluate() const;
    void visit(const std::function<void(const child&)>& visitor) const;
};

std::optional<command> parse_line(const std::string& line, size_t line_number, parse_error* error = nullptr);
channel_value parse_channel(const std::string& text);
playable_channel_info playable_channel_from(channel_value value);

}
