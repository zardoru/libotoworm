#include "rmath.h"

#include <ChartGroup.h>
#include <cmath>
#include <note_loader.h>

#include <format>
#include <fstream>
#include <math.h>
#include <regex>
#include <sstream>
#include "text_and_file_util.h"

/* osu!mania loader. credits to wanwan159, woc2006, Zorori and the author of AIBat for helping me understand this. */

typedef std::vector<std::string> SplitResult;

using namespace otoworm;

constexpr auto sampleset_normal = 1;
constexpr auto sampleset_soft = 2;
constexpr auto sampleset_drum = 3;

constexpr auto hitsound_normal = 0;
constexpr auto hitsound_whistle = 2;
constexpr auto hitsound_finish = 4;
constexpr auto hitsound_clap = 8;

constexpr auto note_slider = 2;
constexpr auto note_hold = 128;
constexpr auto note_normal = 1;

constexpr double line_remove_threshold = 0.0012;

// CfgVar DebugOsuLoader("OsuLoader", "Debug");

std::string get_sampleset_str(const int sampleset)
{
	switch (sampleset)
	{
	case sampleset_soft:
		return "soft";
	case sampleset_drum:
		return "drum";
	case sampleset_normal:
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

int get_track_from_position(const float position, const int channels)
{
    const float step = 512.f / channels;

    return static_cast<int>(position / step);
}

class OsuManiaLoaderException : public std::exception
{
private:
	std::string msg_;
public:
	OsuManiaLoaderException(const char * what) : exception(), msg_(what) {}
	const char* what() const noexcept { return msg_.c_str(); }
};

class OsumaniaLoader
{
    double slider_velocity_;
    int version_;
    int last_sound_index_;
    ChartGroup *osu_sng_;
    std::shared_ptr<OsumaniaChartInfo> info_;
    std::map <std::string, int> sounds_;
    std::vector<hitsound_section> hitsound_sections_;
    TimingData timing_;
    std::unique_ptr<Chart> chart_;
    std::string default_sampleset_;

	std::stringstream events_content_;

    bool read_a_mode_tag_;

    std::vector<NoteData> notes_[max_channels];
    int line_number_;

	void offsetize()
	{
		const auto first = find_first_measure();
		chart_->offset = first->time;

		for (auto & hitsound_section : hitsound_sections_)
		{
			hitsound_section.time -= chart_->offset;
		}
	}

	std::vector<hitsound_section>::iterator find_first_measure()
	{
		auto i = hitsound_sections_.begin();
		auto next = i + 1;
		while (i != hitsound_sections_.end() && i->is_inherited)
		{
			++i;
			next = i + 1;
			if (i != hitsound_sections_.end() && next != hitsound_sections_.end())
			{
				if (!i->is_inherited && !next->is_inherited)
				{
					while (i->time - next->time < 0.001 &&
							i != hitsound_sections_.end() &&
							next != hitsound_sections_.end())
					{
						i++;
						next = i + 1;
					}
				}
			}
		}

		return i != hitsound_sections_.end() ? i : hitsound_sections_.begin();
	}

	void measurize_from_timing_data()
	{
		offsetize();

		auto seclst = filter([](const hitsound_section& h)
		{
			return !h.is_inherited && !h.omit;
		}, hitsound_sections_);

		for (auto i = seclst.begin(); i != seclst.end();)
		{
			double section_duration_in_beats = 0;
			double section_duration_in_seconds_total = 0;

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
				section_duration_in_beats += subsection_duration;
				section_duration_in_seconds_total += (next->time - i->time);

				//if (DebugOsuLoader)
				//	Log::LogPrintf("\nMCALC: %f to %f (%f beats long at %f bpm)", next->time, i->time, bt, i->value);

				// the next section, and the one after that
				auto c_next = next;
				auto sk_next = next + 1;

				// add nivrad's bug
				do {
					// and that next line has a time distance of less than the threshold
					if (sk_next != seclst.end() && sk_next->time - c_next->time <= line_remove_threshold)
					{
						// ah the one after and the next are less than 1ms apart
						// get the displacement in beats of this section
						const auto seg = (sk_next->time - c_next->time) * bpm_to_bps(c_next->value);
						//if (DebugOsuLoader)
						//	Log::LogPrintf("\nMCALC: %f to %f (section is %g beats long)", sk_next->time, c_next->time, seg);
						section_duration_in_beats += seg; // we displace along here
						section_duration_in_seconds_total += (sk_next->time - c_next->time);
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
				const auto section_duration = bpm_to_bps(i->value) * (chart_->duration - i->time);
				section_duration_in_beats += section_duration;
				//if (DebugOsuLoader)
				//	Log::LogPrintf("\nMCALC: Last seg. %f to %f (%g beats long)", Diff->duration, i->time, seg);

			}

			auto total_measures_this_section = section_duration_in_beats / i->measure_len;
			const auto max_measures = section_duration_in_seconds_total / line_remove_threshold;

			/* this is nivrad's bug again, but for its actual intended purpose - capping measure lines */
			if (total_measures_this_section > max_measures)
			{
				i->measure_len = section_duration_in_beats;
				total_measures_this_section = 1;
			}

			//if (DebugOsuLoader)
			//	Log::LogPrintf("\nTotal Measures: %g, beats: %g, mlen: %f", TotalMeasuresThisSection, SectionDurationInBeats, i->measure_len);



			const auto whole = floor(total_measures_this_section);
			const auto fraction = total_measures_this_section - whole;

			// Add the measures.
			for (auto k = 0; k < whole; k++)
			{
				Measure msr;
				msr.length = i->measure_len;
				chart_->transient->measures.push_back(msr);
			}

			if (fraction > 0)
			{
				const auto dur_secs = fraction * i->measure_len * spb(i->value);
				if (dur_secs > line_remove_threshold) {
					Measure msr;
					msr.length = fraction * (i)->measure_len;
					chart_->transient->measures.push_back(msr);
				}
				else {
					if (!chart_->transient->measures.empty())
						chart_->transient->measures.back().length += fraction * i->measure_len;
				}
			}

			i = next;
		}
	}


	void push_notes_to_measures()
	{
		const TimingData& bps = chart_->transient->bps;

		for (int k = 0; k < max_channels; k++)
		{
			for (auto i = notes_[k].begin(); i != notes_[k].end(); ++i)
			{
				double beat;
				double current_beat = 0; // Lower bound of this measure

				if (!::isnan(i->start)) {
                    beat = quantize_beat(bps.integrate_to_time(i->start));
				} else {
				    if (isnan(i->end_time))
				        continue; // ???? why

                    i->type = NK_FAKE;
                    i->start = i->end_time;
                    i->end_time = 0;
				    beat = quantize_beat(bps.integrate_to_time(i->end_time));
				}

				if (beat < 0)
				{
					chart_->transient->measures[0].notes[k].push_back(*i);
					continue;
				}

				for (auto m = chart_->transient->measures.begin(); m != chart_->transient->measures.end(); ++m)
				{
					double next_beat = std::numeric_limits<double>::infinity();
					auto nextm = m + 1;

					if (nextm != chart_->transient->measures.end()) // Higher bound of this measure
						next_beat = current_beat + m->length;

					if (beat >= current_beat && beat < next_beat) // Within bounds
					{
						m->notes[k].push_back(*i); // Add this note to this measure.
						break; // Stop looking for a measure.
					}

					current_beat += m->length;
				}
			}
		}
	}

public:
    double GetBeatspaceAt(const double t)
    {
        double ret;
        if (!hitsound_sections_.empty())
        {
            auto current = hitsound_sections_.begin();
            while (current != hitsound_sections_.end() && (current->time > t || current->is_inherited))
                ++current;

            if (current == hitsound_sections_.end())
            {
                current = hitsound_sections_.begin();
                while (current != hitsound_sections_.end() && current->is_inherited)
                    ++current;

                if (current == hitsound_sections_.end())
                    throw OsuManiaLoaderException("No uninherited timing points were found!");
                else
                    ret = current->value;
            }
            else
                ret = current->value;
        }
        else
            throw OsuManiaLoaderException("No timing points found on this osu! file.");

        return ret;
    }

	OsumaniaLoader(ChartGroup *song): slider_velocity_(1.4), version_(0), last_sound_index_(1), osu_sng_(song)
    {
		read_a_mode_tag_ = false;
        line_number_ = 0;
    }

    double GetSliderMultiplierAt(const double t)
    {
		const auto seclst = filter([&](const hitsound_section &h) { return h.is_inherited && h.time >= t; }, hitsound_sections_);
        return seclst.size() ? seclst[0].value : 1;
    }

	bool read_general(std::string line)
	{
		const std::string command = line.substr(0, line.find_first_of(':') + 1); // Lines are Information:<space>Content
		std::string content = line.substr(line.find_first_of(':') + 1);

		util::trim(content);

		if (command == "AudioFilename:")
		{
			if (content == "virtual")
			{
				chart_->has_no_audio_stream = true;
				return true;
			}
			else
			{
	#ifdef VERBOSE_DEBUG
				printf("Audio filename found: %s\n", content.c_str());
	#endif
				util::trim(content);
				osu_sng_->song_filename = content;
				osu_sng_->song_preview_source = content;
			}
		}
		else if (command == "Mode:")
		{
			read_a_mode_tag_ = true;
			if (content != "3") // It's not a osu!mania chart, so we can't use it.
				return false;
		}
		else if (command == "sample_set:")
		{
			util::to_lower(content); util::trim(content);
			default_sampleset_ = content;
		}
		else if (command == "PreviewTime:")
		{
			if (content != "-1")
			{
				if (osu_sng_->preview_time == 0)
					osu_sng_->preview_time = latof(content) / 1000;
			}
		}
		else if (command == "SpecialStyle:")
		{
			if (content == "1")
				chart_->transient->has_turntable = true;
		}

		return true;
	}


	void read_metadata(const std::string& line)
	{
		const auto command = line.substr(0, line.find_first_of(':')); // Lines are Information:Content
		auto content = line.substr(line.find_first_of(':') + 1, line.length() - line.find_first_of(":"));

	#ifdef VERBOSE_DEBUG
		printf("Command found: %s | Contents: %s\n", Command.c_str(), Content.c_str());
	#endif

		util::trim(content);
		if (command == "Title")
		{
			osu_sng_->title = content;
		}
		else if (command == "Artist")
		{
			osu_sng_->artist = content;
		}
		else if (command == "Version")
		{
			chart_->meta->name = content;
		}
		else if (command == "TitleUnicode")
		{
			if (content.length() > 1)
				osu_sng_->title = content;
		}
		else if (command == "ArtistUnicode")
		{
			if (content.length() > 1)
				osu_sng_->artist = content;
		}
		else if (command == "Creator")
		{
			chart_->meta->author = content;
		}
	}


	void read_difficulty(const std::string& line)
	{
		const std::string command = line.substr(0, line.find_first_of(':')); // Lines are Information:Content
		std::string content = line.substr(line.find_first_of(':') + 1, line.length() - line.find_first_of(':'));
		util::trim(content);

		// We ignore everything but the key count!
		if (command == "CircleSize")
		{
			chart_->channels = atoi(content.c_str());
			if (chart_->channels > max_channels) {
				throw OsuManiaLoaderException("osu! file with more lanes than the engine supports.");
			}
		}
		else if (command == "SliderMultiplier")
		{
			slider_velocity_ = latof(content) * 100;
		}
		else if (command == "HPDrainRate")
		{
			info_->hp = latof(content);
		}
		else if (command == "OverallDifficulty")
		{
			info_->overall_difficulty = latof(content);
		}
	}

	void read_events(const std::string& line)
	{
		auto spl = util::token_split(line);

		if (spl.size() > 1)
		{
			if (spl[0] == "0" && spl[1] == "0")
			{
				util::replace_all(spl[2], "\"", "");
				osu_sng_->background_filename = spl[2];
				chart_->transient->stage_file = spl[2];
				events_content_ << line << std::endl;
			}
			else if (spl[0] == "5" || spl[0] == "Sample")
			{
				util::replace_all(spl[3], "\"", "");

				if (sounds_.find(spl[3]) == sounds_.end())
				{
					sounds_[spl[3]] = last_sound_index_;
					last_sound_index_++;
				}

				const double time = latof(spl[1]) / 1000.0;
				const int evt = sounds_[spl[3]];
				AutoplaySound event;
				event.time = time;
				event.sound = evt;

				chart_->transient->bgm_events.push_back(event);

				chart_->duration = std::max(chart_->duration, time);
			} else
			{
				events_content_ << line << std::endl;
			}
		}
	}

	void read_timing(const std::string& line)
	{
		double value;
		bool is_inherited;
		const auto spl = util::token_split(line);

		if (spl.size() < 2)
			return;

		if (spl[6] == "1") // Non-inherited section
			is_inherited = false;
		else // An inherited section would be added to a velocity changes vector which would later alter speeds.
			is_inherited = true;

		int sampleset = -1;
		int custom = 0;
		double measure_len = 4;

		// We already set the value
			value = -100 / latof(spl[1]);
		if (!is_inherited)
			value = 60000 / latof(spl[1]);
		else

		if (spl.size() > 2)
			measure_len = latof(spl[2]);

		if (spl.size() > 3)
			sampleset = atoi(spl[3].c_str());

		if (spl.size() > 4)
			custom = atoi(spl[4].c_str());

		hitsound_section sec_data;
		sec_data.value = value;
		sec_data.measure_len = measure_len;
		sec_data.time = latof(spl[0].c_str()) / 1000.0;
		sec_data.sampleset = sampleset;
		sec_data.custom = custom;
		sec_data.is_inherited = is_inherited;
		sec_data.omit = false; // adjust if taiko bar omission is up

		if (!isnan(sec_data.time)) // ._.
		hitsound_sections_.push_back(sec_data);
	}
	/*
		This function is mostly correct; the main issue is that we'd have to know
		when custom = 0, that we should use 'per theme' default sounds.
		We don't have those, we don't use those, those are an osu!-ism
		so the sounds are not going to be 100% osu!-correct
		but they're going to be correct enough for virtual-mode charts to be accurate.

		sample_setAddition is an abomination on a VSRG - so it's only left in for informative purposes.
	*/
	std::string GetSampleFilename(SplitResult &split_line, int note_type, int hitsound, double time)
	{
		// sample_setAddition is unused but left for self-documenting purposes.
		int sample_set = 0, sample_set_addition, custom_sample_id = 0;
		std::string sample_filename;

		if (split_line.empty()) // Handle this properly, eventually.
			return "normal-hitnormal.wav";

		auto set_iter = lower_bound(hitsound_sections_.begin(), hitsound_sections_.end(), time);
		auto spl_size = split_line.size();

		if (set_iter != hitsound_sections_.begin()) --set_iter;

		if (note_type & note_hold)
		{
			if (spl_size > 5 && split_line[5].length())
				return split_line[5];

			if (split_line.size() == 4)
			{
				sample_set = atoi(split_line[1].c_str());
				sample_set_addition = atoi(split_line[2].c_str());
				custom_sample_id = atoi(split_line[3].c_str());
			}
			else
			{
				sample_set = atoi(split_line[0].c_str());
				sample_set_addition = atoi(split_line[1].c_str());
				custom_sample_id = atoi(split_line[2].c_str());
			}

			/*
			if (SplCnt > 4)
				volume = atoi(Spl[4].c_str()); // ignored lol
			*/
		}
		else if (note_type & note_normal)
		{
			if (spl_size > 4 && split_line[4].length())
				return split_line[4];

			sample_set = atoi(split_line[0].c_str());
			if (split_line.size() > 1)
				sample_set_addition = atoi(split_line[1].c_str());
			if (split_line.size() > 2)
				custom_sample_id = atoi(split_line[2].c_str());

			/*
			if (SplCnt > 3)
				volume = atoi(Spl[3].c_str()); // ignored
				*/
		}
		else if (note_type & note_slider)
		{
			sample_set = sample_set_addition = custom_sample_id = 0;
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

			if ((set_iter + 1) == hitsound_sections_.begin() || hitsound_sections_.begin() == hitsound_sections_.end())
				set_str = default_sampleset_;
			else
				set_str = get_sampleset_str(set_iter->sampleset);
		}

		if (!custom_sample_id && ! (set_iter + 1 == hitsound_sections_.begin()) && hitsound_sections_.begin() != hitsound_sections_.end() )
			custom_sample_id = set_iter->custom;

		std::string custom_sample;

		if (custom_sample_id)
			custom_sample = int_to_str(custom_sample_id);

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

		if (custom_sample_id > 1)
		{
			sample_filename = set_str + "-hit" + hitsound_type + custom_sample + ".wav";
		}
		else
			sample_filename = set_str + "-hit" + hitsound_type + ".wav";

		return sample_filename;
	}

	void read_objects(const std::string& line)
	{
		const auto object = util::token_split(line);

		const auto chan = get_track_from_position(latof(object[0]), chart_->channels);
        NoteData note;

		SplitResult objecthitsound_data;

		/*
			A few of these "ifs" are just since v11 and v12 store hold endtimes in different locations.
			Or not include some information at all...
		*/
		int split_type = 5;
		if (object.size() == 7)
			split_type = 6;
		else if (object.size() == 5)
			split_type = 4;

		if (split_type != 4) // only 5 entries
			objecthitsound_data = util::token_split(object[split_type], ":");

		const double start_time = latof(object[2]) / 1000.0;
		const int note_type = atoi(object[3].c_str());

		if (note_type & note_hold)
		{
			double end_time;
			if (split_type == 5 && !objecthitsound_data.empty())
				end_time = latof(objecthitsound_data[0]) / 1000.0;
			else if (split_type == 6)
				end_time = latof(object[5]) / 1000.0;
			else // what really? a hold that doesn't bother to tell us when it ends?
				end_time = 0;

			note.start = start_time;
			note.end_time = end_time;

			if (start_time > end_time)
			{ // Okay then, we'll transform this into a regular note..
				//if (DebugOsuLoader)
				//	Log::Printf("NoteLoaderOM: object at track %d has startTime > endTime (%f and %f)\n", Track, startTime, endTime);

				note.end_time = 0;
			}
		}
		else if (note_type & note_normal)
		{
			note.start = start_time;
		}
		else if (note_type & note_slider)
		{
			// 6=repeats 7=length
			const auto slider_repeats = latof(object[6]);
			const auto slider_length = latof(object[7]);

			const auto multiplier = GetSliderMultiplierAt(start_time);

			const auto final_size = slider_length * slider_repeats * multiplier;
			const auto beat_duration = (final_size / slider_velocity_);
			const auto bpm = (60000.0 / GetBeatspaceAt(start_time));
			const auto len_seconds = beat_duration * spb(bpm);

			//if (0 > len_seconds && DebugOsuLoader)
			//	Log::LogPrintf("Line %d: o!m loader warning: object at track %d has startTime > endTime (%f and %f)\n", line_number, Track, startTime, len_seconds + startTime);

			note.start = start_time;
			note.end_time = len_seconds + start_time;
		}

		const int hitsound = atoi(object[4].c_str());

        if (const auto sample = GetSampleFilename(objecthitsound_data, note_type, hitsound, start_time); sample.length())
		{
			if (sounds_.find(sample) == sounds_.end())
			{
				sounds_[sample] = last_sound_index_;
				last_sound_index_++;
			}

			note.sound = sounds_[sample];
		}

		notes_[chan].push_back(note);

		chart_->duration = std::max(std::max(note.start, note.end_time) + 1, chart_->duration);
	}

	void copy_timing_data()
	{
		for (const auto s : hitsound_sections_)
		{
			if (s.is_inherited)
				chart_->transient->scrolls.push_back(TimingSegment(s.time, s.value));
			else
				timing_.push_back(TimingSegment(s.time, 60000 / s.value));
		}
	}

	void load_from_stream(std::istream& filein)
    {
		std::regex versionfmt("osu file format v(\\d+)");

		if (!filein)
			throw OsuManiaLoaderException("Input stream is not readable.");

	    info_ = std::make_shared<OsumaniaChartInfo>();
		chart_ = std::make_unique<Chart>();

        auto transient = std::make_shared<OsumaniaChartTransient>();
		chart_->transient = transient;
		chart_->transient->specialized_info = info_;
        chart_->meta.emplace();

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

		version_ = version;

		OsuLoaderReadingState reading_mode = RNotKnown, reading_mode_old = RNotKnown;

		try
		{
			while (filein)
			{
				line_number_++;
				getline(filein, line_str);
				util::replace_all(line_str, "\r", "");

				if (!line_str.length())
					continue;

				set_reading_mode(line_str, reading_mode);

				if (reading_mode != reading_mode_old || reading_mode == RNotKnown) // Skip this line since it changed modes, or it's not a valid section yet
				{
					if (reading_mode_old == RTiming)
						std::stable_sort(hitsound_sections_.begin(), hitsound_sections_.end());
					if (reading_mode_old == RGeneral)
						if (!read_a_mode_tag_)
							throw OsuManiaLoaderException("Not an osu!mania chart.");

					if (reading_mode_old == REvents)
					    transient->osb_sprites = events_content_.str();

					reading_mode_old = reading_mode;
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
			for (int i = 0; i < chart_->channels; i++) {
				notecount += notes_[i].size();
			}

			if (notecount)
			{
				// Okay then, convert timing data into a measure-based format raindrop can use
				// and calculate offset.
				measurize_from_timing_data();

				// Move scroll data into difficulty and publish normalized BPS.
				copy_timing_data();
				chart_->transient->bps = bps_from_beatspace_timing(timing_, chart_->offset);

				// Then copy notes into these measures.
				push_notes_to_measures();

				// Copy all sounds we registered
				for (const auto& i : sounds_)
					chart_->transient->sound_list[i.second] = i.first;

				// Calculate level as NPS
				chart_->level = chart_->transient->get_scorable_note_count() / chart_->duration;
				osu_sng_->charts.push_back(std::move(chart_));
			}
		}
		catch (std::exception &e)
		{
			// rethrow with line info
			throw OsuManiaLoaderException(std::format("Line {}: {}", line_number_, e.what()).c_str());
		}
    }
};


void NoteLoaderOM::LoadObjectsFromStream(std::istream& input, ChartGroup *out)
{
    OsumaniaLoader info(out);

	info.load_from_stream(input);
}
