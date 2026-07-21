#include <filesystem>
#include <rmath.h>
#include <minizip/unzip.h>

#include <future>

#include <fstream>
#include <istream>
#include <ChartGroup.h>
#include <cstring>
#include <note_loader.h>
#include "text_and_file_util.h"

using namespace otoworm;

namespace
{
    voidpf ZCALLBACK open_stream(voidpf opaque, const void*, int)
    {
        return opaque;
    }

    uLong ZCALLBACK read_stream(voidpf, voidpf stream, void* buffer, uLong size)
    {
        auto& input = *static_cast<std::istream*>(stream);
        input.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
        return static_cast<uLong>(input.gcount());
    }

    ZPOS64_T ZCALLBACK tell_stream(voidpf, voidpf stream)
    {
        const auto position = static_cast<std::istream*>(stream)->tellg();
        return position == std::streampos(-1) ? 0 : static_cast<ZPOS64_T>(position);
    }

    long ZCALLBACK seek_stream(voidpf, voidpf stream, ZPOS64_T offset, int origin)
    {
        auto& input = *static_cast<std::istream*>(stream);
        input.clear();

        const auto direction = origin == ZLIB_FILEFUNC_SEEK_CUR ? std::ios::cur
            : origin == ZLIB_FILEFUNC_SEEK_END ? std::ios::end
            : std::ios::beg;
        input.seekg(static_cast<std::streamoff>(offset), direction);
        return input ? 0 : -1;
    }

    int ZCALLBACK close_stream(voidpf, voidpf)
    {
        return 0;
    }

    int ZCALLBACK stream_error(voidpf, voidpf stream)
    {
        return static_cast<std::istream*>(stream)->bad();
    }

    zlib_filefunc64_def stream_file_functions(std::istream& input)
    {
        return {
            open_stream,
            read_stream,
            nullptr,
            tell_stream,
            seek_stream,
            close_stream,
            stream_error,
            &input,
        };
    }
}

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

void NoteLoaderFTB::load_charts_from_stream(std::istream& input, ChartGroup *out)
{
    auto file_functions = stream_file_functions(input);
	auto archive = unzOpen2_64(&input, &file_functions);

    if (!archive)
    {
        return;
    }

	if (unzGoToFirstFile(archive) == UNZ_OK) {
		do {
			unz_file_info info;
			char full_filename[1024];

			if (unzGetCurrentFileInfo(
					archive,
				&info,
				full_filename,
				1024,
				NULL, 0, NULL, 0) == UNZ_OK) {
				auto name = std::string(full_filename, strchr(full_filename, '.'));
				if (unzOpenCurrentFile(archive) == UNZ_OK) {
					auto diff = std::make_unique<Chart>();

					// read file
					auto len = info.uncompressed_size;
					std::vector<uint8_t> data(len+1);
					unzReadCurrentFile(archive, data.data(), len);

					// read file into difficulty
					diff->meta.emplace();

					diff->channels = 7;
					diff->meta->name = name;
					

					diff->transient = std::make_shared<ChartTransient>();
					diff->transient->specialized_info = std::make_shared<StepmaniaChartInfo>();

					std::string ftb_data((const char*)data.data(), len);

					load_ftb_from_string(ftb_data, diff.get());

					// done!
					if (diff->duration > 0)
						out->charts.push_back(std::move(diff));

					unzCloseCurrentFile(archive);
				} // opened file
			}
			else continue; // no file info
		} while (unzGoToNextFile(archive) == UNZ_OK);
	}

	unzClose(archive);
}
