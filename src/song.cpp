#include <ChartGroup.h>
#include <note_loader.h>
#include <text_and_file_util.h>

#include <array>
#include <format>
#include <fstream>
#include <sstream>

using namespace otoworm;

ChartClass ChartInfo::get_class() const
{
    return type;
}

Chart* ChartGroup::get_chart(const uint32_t i) const {
    if (i >= charts.size())
        return nullptr;
    else
        return charts.at(i).get();
}

uint8_t ChartGroup::get_chart_count() const {
	return charts.size();
}

void Chart::reset_transient()
{
    if (transient)
        transient = nullptr;

}


uint32_t ChartTransient::get_total_note_count() const {
	uint32_t cnt = 0;
	for (const auto measure : measures) {
		for (const auto & Note : measure.notes) {
			for ([[maybe_unused]] auto note : Note) {
				cnt++;
			}
		}
	}

	return cnt;
}

uint32_t ChartTransient::get_scorable_note_count() const {
	uint32_t cnt = 0;
	for (auto measure : measures) {
		for (auto & Note : measure.notes) {
			for (const auto note : Note) {
				if (note.type == NK_FAKE ||
					note.type == NK_INVISIBLE ||
					note.type == NK_MINE)
					continue;

				if (note.end_time != 0 && 
					note.type == NK_NORMAL)
					cnt += 2;
				else
					cnt++;
			}
		}
	}

	return cnt;
}

double Chart::time_for_beat(const double beat) const
{
	if (!transient)
		return 0;

	return transient->bps.time_at_integrated_value(beat);
}

double Chart::beat_at_time(const double time) const
{
	if (!transient)
		return 0;

	return transient->bps.integrate_to_time(time);
}

namespace
{
    struct loader_entry
    {
        std::wstring_view extension;
        void (*load)(std::istream&, ChartGroup*);
    };

    void load_bms(std::istream& input, ChartGroup* group)
    {
        NoteLoaderBMS::load_chart_from_stream(input, group);
    }

    void load_pms(std::istream& input, ChartGroup* group)
    {
        NoteLoaderBMS::load_chart_from_stream(input, group, true);
    }

    constexpr auto loaders = std::array{
        loader_entry{L".bms", load_bms},
        loader_entry{L".bme", load_bms},
        loader_entry{L".bml", load_bms},
        loader_entry{L".pms", load_pms},
        loader_entry{L".sm", NoteLoaderSM::LoadObjectsFromStream},
        loader_entry{L".osu", NoteLoaderOM::LoadObjectsFromStream},
        loader_entry{L".ft2", NoteLoaderFTB::load_charts_from_stream},
        loader_entry{L".ojn", NoteLoaderOJN::LoadObjectsFromStream},
        loader_entry{L".ssc", NoteLoaderSSC::LoadObjectsFromStream},
        loader_entry{L".bmson", NoteLoaderBMSON::load_bmson_stream},
    };
}

std::shared_ptr<ChartGroup> otoworm::load_song_from_file(std::filesystem::path filename)
{
    if (!filename.has_extension())
        return nullptr;

    filename = std::filesystem::absolute(filename);
    const auto extension = filename.extension().wstring();

    auto group = std::make_shared<ChartGroup>();
    group->path = filename.parent_path();

    for (const auto& loader : loaders)
    {
        if (extension != loader.extension)
            continue;

        std::ifstream file_input(filename, std::ios::binary | std::ios::ate);
        if (!file_input)
            throw std::runtime_error(std::format("couldn't open {} for reading", filename.string()));

        const auto file_size = file_input.tellg();
        if (file_size == std::streampos(-1))
            throw std::runtime_error(std::format("couldn't determine the size of {}", filename.string()));

        const auto byte_count = static_cast<std::streamsize>(file_size);
        if (byte_count < 0)
            throw std::runtime_error(std::format("{} is too large to read", filename.string()));

        std::string file_data(static_cast<size_t>(byte_count), '\0');
        file_input.seekg(0);
        file_input.read(file_data.data(), byte_count);
        if (file_input.gcount() != byte_count)
            throw std::runtime_error(std::format("couldn't read {} completely", filename.string()));

        const auto hash = util::get_sha256_for_data(file_data);
        std::istringstream input(std::move(file_data), std::ios::in | std::ios::binary);
        loader.load(input, group.get());

        if (extension == L".ft2")
        {
            auto audio_filename = filename.filename();
            audio_filename.replace_extension("ftb");
            group->song_filename = audio_filename;
            group->song_preview_source = audio_filename;

            if (group->title.empty())
            {
                const auto stem = filename.stem().string();
                group->title = stem.substr(0, stem.find_last_of("_"));
            }
        }

        auto chart_index = 0;
        for (auto& chart : group->charts)
        {
            if (chart->meta)
            {
                chart->meta->path = filename;
                if (chart->meta->name.empty())
                    chart->meta->name = filename.stem().string();
            }

            if (!chart->transient)
                continue;

            if (extension == L".ojn")
                chart->transient->stage_file = locale::wstring_to_utf8(filename.filename().wstring());

            chart->transient->file_hash = hash;
            if (chart->transient->index_in_file == -1)
                chart->transient->index_in_file = chart_index++;
        }

        return group;
    }

    return nullptr;
}
