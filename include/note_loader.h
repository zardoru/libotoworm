#pragma once

#include "bms.h"
#include "ChartGroup.h"

namespace NoteLoaderSM
{
    void LoadObjectsFromFile(const std::filesystem::path &filename, otoworm::ChartGroup *Out);
}

namespace NoteLoaderSSC
{
    void LoadObjectsFromFile(const std::filesystem::path &filename, otoworm::ChartGroup *Out);
}

namespace NoteLoaderFTB
{
    void load_charts_from_file(const std::filesystem::path& filename, otoworm::ChartGroup *Out);
}

namespace NoteLoaderBMS
{
    std::optional<otoworm::bms::tree> ParseTreeFromString(
        const std::string& data,
        otoworm::bms::parse_error* error = nullptr);
    std::optional<otoworm::bms::tree> ParseTreeFromFile(
        const std::filesystem::path& filename,
        otoworm::bms::parse_error* error = nullptr);
    void load_chart_from_file(const std::filesystem::path& filename, otoworm::ChartGroup *Out);
}

namespace NoteLoaderOM
{
    void LoadObjectsFromFile(const std::filesystem::path& filename, otoworm::ChartGroup *Out);
}

const char *load_ojn_cover(const std::filesystem::path& filename, size_t &read);
namespace NoteLoaderOJN
{
    void LoadObjectsFromFile(const std::filesystem::path& filename, otoworm::ChartGroup *Out);
}

namespace NoteLoaderBMSON
{
    void load_bmson_file(const std::filesystem::path& filename, otoworm::ChartGroup *Out);
}

namespace otoworm
{
    std::shared_ptr<ChartGroup> load_song_from_file(std::filesystem::path filename);
}
