#include <ChartGroup.h>
#include <note_loader.h>
#include <text_and_file_util.h>

#include <array>
#include <format>
#include <fstream>

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

        std::ifstream input(filename, std::ios::binary);
        if (!input)
            throw std::runtime_error(std::format("couldn't open {} for reading", filename.string()));

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

        const auto hash = util::get_sha256_for_file(filename);
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
