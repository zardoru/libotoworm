#include <bms.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <random>
#include <sstream>
#include <string_view>

#include <text_and_file_util.h>

namespace otoworm::bms {
namespace {
constexpr int base36(const std::string& text)
{
    return static_cast<int>(std::strtol(text.c_str(), nullptr, 36));
}

std::string lower(std::string text)
{
    std::ranges::transform(text, text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string trim_copy(std::string text)
{
    return util::trim(text);
}

std::shared_ptr<scope> make_scope(scope_type type, const std::shared_ptr<scope>& parent)
{
    auto out = std::make_shared<scope>();
    out->type = type;
    out->parent = parent;
    return out;
}

std::optional<std::shared_ptr<scope>> get_if_root(const std::shared_ptr<scope>& current)
{
    switch (current->type) {
    case scope_type::if_scope:
    case scope_type::else_if_scope:
    case scope_type::else_scope:
        return current->parent.lock();
    case scope_type::random:
        if (const auto parent = current->parent.lock()) {
            return get_if_root(parent);
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

bool has_else_scope(const std::shared_ptr<scope>& if_root)
{
    for (const auto& entry : if_root->children) {
        if (entry.kind == child::type::scope &&
            entry.child_scope &&
            entry.child_scope->type == scope_type::else_scope) {
            return true;
        }
    }
    return false;
}

std::optional<std::shared_ptr<scope>> get_switch_parent(const std::shared_ptr<scope>& current)
{
    switch (current->type) {
    case scope_type::switch_scope:
        return current;
    case scope_type::switch_case:
    case scope_type::switch_default:
        return current->parent.lock();
    default:
        return std::nullopt;
    }
}

bool parse_int(const std::string& text, int& out)
{
    try {
        size_t pos = 0;
        out = std::stoi(text, &pos);
        return pos == text.size();
    } catch (...) {
        return false;
    }
}

void set_error(parse_error* error, parse_error_type type, size_t line, int value = 0, std::string text = {})
{
    if (error) {
        *error = parse_error{type, line, value, std::move(text)};
    }
}

std::optional<std::pair<std::string, std::string>> command_from_line(const std::string& line)
{
    const auto start = line.find('#');
    if (start == std::string::npos) {
        return std::nullopt;
    }

    const auto end_opt = line.find_first_of(" \t", start);
    const auto end = end_opt == std::string::npos ? line.size() : end_opt;
    auto command_name = lower(line.substr(start, end - start));
    auto content = end == line.size() ? std::string{} : trim_copy(line.substr(end + 1));
    return std::make_pair(std::move(command_name), std::move(content));
}

void visit_scope(const std::shared_ptr<scope>& node, const std::function<void(const child&)>& visitor)
{
    for (const auto& entry : node->children) {
        visitor(entry);
        if (entry.kind == child::type::scope && entry.child_scope) {
            visit_scope(entry.child_scope, visitor);
        }
    }
}

struct parse_context {
    tree& output;
    std::shared_ptr<scope>& current;
    parse_error* error;
    std::string command;
    std::string content;
    std::string line;
    size_t line_number{};
};

using parse_handler = bool(*)(parse_context&);

struct parse_dispatch {
    std::string_view command;
    parse_handler handler;
};

void append_raw_line(parse_context& context)
{
    command line_command;
    line_command.type = command_type::raw;
    line_command.name = context.command;
    line_command.content = context.content;
    line_command.raw_line = context.line;
    line_command.line_number = context.line_number;
    context.current->children.push_back(child{child::type::line, line_command, {}});
}

bool handle_setrandom(parse_context& context)
{
    append_raw_line(context);
    return true;
}

bool handle_random(parse_context& context)
{
    int max_roll = 0;
    if (!parse_int(context.content, max_roll)) {
        set_error(context.error, parse_error_type::could_not_parse, context.line_number, 0, context.content);
        return false;
    }
    if (max_roll < 0) {
        set_error(context.error, parse_error_type::out_of_range, context.line_number, max_roll);
        return false;
    }

    if (context.current->type == scope_type::random) {
        if (const auto parent = context.current->parent.lock()) {
            context.current = parent;
        }
    } else if (context.current->type == scope_type::switch_case ||
               context.current->type == scope_type::switch_scope ||
               context.current->type == scope_type::switch_default) {
        set_error(context.error, parse_error_type::disallowed_random, context.line_number);
        return false;
    }

    const auto random_scope = make_scope(scope_type::random, context.current);
    random_scope->max_roll_value = max_roll;
    context.current->children.push_back(child{child::type::scope, {}, random_scope});
    context.current = random_scope;
    return true;
}

bool handle_if(parse_context& context)
{
    int activation = 0;
    if (!parse_int(context.content, activation)) {
        set_error(context.error, parse_error_type::could_not_parse, context.line_number, 0, context.content);
        return false;
    }
    if (context.current->type != scope_type::random) {
        set_error(context.error, parse_error_type::unbound_if, context.line_number);
        return false;
    }

    const auto if_root = make_scope(scope_type::if_root, context.current);
    const auto if_scope = make_scope(scope_type::if_scope, if_root);
    if_scope->activation_value = activation;
    context.current->children.push_back(child{child::type::scope, {}, if_root});
    if_root->children.push_back(child{child::type::scope, {}, if_scope});
    context.current = if_scope;
    return true;
}

bool handle_else_if(parse_context& context)
{
    const auto if_root = get_if_root(context.current);
    if (!if_root || has_else_scope(*if_root)) {
        set_error(context.error, parse_error_type::mismatched_else_if, context.line_number);
        return false;
    }

    int activation = 0;
    if (!parse_int(context.content, activation)) {
        set_error(context.error, parse_error_type::could_not_parse, context.line_number, 0, context.content);
        return false;
    }

    const auto else_if = make_scope(scope_type::else_if_scope, *if_root);
    else_if->activation_value = activation;
    (*if_root)->children.push_back(child{child::type::scope, {}, else_if});
    context.current = else_if;
    return true;
}

bool handle_else(parse_context& context)
{
    const auto if_root = get_if_root(context.current);
    if (!if_root || has_else_scope(*if_root)) {
        set_error(context.error, parse_error_type::mismatched_else, context.line_number);
        return false;
    }

    const auto else_scope = make_scope(scope_type::else_scope, *if_root);
    (*if_root)->children.push_back(child{child::type::scope, {}, else_scope});
    context.current = else_scope;
    return true;
}

bool handle_endif(parse_context& context)
{
    const auto if_root = get_if_root(context.current);
    if (!if_root) {
        set_error(context.error, parse_error_type::mismatched_end_if, context.line_number);
        return false;
    }
    context.current = (*if_root)->parent.lock();
    return true;
}

bool handle_switch(parse_context& context)
{
    int max_roll = 0;
    if (!parse_int(context.content, max_roll)) {
        set_error(context.error, parse_error_type::could_not_parse, context.line_number, 0, context.content);
        return false;
    }

    const auto switch_scope = make_scope(scope_type::switch_scope, context.current);
    switch_scope->max_roll_value = max_roll;
    context.current->children.push_back(child{child::type::scope, {}, switch_scope});
    context.current = switch_scope;
    return true;
}

bool handle_case(parse_context& context)
{
    const auto parent = get_switch_parent(context.current);
    if (!parent) {
        set_error(context.error, parse_error_type::mismatched_switch_case, context.line_number);
        return false;
    }

    int activation = 0;
    if (!parse_int(context.content, activation)) {
        set_error(context.error, parse_error_type::could_not_parse, context.line_number, 0, context.content);
        return false;
    }

    const auto case_scope = make_scope(scope_type::switch_case, *parent);
    case_scope->activation_value = activation;
    (*parent)->children.push_back(child{child::type::scope, {}, case_scope});
    context.current = case_scope;
    return true;
}

bool handle_def(parse_context& context)
{
    const auto parent = get_switch_parent(context.current);
    if (!parent) {
        set_error(context.error, parse_error_type::mismatched_switch_def, context.line_number);
        return false;
    }

    const auto def_scope = make_scope(scope_type::switch_default, *parent);
    (*parent)->children.push_back(child{child::type::scope, {}, def_scope});
    context.current = def_scope;
    return true;
}

bool handle_skip(parse_context& context)
{
    if (context.current->type != scope_type::switch_case &&
        context.current->type != scope_type::switch_default) {
        set_error(context.error, parse_error_type::mismatched_switch_break, context.line_number);
        return false;
    }
    context.current->children.push_back(child{child::type::switch_break, {}, {}});
    return true;
}

bool handle_endsw(parse_context& context)
{
    const auto parent = get_switch_parent(context.current);
    if (!parent) {
        set_error(context.error, parse_error_type::mismatched_switch_end, context.line_number);
        return false;
    }
    context.current = (*parent)->parent.lock();
    return true;
}

bool append_command_line(parse_context& context)
{
    if (context.current->type == scope_type::switch_scope) {
        set_error(context.error, parse_error_type::switch_content_not_in_case, context.line_number);
        return false;
    }

    const auto parsed_line = parse_line(context.line, context.line_number, context.error);
    if (parsed_line && parsed_line->type != command_type::empty) {
        context.current->children.push_back(child{child::type::line, *parsed_line, {}});
    }
    return true;
}

constexpr parse_dispatch parse_dispatch_table[] = {
    {"#setrandom", handle_setrandom},
    {"#random", handle_random},
    {"#if", handle_if},
    {"#elseif", handle_else_if},
    {"#elif", handle_else_if},
    {"#else", handle_else},
    {"#endif", handle_endif},
    {"#switch", handle_switch},
    {"#case", handle_case},
    {"#def", handle_def},
    {"#skip", handle_skip},
    {"#endsw", handle_endsw},
};

parse_handler find_parse_handler(const std::string& command)
{
    for (const auto& entry : parse_dispatch_table) {
        if (entry.command == command) {
            return entry.handler;
        }
    }
    return append_command_line;
}

void evaluate_scope(const std::shared_ptr<scope>& node, std::vector<command>& out, std::mt19937& rng);

void evaluate_if_root(const std::shared_ptr<scope>& node, int roll, std::vector<command>& out, std::mt19937& rng)
{
    std::shared_ptr<scope> fallback;
    for (const auto& entry : node->children) {
        if (entry.kind != child::type::scope || !entry.child_scope) {
            continue;
        }

        const auto& candidate = entry.child_scope;
        if (candidate->type == scope_type::else_scope) {
            fallback = candidate;
            continue;
        }

        if ((candidate->type == scope_type::if_scope ||
             candidate->type == scope_type::else_if_scope) &&
            candidate->activation_value == roll) {
            evaluate_scope(candidate, out, rng);
            return;
        }
    }

    if (fallback) {
        evaluate_scope(fallback, out, rng);
    }
}

void evaluate_switch(const std::shared_ptr<scope>& node, std::vector<command>& out, std::mt19937& rng)
{
    if (node->max_roll_value <= 0) {
        return;
    }

    std::uniform_int_distribution<int> range(1, node->max_roll_value);
    const auto roll = range(rng);
    std::shared_ptr<scope> selected;
    std::shared_ptr<scope> fallback;

    for (const auto& entry : node->children) {
        if (entry.kind != child::type::scope || !entry.child_scope) {
            continue;
        }

        if (entry.child_scope->type == scope_type::switch_case &&
            entry.child_scope->activation_value == roll) {
            selected = entry.child_scope;
            break;
        }

        if (entry.child_scope->type == scope_type::switch_default) {
            fallback = entry.child_scope;
        }
    }

    if (!selected) {
        selected = fallback;
    }

    if (selected) {
        evaluate_scope(selected, out, rng);
    }
}

void evaluate_scope(const std::shared_ptr<scope>& node, std::vector<command>& out, std::mt19937& rng)
{
    int random_roll = 0;
    if (node->type == scope_type::random && node->max_roll_value > 0) {
        std::uniform_int_distribution<int> range(1, node->max_roll_value);
        random_roll = range(rng);
    }

    for (const auto& entry : node->children) {
        if (entry.kind == child::type::line) {
            out.push_back(entry.line);
            continue;
        }

        if (entry.kind != child::type::scope || !entry.child_scope) {
            continue;
        }

        switch (entry.child_scope->type) {
        case scope_type::if_root:
            evaluate_if_root(entry.child_scope, random_roll, out, rng);
            break;
        case scope_type::switch_scope:
            evaluate_switch(entry.child_scope, out, rng);
            break;
        default:
            evaluate_scope(entry.child_scope, out, rng);
            break;
        }
    }
}
}

channel_value parse_channel(const std::string& text)
{
    const auto normalized = lower(text);
    if (normalized == "01") return {channel::bgm, 1};
    if (normalized == "02") return {channel::meter, 2};
    if (normalized == "03") return {channel::bpm, 3};
    if (normalized == "04") return {channel::bga_base, 4};
    if (normalized == "06") return {channel::bga_poor, 6};
    if (normalized == "07") return {channel::bga_layer, 7};
    if (normalized == "08") return {channel::ex_bpm, 8};
    if (normalized == "09") return {channel::stops, 9};
    if (normalized == "0a") return {channel::bga_layer_2, 10};
    if (normalized == "sp") return {channel::speeds, base36(normalized)};
    if (normalized == "sc") return {channel::scrolls, base36(normalized)};
    return {channel::other, base36(normalized)};
}

playable_channel_info playable_channel_from(const channel_value value)
{
    if (value.kind != channel::other) {
        return {};
    }

    struct range {
        int start;
        play_side side;
        note_channel kind;
    };

    const range ranges[] = {
        {base36("11"), play_side::p1, note_channel::visible},
        {base36("51"), play_side::p1, note_channel::long_note},
        {base36("31"), play_side::p1, note_channel::invisible},
        {base36("D1"), play_side::p1, note_channel::mine},
        {base36("21"), play_side::p2, note_channel::visible},
        {base36("61"), play_side::p2, note_channel::long_note},
        {base36("41"), play_side::p2, note_channel::invisible},
        {base36("E1"), play_side::p2, note_channel::mine},
    };

    for (const auto& range : ranges) {
        const auto relative = value.value - range.start;
        if (relative >= 0 && relative <= 35) {
            return {
                range.side,
                range.kind,
                value.value,
                range.start,
                relative,
                true
            };
        }
    }

    return {};
}

std::optional<command> parse_line(const std::string& line, size_t line_number, parse_error* error)
{
    auto parsed = command_from_line(line);
    if (!parsed) {
        return std::nullopt;
    }

    command out;
    out.name = parsed->first;
    out.content = parsed->second;
    out.raw_line = line;
    out.line_number = line_number;

    const auto colon = out.name.find(':');
    if (out.name.size() >= 7 && colon == 6) {
        try {
            out.type = command_type::events;
            out.measure = std::stoi(out.name.substr(1, 3));
            out.event_channel = parse_channel(out.name.substr(4, 2));

            const auto event_text = out.name.substr(7);
            out.content = event_text;
            const auto pair_count = event_text.size() / 2;
            out.events.reserve(pair_count);
            for (size_t i = 0; i < pair_count; ++i) {
                out.events.push_back(static_cast<uint16_t>(base36(event_text.substr(i * 2, 2))));
            }
            return out;
        } catch (...) {
            set_error(error, parse_error_type::could_not_parse, line_number, 0, out.name);
            return std::nullopt;
        }
    }

    out.type = command_type::raw;
    return out;
}

std::optional<tree> tree::from_file(const std::filesystem::path& path, parse_error* error)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        set_error(error, parse_error_type::could_not_parse, 0, 0, path.string());
        return std::nullopt;
    }

    std::stringstream data;
    data << input.rdbuf();
    return from_string(data.str(), error);
}

std::optional<tree> tree::from_string(const std::string& data, parse_error* error)
{
    tree out;
    out.root = std::make_shared<scope>();
    out.root->type = scope_type::root;

    auto current = out.root;
    std::stringstream input(data);
    std::string line;
    size_t line_number = 0;

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        auto parsed = command_from_line(line);
        if (!parsed) {
            ++line_number;
            continue;
        }

        parse_context context{
            out,
            current,
            error,
            parsed->first,
            parsed->second,
            line,
            line_number,
        };

        if (!find_parse_handler(context.command)(context)) {
            return std::nullopt;
        }

        ++line_number;
    }

    return out;
}

std::vector<command> tree::evaluate() const
{
    std::vector<command> out;
    std::mt19937 rng{};
    evaluate_scope(root, out, rng);
    return out;
}

void tree::visit(const std::function<void(const child&)>& visitor) const
{
    visit_scope(root, visitor);
}

}
