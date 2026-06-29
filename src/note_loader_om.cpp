#include "rmath.h"

#include <ChartGroup.h>
#include <note_loader_7k.h>

#include <format>
#include <fstream>
#include <regex>
#include <sstream>
#include "text_and_file_util.h"

/* osu!mania loader. credits to wanwan159, woc2006, Zorori and the author of AIBat for helping me understand this. */

typedef std::vector<std::string> SplitResult;

using namespace otoworm;

constexpr auto SAMPLESET_NORMAL = 1;
constexpr auto SAMPLESET_SOFT = 2;
constexpr auto SAMPLESET_DRUM = 3;

constexpr auto HITSOUND_NORMAL = 0;
constexpr auto HITSOUND_WHISTLE = 2;
constexpr auto HITSOUND_FINISH = 4;
constexpr auto HITSOUND_CLAP = 8;

constexpr auto NOTE_SLIDER = 2;
constexpr auto NOTE_HOLD = 128;
constexpr auto NOTE_NORMAL = 1;

constexpr double LINE_REMOVE_THRESHOLD = 0.0012;

// CfgVar DebugOsuLoader("OsuLoader", "Debug");

std::string get_sampleset_str(const int sampleset)
{
	switch (sampleset)
	{
	case SAMPLESET_SOFT:
		return "soft";
	case SAMPLESET_DRUM:
		return "drum";
	case SAMPLESET_NORMAL:
	default:
		return "normal";
	}	
}


enum OsuLoaderReadingState
{
    RNotKnown,
    RGeneral,
    RMetadata,
    RDifficulty,
    REvents,
    RTiming,
    RHitobjects
};

void set_reading_mode(std::string& line, OsuLoaderReadingState& reading_mode)
{
	struct
	{
		std::string cnt;
		OsuLoaderReadingState val;
	} state_table[] = {
		{"[General]", RGeneral},
		{"[Metadata]", RMetadata},
		{"[Difficulty]", RDifficulty},
		{"[Events]", REvents},
		{"[TimingPoints]", RTiming},
		{"[HitObjects]", RHitobjects},
	};

	for (const auto& x: state_table)
	{
		if (x.cnt == line) { reading_mode = x.val; return; }
	}

	if (line.find_first_of('[') == 0)
		reading_mode = RNotKnown;
}

struct hitsound_section
{
    int sampleset;
    int volume; // In %
    int custom;
    int is_inherited;
    double time; // In Seconds
    double value; // BPM or Multiplier
    double measure_len; // In quarter notes
    // bool Kiai;
    bool omit;
};

bool operator <(const hitsound_section& lhs, const hitsound_section& rhs)
{
    return lhs.time < rhs.time;
}

bool operator< (const hitsound_section& lhs, const double rhs) {
	return lhs.time < rhs;
}

int get_track_from_position(const float Position, const int Channels)
{
    const float Step = 512.f / Channels;

    return static_cast<int>(Position / Step);
}

class OsuManiaLoaderException : public std::exception
{
private:
	std::string msg;
public:
	OsuManiaLoaderException(const char * what) : exception(), msg(what) {}
	const char* what() const noexcept { return msg.c_str(); }
};

class OsumaniaLoader
{
    double slider_velocity;
    int Version;
    int last_sound_index;
    ChartGroup *osu_sng;
    std::shared_ptr<OsumaniaChartInfo> info;
    std::map <std::string, int> sounds;
    std::vector<hitsound_section> hitsound_sections;
    TimingData timing;
    std::unique_ptr<Chart> chart;
    std::string Defaultsampleset;

	std::stringstream EventsContent;

    bool ReadAModeTag;

    std::vector<NoteData> notes[MAX_CHANNELS];
    int line_number;

	void offsetize()
	{
		const auto first = find_first_measure();
		chart->offset = first->time;

		for (auto & hitsound_section : hitsound_sections)
		{
			hitsound_section.time -= chart->offset;
		}
	}

	std::vector<hitsound_section>::iterator find_first_measure()
	{
		auto i = hitsound_sections.begin();
		auto next = i + 1;
		while (i != hitsound_sections.end() && i->is_inherited)
		{
			++i;
			next = i + 1;
			if (i != hitsound_sections.end() && next != hitsound_sections.end())
			{
				if (!i->is_inherited && !next->is_inherited)
				{
					while (i->time - next->time < 0.001 &&
							i != hitsound_sections.end() &&
							next != hitsound_sections.end())
					{
						i++;
						next = i + 1;
					}
				}
			}
		}

		return i != hitsound_sections.end() ? i : hitsound_sections.begin();
	}

	void measurize_from_timing_data()
	{
		offsetize();

		auto seclst = filter([](const hitsound_section& H) 
		{
			return !H.is_inherited && !H.omit;
		}, hitsound_sections);

		for (auto i = seclst.begin(); i != seclst.end();)
		{
			double SectionDurationInBeats = 0;
			double SectionDurationInSecondsTotal = 0;

			/* 
			
			if there's an older one that is 1ms later
			 we've got to skip to it osu doesn't display the current one
			it just displays the last one
			thanks nivrad00 for finding this glitch
			
			Emulating the skip bug implies we
			skip all of the following lines 
			up to the next one 
			that does not have a line with a line before it 
			with a time distance less than 1ms. 

			The most important detail about it is that 
			the displacement of the measure line 
			is actually applied
			it's merely not shown
			*/
			auto next = i + 1;
			if (next != seclst.end()) /* not the last item case */
			{
				const auto subsection_duration = (next->time - i->time) * bpm_to_bps(i->value);
				SectionDurationInBeats += subsection_duration;
				SectionDurationInSecondsTotal += (next->time - i->time);

				//if (DebugOsuLoader)
				//	Log::LogPrintf("\nMCALC: %f to %f (%f beats long at %f bpm)", next->time, i->time, bt, i->value);

				// the next section, and the one after that
				auto c_next = next;
				auto sk_next = next + 1;

				// add nivrad's bug
				do {
					// and that next line has a time distance of less than the threshold
					if (sk_next != seclst.end() && sk_next->time - c_next->time <= LINE_REMOVE_THRESHOLD) 
					{
						// ah the one after and the next are less than 1ms apart
						// get the displacement in beats of this section
						const auto seg = (sk_next->time - c_next->time) * bpm_to_bps(c_next->value);
						//if (DebugOsuLoader)
						//	Log::LogPrintf("\nMCALC: %f to %f (section is %g beats long)", sk_next->time, c_next->time, seg);
						SectionDurationInBeats += seg; // we displace along here
						SectionDurationInSecondsTotal += (sk_next->time - c_next->time);
						++c_next;
						++sk_next;
					}
					else {
						break; // otherwise we stop looking for the next line
					}
				} while (true);

				// either what we skipped or the inmediate next
				// if what we skipped we're out of the 1ms woods
				// anyway, the one after lasts more than 1ms
				next = c_next;
			} else /* last item case */
			{
				const auto section_duration = bpm_to_bps(i->value) * (chart->duration - i->time);
				SectionDurationInBeats += section_duration;
				//if (DebugOsuLoader)
				//	Log::LogPrintf("\nMCALC: Last seg. %f to %f (%g beats long)", Diff->duration, i->time, seg);

			}
			
			auto TotalMeasuresThisSection = SectionDurationInBeats / i->measure_len;
			const auto MaxMeasures = SectionDurationInSecondsTotal / LINE_REMOVE_THRESHOLD;

			/* this is nivrad's bug again, but for its actual intended purpose - capping measure lines */
			if (TotalMeasuresThisSection > MaxMeasures)
			{
				i->measure_len = SectionDurationInBeats;
				TotalMeasuresThisSection = 1; 
			}
			
			//if (DebugOsuLoader)
			//	Log::LogPrintf("\nTotal Measures: %g, beats: %g, mlen: %f", TotalMeasuresThisSection, SectionDurationInBeats, i->measure_len);



			const auto Whole = floor(TotalMeasuresThisSection);
			const auto Fraction = TotalMeasuresThisSection - Whole;

			// Add the measures.
			for (auto k = 0; k < Whole; k++)
			{
				Measure Msr;
				Msr.length = i->measure_len;
				chart->transient->measures.push_back(Msr);
			}

			if (Fraction > 0)
			{
				const auto dur_secs = Fraction * i->measure_len * spb(i->value);
				if (dur_secs > LINE_REMOVE_THRESHOLD) {
					Measure Msr;
					Msr.length = Fraction * (i)->measure_len;
					chart->transient->measures.push_back(Msr);
				}
				else {
					if (!chart->transient->measures.empty())
						chart->transient->measures.back().length += Fraction * i->measure_len;
				}
			}

			i = next;
		}
	}

	
	void push_notes_to_measures()
	{
		const TimingData& BPS = chart->transient->bps;

		for (int k = 0; k < MAX_CHANNELS; k++)
		{
			for (auto i = notes[k].begin(); i != notes[k].end(); ++i)
			{
				double Beat;
				double CurrentBeat = 0; // Lower bound of this measure

				if (!isnan(i->start)) {
                    Beat = quantize_beat(BPS.integrate_to_time(i->start));
				} else {
				    if (isnan(i->end_time))
				        continue; // ???? why

                    i->type = NK_FAKE;
                    i->start = i->end_time;
                    i->end_time = 0;
				    Beat = quantize_beat(BPS.integrate_to_time(i->end_time));
				}

				if (Beat < 0)
				{
					chart->transient->measures[0].notes[k].push_back(*i);
					continue;
				}

				for (auto m = chart->transient->measures.begin(); m != chart->transient->measures.end(); ++m)
				{
					double NextBeat = std::numeric_limits<double>::infinity();
					auto nextm = m + 1;

					if (nextm != chart->transient->measures.end()) // Higher bound of this measure
						NextBeat = CurrentBeat + m->length;

					if (Beat >= CurrentBeat && Beat < NextBeat) // Within bounds
					{
						m->notes[k].push_back(*i); // Add this note to this measure.
						break; // Stop looking for a measure.
					}

					CurrentBeat += m->length;
				}
			}
		}
	}

public:
    double GetBeatspaceAt(const double T)
    {
        double Ret;
        if (!hitsound_sections.empty())
        {
            auto Current = hitsound_sections.begin();
            while (Current != hitsound_sections.end() && (Current->time > T || Current->is_inherited))
                ++Current;

            if (Current == hitsound_sections.end())
            {
                Current = hitsound_sections.begin();
                while (Current != hitsound_sections.end() && Current->is_inherited)
                    ++Current;

                if (Current == hitsound_sections.end())
                    throw OsuManiaLoaderException("No uninherited timing points were found!");
                else
                    Ret = Current->value;
            }
            else
                Ret = Current->value;
        }
        else
            throw OsuManiaLoaderException("No timing points found on this osu! file.");

        return Ret;
    }

    OsumaniaLoader(ChartGroup *song): slider_velocity(1.4), Version(0), last_sound_index(1), osu_sng(song)
    {
        ReadAModeTag = false;
        line_number = 0;
    }

    double GetSliderMultiplierAt(const double T)
    {
		const auto seclst = filter([&](const hitsound_section &H) { return H.is_inherited && H.time >= T; }, hitsound_sections);
        return seclst.size() ? seclst[0].value : 1;
    }

	bool read_general(std::string line)
	{
		const std::string Command = line.substr(0, line.find_first_of(':') + 1); // Lines are Information:<space>Content
		std::string content = line.substr(line.find_first_of(':') + 1);

		util::trim(content);

		if (Command == "AudioFilename:")
		{
			if (content == "virtual")
			{
				chart->has_no_audio_stream = true;
				return true;
			}
			else
			{
	#ifdef VERBOSE_DEBUG
				printf("Audio filename found: %s\n", content.c_str());
	#endif
				util::trim(content);
				osu_sng->song_filename = content;
				osu_sng->song_preview_source = content;
			}
		}
		else if (Command == "Mode:")
		{
			ReadAModeTag = true;
			if (content != "3") // It's not a osu!mania chart, so we can't use it.
				return false;
		}
		else if (Command == "sample_set:")
		{
			util::to_lower(content); util::trim(content);
			Defaultsampleset = content;
		}
		else if (Command == "PreviewTime:")
		{
			if (content != "-1")
			{
				if (osu_sng->preview_time == 0)
					osu_sng->preview_time = latof(content) / 1000;
			}
		}
		else if (Command == "SpecialStyle:")
		{
			if (content == "1")
				chart->transient->has_turntable = true;
		}

		return true;
	}


	void read_metadata(const std::string& line)
	{
		const auto Command = line.substr(0, line.find_first_of(':')); // Lines are Information:Content
		auto Content = line.substr(line.find_first_of(':') + 1, line.length() - line.find_first_of(":"));

	#ifdef VERBOSE_DEBUG
		printf("Command found: %s | Contents: %s\n", Command.c_str(), Content.c_str());
	#endif

		util::trim(Content);
		if (Command == "Title")
		{
			osu_sng->title = Content;
		}
		else if (Command == "Artist")
		{
			osu_sng->artist = Content;
		}
		else if (Command == "Version")
		{
			chart->meta->name = Content;
		}
		else if (Command == "TitleUnicode")
		{
			if (Content.length() > 1)
				osu_sng->title = Content;
		}
		else if (Command == "ArtistUnicode")
		{
			if (Content.length() > 1)
				osu_sng->artist = Content;
		}
		else if (Command == "Creator")
		{
			chart->meta->author = Content;
		}
	}


	void read_difficulty(const std::string& line)
	{
		const std::string Command = line.substr(0, line.find_first_of(':')); // Lines are Information:Content
		std::string Content = line.substr(line.find_first_of(':') + 1, line.length() - line.find_first_of(':'));
		util::trim(Content);

		// We ignore everything but the key count!
		if (Command == "CircleSize")
		{
			chart->channels = atoi(Content.c_str());
			if (chart->channels > MAX_CHANNELS) {
				throw OsuManiaLoaderException("osu! file with more lanes than the engine supports.");
			}
		}
		else if (Command == "SliderMultiplier")
		{
			slider_velocity = latof(Content) * 100;
		}
		else if (Command == "HPDrainRate")
		{
			info->hp = latof(Content);
		}
		else if (Command == "OverallDifficulty")
		{
			info->od = latof(Content);
		}
	}

	void read_events(const std::string& line)
	{
		auto Spl = util::token_split(line);

		if (Spl.size() > 1)
		{
			if (Spl[0] == "0" && Spl[1] == "0")
			{
				util::replace_all(Spl[2], "\"", "");
				osu_sng->background_filename = Spl[2];
				chart->transient->stage_file = Spl[2];
				EventsContent << line << std::endl;
			}
			else if (Spl[0] == "5" || Spl[0] == "Sample")
			{
				util::replace_all(Spl[3], "\"", "");

				if (sounds.find(Spl[3]) == sounds.end())
				{
					sounds[Spl[3]] = last_sound_index;
					last_sound_index++;
				}

				const double Time = latof(Spl[1]) / 1000.0;
				const int Evt = sounds[Spl[3]];
				AutoplaySound New;
				New.time = Time;
				New.sound = Evt;

				chart->transient->bgm_events.push_back(New);

				chart->duration = std::max(chart->duration, Time);
			} else
			{
				EventsContent << line << std::endl;
			}
		}
	}

	void read_timing(const std::string& line)
	{
		double Value;
		bool is_inherited;
		const auto Spl = util::token_split(line);

		if (Spl.size() < 2)
			return;

		if (Spl[6] == "1") // Non-inherited section
			is_inherited = false;
		else // An inherited section would be added to a velocity changes vector which would later alter speeds.
			is_inherited = true;

		int sampleset = -1;
		int custom = 0;
		double measure_len = 4;

		// We already set the value
			Value = -100 / latof(Spl[1]);
		if (!is_inherited)
			Value = 60000 / latof(Spl[1]);
		else

		if (Spl.size() > 2)
			measure_len = latof(Spl[2]);

		if (Spl.size() > 3)
			sampleset = atoi(Spl[3].c_str());

		if (Spl.size() > 4)
			custom = atoi(Spl[4].c_str());

		hitsound_section SecData;
		SecData.value = Value;
		SecData.measure_len = measure_len;
		SecData.time = latof(Spl[0].c_str()) / 1000.0;
		SecData.sampleset = sampleset;
		SecData.custom = custom;
		SecData.is_inherited = is_inherited;
		SecData.omit = false; // adjust if taiko bar omission is up

		if (!isnan(SecData.time)) // ._.
    		hitsound_sections.push_back(SecData);
	}
	/*
		This function is mostly correct; the main issue is that we'd have to know
		when custom = 0, that we should use 'per theme' default sounds.
		We don't have those, we don't use those, those are an osu!-ism
		so the sounds are not going to be 100% osu!-correct
		but they're going to be correct enough for virtual-mode charts to be accurate.

		sample_setAddition is an abomination on a VSRG - so it's only left in for informative purposes.
	*/
	std::string GetSampleFilename(SplitResult &split_line, int note_type, int hitsound, double Time)
	{
		// sample_setAddition is unused but left for self-documenting purposes.
		int sample_set = 0, sample_setAddition, customSample = 0;
		std::string SampleFilename;

		if (split_line.empty()) // Handle this properly, eventually.
			return "normal-hitnormal.wav";

		auto set_iter = lower_bound(hitsound_sections.begin(), hitsound_sections.end(), Time);
		auto spl_size = split_line.size();

		if (set_iter != hitsound_sections.begin()) --set_iter;

		if (note_type & NOTE_HOLD)
		{
			if (spl_size > 5 && split_line[5].length())
				return split_line[5];

			if (split_line.size() == 4)
			{
				sample_set = atoi(split_line[1].c_str());
				sample_setAddition = atoi(split_line[2].c_str());
				customSample = atoi(split_line[3].c_str());
			}
			else
			{
				sample_set = atoi(split_line[0].c_str());
				sample_setAddition = atoi(split_line[1].c_str());
				customSample = atoi(split_line[2].c_str());
			}

			/*
			if (SplCnt > 4)
				volume = atoi(Spl[4].c_str()); // ignored lol
			*/
		}
		else if (note_type & NOTE_NORMAL)
		{
			if (spl_size > 4 && split_line[4].length())
				return split_line[4];

			sample_set = atoi(split_line[0].c_str());
			if (split_line.size() > 1)
				sample_setAddition = atoi(split_line[1].c_str());
			if (split_line.size() > 2)
				customSample = atoi(split_line[2].c_str());

			/*
			if (SplCnt > 3)
				volume = atoi(Spl[3].c_str()); // ignored
				*/
		}
		else if (note_type & NOTE_SLIDER)
		{
			sample_set = sample_setAddition = customSample = 0;
		}

		std::string set_str;

		if (sample_set)
		{
			// translate sampleset int into samplesetGString
			set_str = get_sampleset_str(sample_set);
		}
		else
		{
			// get sampleset std::string from sampleset active at starttime

			if ((set_iter + 1) == hitsound_sections.begin() || hitsound_sections.begin() == hitsound_sections.end())
				set_str = Defaultsampleset;
			else
				set_str = get_sampleset_str(set_iter->sampleset);
		}

		if (!customSample && ! (set_iter + 1 == hitsound_sections.begin()) && hitsound_sections.begin() != hitsound_sections.end() )
			customSample = set_iter->custom;

		std::string custom_sample;

		if (customSample)
			custom_sample = int_to_str(customSample);

		std::string hitsound_type;

		if (hitsound)
		{
			switch (hitsound)
			{
			case 1:
				hitsound_type = "normal";
				break;
			case 2:
				hitsound_type = "whistle";
				break;
			case 4:
				hitsound_type = "finish";
				break;
			case 8:
				hitsound_type = "clap";
			default:
				break;
			}
		}
		else
			hitsound_type = "normal";

		if (customSample > 1)
		{
			SampleFilename = set_str + "-hit" + hitsound_type + custom_sample + ".wav";
		}
		else
			SampleFilename = set_str + "-hit" + hitsound_type + ".wav";

		return SampleFilename;
	}

	void read_objects(const std::string& line)
	{
		const auto object = util::token_split(line);

		const auto chan = get_track_from_position(latof(object[0]), chart->channels);
        NoteData note;

		SplitResult ObjecthitsoundData;

		/*
			A few of these "ifs" are just since v11 and v12 store hold endtimes in different locations.
			Or not include some information at all...
		*/
		int splitType = 5;
		if (object.size() == 7)
			splitType = 6;
		else if (object.size() == 5)
			splitType = 4;

		if (splitType != 4) // only 5 entries
			ObjecthitsoundData = util::token_split(object[splitType], ":");

		const double startTime = latof(object[2]) / 1000.0;
		const int note_type = atoi(object[3].c_str());

		if (note_type & NOTE_HOLD)
		{
			double endTime;
			if (splitType == 5 && !ObjecthitsoundData.empty())
				endTime = latof(ObjecthitsoundData[0]) / 1000.0;
			else if (splitType == 6)
				endTime = latof(object[5]) / 1000.0;
			else // what really? a hold that doesn't bother to tell us when it ends?
				endTime = 0;

			note.start = startTime;
			note.end_time = endTime;

			if (startTime > endTime)
			{ // Okay then, we'll transform this into a regular note..
				//if (DebugOsuLoader)
				//	Log::Printf("NoteLoaderOM: object at track %d has startTime > endTime (%f and %f)\n", Track, startTime, endTime);

				note.end_time = 0;
			}
		}
		else if (note_type & NOTE_NORMAL)
		{
			note.start = startTime;
		}
		else if (note_type & NOTE_SLIDER)
		{
			// 6=repeats 7=length
			const auto sliderRepeats = latof(object[6]);
			const auto sliderLength = latof(object[7]);

			const auto Multiplier = GetSliderMultiplierAt(startTime);

			const auto finalSize = sliderLength * sliderRepeats * Multiplier;
			const auto beatDuration = (finalSize / slider_velocity);
			const auto bpm = (60000.0 / GetBeatspaceAt(startTime));
			const auto len_seconds = beatDuration * spb(bpm);

			//if (0 > len_seconds && DebugOsuLoader)
			//	Log::LogPrintf("Line %d: o!m loader warning: object at track %d has startTime > endTime (%f and %f)\n", line_number, Track, startTime, len_seconds + startTime);

			note.start = startTime;
			note.end_time = len_seconds + startTime;
		}

		const int hitsound = atoi(object[4].c_str());

        if (const auto sample = GetSampleFilename(ObjecthitsoundData, note_type, hitsound, startTime); sample.length())
		{
			if (sounds.find(sample) == sounds.end())
			{
				sounds[sample] = last_sound_index;
				last_sound_index++;
			}

			note.sound = sounds[sample];
		}

		notes[chan].push_back(note);

		chart->duration = std::max(std::max(note.start, note.end_time) + 1, chart->duration);
	}

	void copy_timing_data()
	{
		for (const auto S : hitsound_sections)
		{
			if (S.is_inherited)
				chart->transient->scrolls.push_back(TimingSegment(S.time, S.value));
			else
				timing.push_back(TimingSegment(S.time, 60000 / S.value));
		}
	}

	void load_from_file(const std::filesystem::path& path)
    {
		std::ifstream filein (path, std::ios::in);

		std::regex versionfmt("osu file format v(\\d+)");

		if (!filein.is_open())
			throw OsuManiaLoaderException("Could not open file.");

	    info = std::make_shared<OsumaniaChartInfo>();
		chart = std::make_unique<Chart>();

        auto transient = std::make_shared<OsumaniaChartTransient>();
		chart->transient = transient;
		chart->transient->specialized_info = info;
        chart->meta.emplace();
		chart->meta->path = path;
		
		/*
			Just like BMS, osu!mania charts have timing data separated by files
			and a set is implied using folders.
		*/

		std::string line_str;

		getline(filein, line_str);
		int version = -1;

        // "search" was picked instead of "match" since a line can have a bunch of
		// junk before the version declaration
		if (std::smatch sm; regex_search(line_str.cbegin(), line_str.cend(), sm, versionfmt))
			version = atoi(sm[1].str().c_str());
		else
			throw OsuManiaLoaderException("Invalid .osu file.");

		// "osu file format v"
		if (version < 10) // why
			throw OsuManiaLoaderException(std::format("Unsupported osu! file version ({} < 10)", version).c_str());

		Version = version;

		OsuLoaderReadingState reading_mode = RNotKnown, reading_modeOld = RNotKnown;

		try
		{
			while (filein)
			{
				line_number++;
				getline(filein, line_str);
				util::replace_all(line_str, "\r", "");

				if (!line_str.length())
					continue;

				set_reading_mode(line_str, reading_mode);

				if (reading_mode != reading_modeOld || reading_mode == RNotKnown) // Skip this line since it changed modes, or it's not a valid section yet
				{
					if (reading_modeOld == RTiming)
						std::stable_sort(hitsound_sections.begin(), hitsound_sections.end());
					if (reading_modeOld == RGeneral)
						if (!ReadAModeTag)
							throw OsuManiaLoaderException("Not an osu!mania chart.");

					if (reading_modeOld == REvents)
					    transient->osb_sprites = EventsContent.str();

					reading_modeOld = reading_mode;
					continue;
				}

				switch (reading_mode)
				{
				case RGeneral:
					if (!read_general(line_str))  // don't load charts that we can't work with
						throw OsuManiaLoaderException("osu! file unusable.");
					break;
				case RMetadata: read_metadata(line_str); break;
				case RDifficulty: read_difficulty(line_str); break;
				case REvents: read_events(line_str); break;
				case RTiming: read_timing(line_str); break;
				case RHitobjects: read_objects(line_str); break;
				default: break;
				}
			}
			
			auto notecount = 0;
			for (int i = 0; i < chart->channels; i++) {
				notecount += notes[i].size();
			}

			if (notecount)
			{
				// Okay then, convert timing data into a measure-based format raindrop can use 
				// and calculate offset.
				measurize_from_timing_data();

				// Move scroll data into difficulty and publish normalized BPS.
				copy_timing_data();
				chart->transient->bps = bps_from_beatspace_timing(timing, chart->offset);

				// Then copy notes into these measures.
				push_notes_to_measures();

				// Copy all sounds we registered
				for (const auto& i : sounds)
					chart->transient->sound_list[i.second] = i.first;

				// Calculate level as NPS
				chart->level = chart->transient->get_scorable_note_count() / chart->duration;
				osu_sng->charts.push_back(std::move(chart));
			}
		}
		catch (std::exception &e)
		{
			// rethrow with line info
			throw OsuManiaLoaderException(std::format("Line {}: {}", line_number, e.what()).c_str());
		}
    }
};


void NoteLoaderOM::LoadObjectsFromFile(const std::filesystem::path& filename, ChartGroup *Out)
{
    OsumaniaLoader Info(Out);

	Info.load_from_file(filename);
}
