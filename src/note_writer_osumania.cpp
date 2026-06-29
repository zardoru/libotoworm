#include <format>
#include <fstream>
#include <rmath.h>
#include <ChartGroup.h>
#include <ProcessedChart.h>
#include "text_and_file_util.h"


class path;

int track_to_xpos(const int totaltracks, const int track)
{
    const auto base = (512.0 / totaltracks);
    const auto minus = (256.0 / totaltracks);
    return (base * (track + 1) - minus);
}

void convert_to_om(otoworm::ChartGroup *sng, std::filesystem::path path_out, std::string author)
{
	// Log::LogPrintf("Attempt to convert %d difficulties...\n", sng->Difficulties.size());
    for (auto &chart : sng->charts)
    {
        std::string name = sng->title;
        std::string Dname = chart->meta ? chart->meta->name : std::string();
        std::string charter = author;

        otoworm::util::normalize_filename(author, true);
        otoworm::util::normalize_filename(name, true);
        otoworm::util::normalize_filename(Dname, true);
        otoworm::util::normalize_filename(charter, true);

		std::filesystem::path str = 
			path_out / std::format("{} - {} [{}] ({}).osu",
									  author, 
									  name, 
									  Dname, 
									  charter);

		// Log::Printf("Converting into file %s...\n", str.string().c_str());
        std::ofstream out(str.string());

        if (!out.is_open())
        {
            // Log::Printf("Unable to open file %s for writing.\n", str.string().c_str());
            return;
        }

		otoworm::ProcessedChart data = otoworm::ProcessedChart::from(chart.get());

        // First, convert metadata.
        out
            << "osu file format v11\n\n"
            << "[General]\n"
            << "AudioFilename: " << (chart->has_no_audio_stream ? "virtual" : sng->song_filename) << "\n"
            << "AudioLeadIn: 1500\n"
            << "PreviewTime: " << static_cast<int>(sng->preview_time) * 1000 << "\n"
            << "Countdown: 0\n"
            << "SampleSet: None\n"
            << "StackLeniency: 0.7\n"
            << "SpecialStyle: 0\n"
            << "WidescreenStoryboard: 0\n"
            << "Mode: 3\n"
            << "LetterboxInBreaks: 0\n\n"
            << "[Editor]\n"
            << "DistanceSnapping: 0.9\n"
            << "BeatDivisor: 4\n"
            << "GridSize: 16\n"
            << "CurrentTime: 0\n\n";

        out.flush();

        out
            << "[Metadata]\n"
            << "Title: " << sng->title << "\n"
            << "TitleUnicode: " << sng->title << "\n"
            << "Artist: " << sng->artist << "\n"
            << "ArtistUnicode: " << sng->artist << "\n"
            << "Creator: " << author << "\n"
            << "Version: " << Dname << "\n"
            << "Source: \nTags: \nBeatmapID:0\nBeatmapSetID:-1\n\n";

        out.flush();

        out
            << "[difficulty]\n"
            << "HPDrainRate: 8\n"
            << "CircleSize: " << static_cast<int>(chart->channels) << "\n"
            << "Overalldifficulty: 8\n"
            << "ApproachRate: 9\n"
            << "SliderMultiplier: 1.4\n"
            << "SliderTickRate: 1\n\n"
            << "[Events]\n"
            << "// Background and Video events\n"
            << "0,0,\"" << sng->background_filename << "\"\n"
            << "// Storyboard Layer 0 (Background)\n// Storyboard Layer 1 (Fail)\n// Storyboard Layer 2 (Pass)\n"
            << "// Storyboard Layer 3 (Foreground)\n// Storyboard Sound Samples\n";

        // Write bgm events here.
        for (auto bgm : chart->transient->bgm_events)
        {
            auto sndf = chart->transient->sound_list[bgm.sound];
            out << "5," << static_cast<int>(round(bgm.time * 1000)) << ",0,\"" << sndf << "\",100" << std::endl;
        }

        out
            << "// Background Colour Transformations\n"
            << "3,100,163,162,255\n\n";

        out.flush();

        // Then, timing points.
        out << "[TimingPoints]\n";

        for (auto t : data.bps)
        {
            out << t.time * 1000 << "," << 1000 / (t.value ? t.value : 0.00001) << ",4,1,0,15,1,0\n";
            out.flush();
        }

        for (auto t : chart->transient->scrolls)
        {
            out << (t.time + chart->offset) * 1000 << "," << -100 / (t.value ? t.value : 0.00001) << ",4,1,0,15,0,0\n";
            out.flush();
        }

        out << "\n\n[HitObjects]\n";

        // Then, objects.
        for (auto k : chart->transient->measures)
        {
            for (auto n = 0U; n < chart->channels; n++)
            {
                for (auto note : k.notes[n])
                {
                    if (data.is_warping_at(note.start)) continue;

                    out << track_to_xpos(chart->channels, n) << ",0," << static_cast<int>(round(note.start * 1000.0)) << ","
                        << (note.end_time ? "128" : "1") << ",0,";
                    if (note.end_time)
                        out << static_cast<int>(round(note.end_time * 1000.0)) << ",";
					// "sampleSet:additionSet:customIndex:sampleVolume:filename"
					out << "1:0:0:";
					

					if (note.sound)
					{
						out << "100:";
						out << (chart->transient->sound_list.contains(note.sound) ?
							chart->transient->sound_list[note.sound] : "");
					}
					else {
						out << "0:";
					}

                    out << "\n";
                    out.flush();
                }
            }
        }
    }
}

void convert_to_sm_timing(otoworm::ChartGroup *sng, std::filesystem::path path_out)
{
    otoworm::Chart* chart = sng->charts[0].get();
	otoworm::ProcessedChart data = otoworm::ProcessedChart::from(chart);

	if (std::filesystem::is_directory(path_out)) {
        path_out = path_out /  std::format("{} {}.sm", sng->artist, sng->title);
	}

    std::ofstream out(path_out);
	if (!out.is_open()) {
	    throw std::invalid_argument("could not open %s for output.");
	}

    // Technically, stepmania's #OFFSET is actually #GAP, not #OFFSET.
    out << "#OFFSET:" << -chart->offset << ";\n";

    out << "#BPMS:";

    for (auto i = data.bps.begin(); i != data.bps.end(); ++i)
    {
        if (i->value <= 0)
            continue;

        double Beat = quantize_beat(data.bps.integrate_to_time(i->time));
        double Value = i->value * 60;

        out << Beat << "=" << Value;

        if ((i + 1) != data.bps.end()) // Only output comma if there's still stuff to output.
            out << "\n,";
    }

    out << ";";
}

void export_to_bms(otoworm::ChartGroup *sng, std::filesystem::path path_out);
void convert_to_bms(otoworm::ChartGroup *sng, std::filesystem::path path_out)
{
    export_to_bms(sng, path_out);
}
