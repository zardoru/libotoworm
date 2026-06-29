#include <filesystem>
#include <rmath.h>
#include <minizip/unzip.h>

#include <future>

#include <fstream>
#include <ChartGroup.h>
#include <note_loader_7k.h>
#include "text_and_file_util.h"

using namespace otoworm;

/*
    This is pretty much the simplest possible loader.

	It's important to consider the most complex part is reading from the zip files.

    The important part is: all notes should contain a start time, possibly an end time (both in seconds)
    and you require at least a single BPM to calculate speed properly. And that's it, set the duration
    of the chart and the loading is done.
*/
void load_ftb_from_string(std::string s, Chart *chart)
{
	Measure msr;
	TimingData timing;

	int cnt = 0;

    for(const auto lines = util::token_split(s, "\r\n", true);
        auto line : lines)
	{
		line = util::trim(line);

		if (line[0] == '#' || line.length() == 0)
			continue;

		auto content = util::token_split(line, " ");

		if (content.at(0) == "BPM")
		{
			TimingSegment seg;
			seg.time = latof(content[1]) / 1000.0;
			seg.value = latof(content[2]);
			timing.push_back(seg);
		}
		else
		{
			/* We'll make a few assumptions about the structure from now on
			->The vertical speeds determine note position, not measure
			->More than one measure is unnecessary when using time-based BPMs,
			so we'll use a single measure for all the song, containing all notes.
			*/

			NoteData note;
			auto noteinfo = util::token_split(content.at(0), "-");
			if (noteinfo.size() > 1)
			{
				note.start = latof(noteinfo.at(0)) / 1000.0;
				note.end_time = latof(noteinfo.at(1)) / 1000.0;
			}
			else
			{
				note.start = latof(noteinfo.at(0)) / 1000.0;
			}

			/* index 1 is unused */
			const int track = atoi(content[2].c_str()); // Always > 1

			chart->duration = std::max(std::max(note.start, note.end_time), chart->duration);
			
			if (track > 0)
				msr.notes[track - 1].push_back(note);

			cnt++;
		}
	}

	// Offsetize
	if (timing.size() == 0)
	{
		timing.push_back(TimingSegment{ 0, 120 });
	}

	chart->transient->measures.push_back(msr);
	chart->transient->bps = bps_from_time_timing(timing, chart->offset);
	chart->level = static_cast<float>(cnt) / chart->duration;
}

void NoteLoaderFTB::load_charts_from_file(const std::filesystem::path& filename, ChartGroup *out)
{
	auto input = unzOpen(filename.string().c_str());

    if (!input)
    {
        return;
    }

	// whatever__stuff.ext -> whatever
	auto filestr = filename.filename().replace_extension("").string();
	auto songname = filestr.substr(0, filestr.find_last_of("_"));

	if (unzGoToFirstFile(input) == UNZ_OK) {
		do {
			unz_file_info info;
			char full_filename[1024];

			if (unzGetCurrentFileInfo(
				input,
				&info,
				full_filename,
				1024,
				NULL, 0, NULL, 0) == UNZ_OK) {
				auto name = std::string(full_filename, strchr(full_filename, '.'));
				if (unzOpenCurrentFile(input) == UNZ_OK) {
					auto diff = std::make_unique<Chart>();

					// read file
					auto len = info.uncompressed_size;
					std::vector<uint8_t> data(len+1);
					unzReadCurrentFile(input, data.data(), len);

					// read file into difficulty
					diff->meta.emplace();
					diff->meta->path = filename;
					diff->meta->path.replace_extension("ft2");

					diff->channels = 7;
					diff->meta->name = name;
					

					diff->transient = std::make_shared<ChartTransient>();
					diff->transient->specialized_info = std::make_shared<StepmaniaChartInfo>();

					std::string ftb_data((const char*)data.data(), len);

					load_ftb_from_string(ftb_data, diff.get());

					// done!
					if (diff->duration > 0)
						out->charts.push_back(std::move(diff));

					unzCloseCurrentFile(input);
				} // opened file
			}
			else continue; // no file info
		} while (unzGoToNextFile(input) == UNZ_OK);
	}

	unzClose(input);

	// done with difficulties. Now metadata
	out->song_filename = filename.filename().replace_extension("ftb");
	out->song_preview_source = out->song_filename;
	if (out->title.empty()) {
		out->title = songname;
	}
}
