#include <ChartGroup.h>
#include <converter.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <vector>

namespace
{
    int count_interval(const otoworm::Chart& chart, const double time_start, const double time_end)
    {
        if (!chart.transient)
            return 0;

        int out = 0;
        for (const auto& measure : chart.transient->measures)
        {
            for (int channel = 0; channel < chart.channels; ++channel)
            {
                for (const auto& note : measure.notes[channel])
                {
                    const auto starts_inside = note.start >= time_start && note.start < time_end;
                    const auto ends_inside = note.end_time != 0 && note.end_time >= time_start && note.end_time < time_end;
                    if (starts_inside || ends_inside)
                        ++out;
                }
            }
        }

        return out;
    }

    std::vector<int> get_data_points(const otoworm::Chart& chart, const double interval_duration)
    {
        std::vector<int> datapoints;
        const auto interval_count = static_cast<int>(std::ceil(chart.duration / interval_duration));
        datapoints.reserve(interval_count);

        for (int i = 0; i < interval_count; ++i)
            datapoints.push_back(count_interval(chart, i * interval_duration, (i + 1) * interval_duration));

        return datapoints;
    }

    uint32_t get_scorable_note_count(const otoworm::Chart& chart)
    {
        if (!chart.transient)
            return 0;

        return chart.transient->get_scorable_note_count();
    }

    std::string get_svg_text(
        const otoworm::ChartGroup& group,
        const otoworm::Chart& chart,
        const double interval_duration = 1,
        const double peak_margin = 1.2)
    {
        using std::endl;

        std::stringstream out;
        const auto data_points = get_data_points(chart, interval_duration);
        const auto peak_it = std::max_element(data_points.begin(), data_points.end());
        const auto peakf = peak_it == data_points.end() ? 0.0F : static_cast<float>(*peak_it);
        const auto peak = std::max(peakf * peak_margin, 1.0);

        out << "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\">" << endl;

        size_t point_index = 0;
        constexpr float image_height = 300;
        constexpr float graph_y_offset = 50;
        constexpr float graph_x_offset = 100;
        constexpr float text_x_offset = 20;
        constexpr float text_y_offset = 20;
        constexpr float interval_width = 10;
        constexpr float real_graph_width = 1000;

        const auto graph_width = std::max(static_cast<float>(data_points.size()) * interval_width, 1.0F);
        const auto x_ratio = real_graph_width / graph_width;

        const auto left = graph_x_offset;
        const auto bottom = graph_y_offset + image_height;
        const auto right = graph_x_offset + real_graph_width;
        const auto top = graph_y_offset;

        auto chart_name = chart.meta ? chart.meta->name : std::string();
        auto chart_author = chart.meta ? chart.meta->author : std::string();
        if (chart_author.empty())
            chart_author = "an anonymous charter";

        const auto average_nps = chart.duration > 0 ? get_scorable_note_count(chart) / chart.duration : 0;

        out << std::format(
            "<text x=\"{:.0f}\" y=\"{:.0f}\" fill=\"black\">{} - {} ({}) by {} (Max NPS: {:.2f}/Avg NPS: {:.2f})</text>",
            text_x_offset,
            text_y_offset,
            group.title,
            group.artist,
            chart_name,
            chart_author,
            peakf / interval_duration,
            average_nps) << endl;

        out << std::format(
            "\t<line x1=\"{:.0f}\" y1=\"{:.0f}\" x2=\"{:.0f}\" y2=\"{:.0f}\" style=\"stroke:rgb(0,0,0);stroke-width:4\"/>",
            left,
            bottom,
            right,
            bottom) << endl;
        out << std::format(
            "\t<line x1=\"{:.0f}\" y1=\"{:.0f}\" x2=\"{:.0f}\" y2=\"{:.0f}\" style=\"stroke:rgb(0,0,0);stroke-width:4\"/>",
            left,
            top,
            left,
            bottom) << endl;

        constexpr auto marker_count = 5;
        for (auto i = 1; i <= marker_count; ++i)
        {
            const auto x = left - graph_x_offset / 2;
            const auto y = bottom - i * (image_height / marker_count / peak_margin);
            const auto value = peakf * i / marker_count / interval_duration;
            out << std::format("\t<text x=\"{:.0f}\" y=\"{:.0f}\" fill=\"black\">{:.2f}</text>", x, y, value) << endl;
            out << std::format(
                "\t<line x1=\"{:.0f}\" y1=\"{:.0f}\" x2=\"{:.0f}\" y2=\"{:.0f}\" style=\"stroke:rgb(0,0,0);stroke-width:0.5\"/>",
                x,
                y,
                right,
                y) << endl;
        }

        for (const auto point : data_points)
        {
            const auto relative_frequency = point / peak;
            const auto next_relative_frequency =
                point_index + 1 < data_points.size() ? data_points[point_index + 1] / peak : 0;

            const auto x1 = static_cast<int>(interval_width * point_index * x_ratio + graph_x_offset);
            const auto y1 = static_cast<int>(image_height - image_height * relative_frequency + graph_y_offset);
            const auto x2 = static_cast<int>(interval_width * (point_index + 1) * x_ratio + graph_x_offset);
            const auto y2 = static_cast<int>(image_height - image_height * next_relative_frequency + graph_y_offset);

            out << std::format(
                "\t<line x1=\"{}\" y1=\"{}\" x2=\"{}\" y2=\"{}\" style=\"stroke:rgb(255,0,0);stroke-width:2\"/>\n",
                x1,
                y1,
                x2,
                y2);

            ++point_index;
        }

        out << "</svg>";
        return out.str();
    }
}

void convert_to_nps_graph(otoworm::ChartGroup* group, std::filesystem::path path_out)
{
    for (const auto& chart : group->charts)
    {
        if (!chart)
            continue;

        const auto chart_name = chart->meta ? chart->meta->name : std::string();
        const auto chart_author = chart->meta ? chart->meta->author : std::string();
        const auto filename = std::format("{} ({}) - {}.svg", group->title, chart_name, chart_author);
        const auto output_path = path_out / filename;

        std::ofstream out(output_path.string());
        if (out.is_open())
            out << get_svg_text(*group, *chart);
    }
}
