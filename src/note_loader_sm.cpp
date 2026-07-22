#include "rmath.h"

#include <constants.h>
#include <ChartGroup.h>

#include "text_and_file_util.h"

#include "utf8.h"

#include <format>
#include <fstream>
#include <note_loader.h>

/* Stepmania/SSC loader. Lacks delays and speeds for now. As well as keysounds. */

using namespace otoworm;

struct StepmaniaSpeed
{
    float time, duration, value;
    enum ModeType : int { Beats, Seconds } mode;
};

typedef std::vector<StepmaniaSpeed> SpeedData;

struct
{
    const char* name;
    uint8_t tracks;
} mode_tracks[] = {
    {"kb7-single", 7},
    {"dance-single", 4},
    {"dance-solo", 6},
    {"dance-couple", 8},
    {"dance-threepanel", 3},
    {"dance-double", 8},
    {"pump-single", 5},
    {"pump-double", 10},
    {"pump-halfdouble", 6}
};

int get_tracks_by_mode(std::string mode)
{
    for (const auto v : mode_tracks)
    {
        if (mode == v.name)
            return v.tracks;
    }

    // Log::LogPrintf("Unknown track mode: %s, skipping difficulty\n", mode.c_str());
    return 0;
}

#undef ModeType

std::string remove_comments(const std::string str)
{
    std::string result;
    int k = 0;
    int awating_eol = 0;
    const ptrdiff_t len = str.length() - 1;

    for (ptrdiff_t i = 0; i < len; i++)
    {
        if (awating_eol)
        {
            if (str[i] != '\n')
                continue;
            else
            {
                awating_eol = 0;
                continue;
            }
        }
        else
        {
            if (str[i] == '/' && str[i + 1] == '/')
            {
                awating_eol = true;
                continue;
            }
            else
            {
                result.push_back(str.at(i));
                k++;
            }
        }
    }

    util::replace_all(result, "[\\n\\r ]", "");
    return result;
}

// See if Time is within a warp section. We use this after already having calculated the warp times.
bool is_time_within_warp(Chart* chart, const double time)
{

    for (const auto warp : chart->transient->warps)
    {
        const double lower_bound = warp.time;
        const double higher_bound = warp.time + warp.value;
        if (time >= lower_bound && time < higher_bound)
            return true;
    }

    return false;
}

void load_notes_sm(ChartGroup *out, Chart *chart, const TimingData& timing, std::vector<std::string> &measure_text)
{
    /* Hold data */
    const int keys = chart->channels;
	double key_start_time[16] = {};
    if (!keys)
        return;

    /* For each measure of the song */
    for (size_t i = 0; i < measure_text.size(); i++) /* i = current measure */
    {
        const ptrdiff_t measure_subdivisions = measure_text[i].length() / keys;
        Measure msr;

        if (measure_text[i].length())
        {
            /* For each fraction of the measure*/
            for (ptrdiff_t m = 0; m < measure_subdivisions; m++) /* m = current fraction */
            {
                const double beat = i * 4.0 + m * 4.0 / static_cast<double>(measure_subdivisions); /* Current beat */
                const double stops_time = chart->transient->stops.elapsed_stop_time_at_beat(beat);
                const double time = timing.integrate_beats_to_seconds(chart->offset, beat, true) + stops_time;
                const bool in_warp_section = is_time_within_warp(chart, time);

                /* For every track of the fraction */
                for (ptrdiff_t k = 0; k < keys; k++) /* k = current track */
                {
                    double key_beat[16];
                    NoteData note;

                    if (in_warp_section)
                        note.type = NK_FAKE;

                    switch (measure_text[i].at(0))
                    {
                    case '1': /* Taps */
                        note.start = time;
                        msr.notes[k].push_back(note);
                        break;
                    case '2': /* Holds */
                    case '4':
                        key_start_time[k] = time;
                        key_beat[k] /*heh*/ = beat;

                        break;
                    case '3': /* Hold releases */
                        note.start = key_start_time[k];
                        note.end_time = time;

                        if (!is_time_within_warp(chart, key_start_time[k]))
                            note.type = NK_NORMAL; // Un-fake it.
                        msr.notes[k].push_back(note);
                        break;
                    case 'F':
                        note.start = time;
                        note.type = NK_FAKE;

                        msr.notes[k].push_back(note);
                    default:
                        break;
                    }

                    chart->duration = std::max(std::max(note.start, note.end_time), chart->duration);

                    if (measure_text[i].length() > 0)
                        measure_text[i].erase(0, 1);
                }
            }
        }

        chart->transient->measures.push_back(msr);
    }
}

bool LoadTracksSM(ChartGroup *out, Chart *chart, const TimingData& timing, std::string line)
{
    std::string command_contents = line.substr(line.find_first_of(":") + 1);

    /* Remove newlines and comments */
    command_contents = remove_comments(command_contents);

    const auto mainline = util::token_split(command_contents, ":");

    if (mainline.size() < 6) // No, like HELL I'm loading this.
    {
        // The first time I found this it was because a ; was used as a separator instead of a :
        // Which means a rewrite is what probably should be done to fix that particular case.
        wprintf(L"Corrupt simfile (%zd entries instead of 6)", mainline.size());
        return false;
    }

    /* What we'll work with */
    const std::string note_string = mainline[5];
    const int keys = get_tracks_by_mode(mainline[0]);

    if (!keys)
        return false;

    chart->level = atoi(mainline[3].c_str());
    chart->channels = keys;
    chart->meta->name = mainline[2] + "(" + mainline[0] + ")";

    /* Now we should have our notes within NoteGString.
    We'll split them by measure using , as a separator.*/
    auto measure_text = util::token_split(note_string);

    load_notes_sm(out, chart, timing, measure_text);

    /*
        Through here we can make a few assumptions.
        ->The measures are in order from start to finish
        ->Each measure has all potential playable tracks, even if that track is empty during that measure.
        ->Measures are internally ordered
        */
    return true;
}
#define OnCommand(x) if(command == #x || command == #x + std::string(":"))
#define _OnCommand(x) else if(command == #x || command == #x + std::string(":"))

void DoCommonSMCommands(std::string command, std::string command_contents, ChartGroup* out)
{
    OnCommand(#TITLE)
    {
        if (utf8::is_valid(command_contents.begin(), command_contents.end()))
        {
#ifdef WIN32
            out->title = CommandContents;
#else
            out->title = command_contents;
            try
            {
                std::vector<int> cp;
                utf8::utf8to16(command_contents.begin(), command_contents.end(), std::back_inserter(cp));
            }
            catch (utf8::not_enough_room &e)
            {
                out->title = locale::sjis_to_u8(command_contents);
            }
#endif
        }
        else
            out->title = locale::sjis_to_u8(command_contents);
    }

    _OnCommand(#SUBTITLE)
    {
        if (utf8::is_valid(command_contents.begin(), command_contents.end()))
        {
#ifdef WIN32
            out->subtitle = CommandContents;
#else
            out->subtitle = command_contents;
            try
            {
                std::vector<int> cp;
                utf8::utf8to16(command_contents.begin(), command_contents.end(), std::back_inserter(cp));
            }
            catch (utf8::not_enough_room &e)
            {
                out->subtitle = locale::sjis_to_u8(command_contents);
            }
#endif
        }
        else
            out->artist = locale::sjis_to_u8(command_contents);
    }

    _OnCommand(#ARTIST)
    {
        if (utf8::is_valid(command_contents.begin(), command_contents.end()))
        {
#ifdef WIN32
            out->artist = CommandContents;
#else
            out->artist = command_contents;
            try
            {
                std::vector<int> cp;
                utf8::utf8to16(command_contents.begin(), command_contents.end(), std::back_inserter(cp));
            }
            catch (utf8::not_enough_room &e)
            {
                out->artist = locale::sjis_to_u8(command_contents);
            }
#endif
        }
        else
            out->artist = locale::sjis_to_u8(command_contents);
    }

    _OnCommand(#BACKGROUND)
    {
        out->background_filename = command_contents;
    }

    _OnCommand(#MUSIC)
    {
        if (utf8::is_valid(command_contents.begin(), command_contents.end()))
            out->song_filename = command_contents;
        else
            out->song_filename = locale::sjis_to_u8(command_contents);

        out->song_preview_source = out->song_filename;
    }

    _OnCommand(#SAMPLESTART)
    {
        out->preview_time = latof(command_contents);
    }
}

// Convert SSC warps into Raindrop warps.
// Warning: This doesn't follow the beat-integration model dtinth programmed into Stepmania.
// It's not interpreted as "infinite bpm" but as literally jumping around.
// Should work for most cases. Haven't seen anyone put stops in the middle
// of their warps, fortunately.
TimingData reinterpret_warp_data(const Chart* chart, const TimingData& timing, const TimingData& warps)
{
	TimingData ret;
	for (auto warp : warps)
	{
		// Since we use real song time instead of warped time to calculate warped time
		// no need for worry about having to align these.
		double time = timing.integrate_beats_to_seconds(chart->offset, warp.time, true) +
			chart->transient->stops.elapsed_stop_time_at_beat(warp.time);

		double value = warp.value * 60 / timing.section_value(warp.time);

		ret.push_back(TimingSegment(time, value));
	}

	return ret;
}

// DRY etc I'll see it later.
TimingData CalculateRaindropScrollData(const Chart* chart, const TimingData& timing, const TimingData& s_cd)
{
	TimingData ret;
	for (auto sc : s_cd)
	{
		// No need to align these either, but since offset is applied at processing time for speed change
		// we need to set the offset at 0.
		double time = timing.integrate_beats_to_seconds(0, sc.time, true) +
			chart->transient->stops.elapsed_stop_time_at_beat(sc.time);

		ret.push_back(TimingSegment(time, sc.value));
	}

	return ret;
}

// Transform the time data from beats to seconds
VectorInterpolatedSpeedMultipliers copy_speed_data(Chart *chart, const TimingData& timing, const SpeedData& in)
{
	VectorInterpolatedSpeedMultipliers ret;

	for (const auto scroll : in)
	{
		const double time = timing.integrate_beats_to_seconds(0, scroll.time, true) +
			chart->transient->stops.elapsed_stop_time_at_beat(scroll.time);
		double time_end;

		if (scroll.mode == StepmaniaSpeed::Beats)
		{
			time_end = timing.integrate_beats_to_seconds(0, scroll.time + scroll.duration, true) +
				chart->transient->stops.elapsed_stop_time_at_beat(scroll.time + scroll.duration);
		}
		else
			time_end = time + scroll.duration;

		SpeedSection newscroll;
		newscroll.time = time;
		newscroll.duration = time_end - time;
		newscroll.value = scroll.value;

		ret.push_back(newscroll);
	}

	return ret;
}

SpeedData parse_scrolls(std::string line)
{
    const auto scroll_lines = util::token_split(line);
    SpeedData ret;

    for (auto segment : scroll_lines)
    {
        auto data = util::token_split(segment, "=");

        if (data.size() == 4)
        {
            StepmaniaSpeed newscroll;
            newscroll.time = latof(data[0]);
            newscroll.value = latof(data[1]);
            newscroll.duration = latof(data[2]);
            newscroll.mode = static_cast<StepmaniaSpeed::ModeType>(int(latof(data[3])));

            ret.push_back(newscroll);
        }
    }

    return ret;
}

void NoteLoaderSSC::LoadObjectsFromStream(std::istream& filein, ChartGroup *out)
{
    TimingData bpm_data;
    TimingData stops_data;
    TimingData warps_data;
    TimingData scroll_data;
    TimingData chart_timing;
    SpeedData speed_data;
    SpeedData diff_speed_data;
    double offset = 0;

    std::unique_ptr<Chart> chart = nullptr;

    if (!filein)
        throw std::runtime_error("input stream is not readable");

    std::string banner;

    std::string line;
    while (filein)
    {
        getline(filein, line, ';');

        if (line.length() < 3)
            continue;

        std::string command;
        size_t i_hash = line.find_first_of("#");
        size_t i_colon = line.find_first_of(":");
        if (i_hash != std::string::npos && i_colon != std::string::npos)
            command = line.substr(i_hash, i_colon - i_hash);
        else
            continue;

        util::replace_all(command, "\n", "");

        std::string command_contents = line.substr(line.find_first_of(":") + 1);

        OnCommand(#NOTEDATA)
        {
            chart = std::make_unique<Chart>();
            chart->meta.emplace();
            chart->transient = std::make_shared<ChartTransient>();
            chart_timing.clear();
            diff_speed_data.clear(); // Clear this in particular since we're using a temp for diffs unlike the rest of the data.
        }

        DoCommonSMCommands(command, command_contents, out);

        OnCommand(#OFFSET)
        {
            offset = latof(command_contents);
        }

        _OnCommand(#BPMS)
        {
            if (!chart)
                bpm_data.load_list(line);
            else
                chart_timing.load_list(line);
        }

        _OnCommand(#STOPS)
        {
            if (!chart)
                stops_data.load_list(line);
            else
                chart->transient->stops.load_list(line);
        }

        _OnCommand(#BANNER)
        {
            banner = command_contents;
        }

        _OnCommand(#WARPS)
        {
            if (!chart)
                warps_data.load_list(command_contents);
            else
                chart->transient->warps.load_list(command_contents);
        }

        _OnCommand(#SCROLLS)
        {
            if (!chart)
                scroll_data.load_list(command_contents, true);
            else
                chart->transient->scrolls.load_list(command_contents, true);
        }

        _OnCommand(#SPEEDS)
        {
            if (!chart)
                speed_data = parse_scrolls(command_contents);
            else
                diff_speed_data = parse_scrolls(command_contents);
        }

        // Notedata only here

        if (!chart) continue;

        _OnCommand(#CREDIT)
        {
            chart->meta->author = command_contents;
        }

        _OnCommand(#METER)
        {
            chart->level = atoi(command_contents.c_str());
        }

        _OnCommand(#DIFFICULTY)
        {
            chart->meta->name = command_contents;
        }

        _OnCommand(#CHARTSTYLE)
        {
            chart->meta->name += " " + command_contents;
        }

        _OnCommand(#STEPSTYPE)
        {
            chart->channels = get_tracks_by_mode(command_contents);
            if (chart->channels == 0)
                chart = nullptr;
        }

        _OnCommand(#NOTES)
        {
            if (!chart_timing.size())
                chart_timing = bpm_data;

            if (!chart->transient->stops.size())
                chart->transient->stops = stops_data;

            if (!chart->transient->warps.size())
                chart->transient->warps = warps_data;

            if (!chart->transient->scrolls.size())
                chart->transient->scrolls = scroll_data;

            if (!diff_speed_data.size())
                chart->transient->interpolated_speed_multipliers = copy_speed_data(chart.get(), chart_timing, speed_data);
            else
                chart->transient->interpolated_speed_multipliers = copy_speed_data(chart.get(), chart_timing, diff_speed_data);

            chart->offset = -offset;
            chart->duration = 0;
            chart->transient->specialized_info = std::make_shared<StepmaniaChartInfo>();
            chart->transient->stage_file = banner;

            chart->transient->warps = reinterpret_warp_data(chart.get(), chart_timing, chart->transient->warps);
            chart->transient->scrolls = CalculateRaindropScrollData(chart.get(), chart_timing, chart->transient->scrolls);
            chart->transient->bps = bps_from_beat_timing(chart_timing, chart->transient->stops, chart->offset);

            command_contents = remove_comments(command_contents);
            auto measures = util::token_split(command_contents);
            load_notes_sm(out, chart.get(), chart_timing, measures);
            out->charts.push_back(std::move(chart));
        }
    }
}
// Convert all negative stops to warps.
void convert_stops_to_warps(Chart* chart, const TimingData& timing)
{
    TimingData tmp_warps;
    TimingData &stops = chart->transient->stops;

    for (auto i = stops.begin(); i != stops.end();)
    {
        if (i->value < 0) // Warpahead.
        {
            tmp_warps.push_back(*i);
            i = stops.erase(i);
            if (i == stops.end()) break;
            else continue;
        }
        ++i;
    }

    for (auto warp : tmp_warps)
    {
        double time = timing.integrate_beats_to_seconds(chart->offset, warp.time, true) +
            chart->transient->stops.elapsed_stop_time_at_beat(warp.time);

        TimingSegment segment;
        segment.time = time;
        segment.value = -warp.value;
        chart->transient->warps.push_back(segment);
    }
}

// We need stops to already be warps by this point. Like with the other times, no need to account for the warps themselves.
void WarpifyTiming(Chart* chart, TimingData& timing)
{
    for (auto i = timing.begin(); i != timing.end(); ++i)
    {
        if (i->value < 0)
        {
            auto current_section = i;

            // for all negative sections between i and the next positive section
            // add up their duration in seconds
            double total_warp_duration = 0;
            while (current_section->value < 0)
            {
                if (current_section != timing.end())
                {
                    // add the duration of section, if there's one to determine it.
                    auto next_section = current_section + 1;
                    if (next_section != timing.end())
                    {
						auto section_duration_beats = (next_section->time - current_section->time);
						auto section_seconds_per_beat = spb(abs(current_section->value));
                        total_warp_duration += section_seconds_per_beat * section_duration_beats;
                    }

					/* 
					todo: 
					dtinth's interpretation for sm5 warps 
					infinite bpm, implies we have to split the warp when
					there's a stop.
					*/ 

					// negative bpm means backward scrolling to raindrop
					// therefore, to scroll forwards and warp over this section properly
					// we make it positive
                    current_section->value = -current_section->value;
                }
                else break;
                ++current_section;
            }
            
			// timing is in "no-warp" time
			// which means it's in "the time if warps were not considered"
			// or "chart time" used for display
			// the audio time, or time used for judgement
			// is usually behind the chart time.
			// notedata is in chart time.
			// therefore, tracknote time data is in audio time
			// and vertical position is in chart time.
			// chart time is calculated by adding up all the
			// time scrolled by bpm ignoring everything else
			// plus time scrolled by stops 
			// plus time scrolled by warps.
            const auto warp_time = timing.integrate_beats_to_seconds(chart->offset, i->time, true) +
				chart->transient->stops.elapsed_stop_time_at_beat(i->time);

            chart->transient->warps.push_back(TimingSegment(warp_time, total_warp_duration * 2));
            // And now that we're done, there's no need to check the negative BPMs inbetween this one and the next positive BPM, so...
            i = current_section;
            if (i == timing.end()) break;
        }
    }
}

void NoteLoaderSM::LoadObjectsFromStream(std::istream& filein, ChartGroup *out)
{
    TimingData bpm_data;
    TimingData stops_data;
    double offset = 0;

    auto chart = std::make_unique<Chart>();
    chart->meta.emplace();

    if (!filein)
        throw std::runtime_error("input stream is not readable");

    std::string banner;

    chart->offset = 0;
    chart->duration = 0;
    chart->transient = std::make_shared<ChartTransient>();

    std::string line;
    while (filein)
    {
        std::getline(filein, line, ';');

        if (line.length() < 3)
            continue;

        std::string command;
        size_t i_hash = line.find_first_of("#");
        size_t i_colon = line.find_first_of(":");
        if (i_hash != std::string::npos && i_colon != std::string::npos)
            command = line.substr(i_hash, i_colon - i_hash);
        else
            continue;

        util::replace_all(command, "\n", "");

        std::string command_contents = line.substr(line.find_first_of(":") + 1);

        DoCommonSMCommands(command, command_contents, out);

        OnCommand(#OFFSET)
        {
            offset = latof(command_contents);
        }

        OnCommand(#BPMS)
        {
            bpm_data.load_list(line);
        }

        OnCommand(#STOPS)
        {
            stops_data.load_list(line);
        }

        /* Stops: TBD */

        OnCommand(#NOTES)
        {
            TimingData chart_timing = bpm_data;
            chart->transient = std::make_shared<ChartTransient>();
            chart->transient->stops = stops_data;
            chart->offset = -offset;
            chart->duration = 0;
            chart->transient->specialized_info = std::make_shared<StepmaniaChartInfo>();
            chart->transient->stage_file = banner;
            convert_stops_to_warps(chart.get(), chart_timing);
            WarpifyTiming(chart.get(), chart_timing);
            chart->transient->bps = bps_from_beat_timing(chart_timing, chart->transient->stops, chart->offset);

            if (LoadTracksSM(out, chart.get(), chart_timing, line))
            {
                out->charts.push_back(std::move(chart));
                chart = std::make_unique<Chart>();
                chart->meta.emplace();
            }
        }
    }
}
