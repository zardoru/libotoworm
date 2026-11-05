#pragma once

#include "bms.h"
#include "ChartGroup.h"

#include <istream>

namespace NoteLoaderSM
{
    void load_objects_from_stream(std::istream& input, otoworm::ChartGroup *out);
}

namespace NoteLoaderSSC
{
    void load_objects_from_stream(std::istream& input, otoworm::ChartGroup *out);
}

namespace NoteLoaderFTB
{
    void load_charts_from_stream(std::istream& input, otoworm::ChartGroup *out);
}

namespace NoteLoaderBMS
{
    void load_chart_from_stream(std::istream& input, otoworm::ChartGroup *out, bool is_pms = false);
}

namespace NoteLoaderOM
{
    void load_objects_from_stream(std::istream& input, otoworm::ChartGroup *out);
}

std::unique_ptr<uint8_t[]> load_ojn_cover(const std::filesystem::path& filename, size_t &read);
namespace NoteLoaderOJN
{
    void load_objects_from_stream(std::istream& input, otoworm::ChartGroup *out);
}

namespace NoteLoaderBMSON
{
    void load_bmson_stream(std::istream& input, otoworm::ChartGroup *out);
}

namespace otoworm
{
    std::shared_ptr<ChartGroup> load_song_from_file(std::filesystem::path filename);
}
