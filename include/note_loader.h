#pragma once

#include "bms.h"
#include "ChartGroup.h"

#include <istream>

namespace NoteLoaderSM
{
    void LoadObjectsFromStream(std::istream& input, otoworm::ChartGroup *Out);
}

namespace NoteLoaderSSC
{
    void LoadObjectsFromStream(std::istream& input, otoworm::ChartGroup *Out);
}

namespace NoteLoaderFTB
{
    void load_charts_from_stream(std::istream& input, otoworm::ChartGroup *Out);
}

namespace NoteLoaderBMS
{
    std::optional<otoworm::bms::tree> ParseTreeFromString(
        const std::string& data,
        otoworm::bms::parse_error* error = nullptr);
    std::optional<otoworm::bms::tree> ParseTreeFromStream(
        std::istream& input,
        otoworm::bms::parse_error* error = nullptr);
    void load_chart_from_stream(std::istream& input, otoworm::ChartGroup *Out, bool is_pms = false);
}

namespace NoteLoaderOM
{
    void LoadObjectsFromStream(std::istream& input, otoworm::ChartGroup *Out);
}

const char *load_ojn_cover(const std::filesystem::path& filename, size_t &read);
namespace NoteLoaderOJN
{
    void LoadObjectsFromStream(std::istream& input, otoworm::ChartGroup *Out);
}

namespace NoteLoaderBMSON
{
    void load_bmson_stream(std::istream& input, otoworm::ChartGroup *Out);
}

namespace otoworm
{
    std::shared_ptr<ChartGroup> load_song_from_file(std::filesystem::path filename);
}
