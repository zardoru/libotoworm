#include "rmath.h"

#include <constants.h>
#include <ChartGroup.h>

#include "text_and_file_util.h"

#include "utf8.h"

#include <format>
#include <fstream>
#include <note_loader_7k.h>

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
    std::string Result;
    int k = 0;
    int AwatingEOL = 0;
    const ptrdiff_t len = str.length() - 1;

    for (ptrdiff_t i = 0; i < len; i++)
    {
        if (AwatingEOL)
        {
            if (str[i] != '\n')
                continue;
            else
            {
                AwatingEOL = 0;
                continue;
            }
        }
        else
        {
            if (str[i] == '/' && str[i + 1] == '/')
            {
                AwatingEOL = true;
                continue;
            }
            else
            {
                Result.push_back(str.at(i));
                k++;
            }
        }
    }

    util::replace_all(Result, "[\\n\\r ]", "");
    return Result;
}

// See if Time is within a warp section. We use this after already having calculated the warp times.
bool is_time_within_warp(Chart* chart, const double time)
{

    for (const auto warp : chart->transient->warps)
    {
        const double LowerBound = warp.time;
        const double HigherBound = warp.time + warp.value;
        if (time >= LowerBound && time < HigherBound)
            return true;
    }

    return false;
}

void load_notes_sm(ChartGroup *out, Chart *chart, const TimingData& timing, std::vector<std::string> &measure_text)
{
    /* Hold data */
    const int Keys = chart->channels;
	double KeyStartTime[16] = {};
    if (!Keys)
        return;

    /* For each measure of the song */
    for (size_t i = 0; i < measure_text.size(); i++) /* i = current measure */
    {
        const ptrdiff_t MeasureSubdivisions = measure_text[i].length() / Keys;
        Measure Msr;

        if (measure_text[i].length())
        {
            /* For each fraction of the measure*/
            for (ptrdiff_t m = 0; m < MeasureSubdivisions; m++) /* m = current fraction */
            {
                const double Beat = i * 4.0 + m * 4.0 / static_cast<double>(MeasureSubdivisions); /* Current beat */
                const double StopsTime = chart->transient->stops.elapsed_stop_time_at_beat(Beat);
                const double Time = timing.integrate_beats_to_seconds(chart->offset, Beat, true) + StopsTime;
                const bool InWarpSection = is_time_within_warp(chart, Time);

                /* For every track of the fraction */
                for (ptrdiff_t k = 0; k < Keys; k++) /* k = current track */
                {
                    double KeyBeat[16];
                    NoteData Note;

                    if (InWarpSection)
                        Note.type = NK_FAKE;

                    switch (measure_text[i].at(0))
                    {
                    case '1': /* Taps */
                        Note.start = Time;
                        Msr.notes[k].push_back(Note);
                        break;
                    case '2': /* Holds */
                    case '4':
                        KeyStartTime[k] = Time;
                        KeyBeat[k] /*heh*/ = Beat;

                        break;
                    case '3': /* Hold releases */
                        Note.start = KeyStartTime[k];
                        Note.end_time = Time;

                        if (!is_time_within_warp(chart, KeyStartTime[k]))
                            Note.type = NK_NORMAL; // Un-fake it.
                        Msr.notes[k].push_back(Note);
                        break;
                    case 'F':
                        Note.start = Time;
                        Note.type = NK_FAKE;

                        Msr.notes[k].push_back(Note);
                    default:
                        break;
                    }

                    chart->duration = std::max(std::max(Note.start, Note.end_time), chart->duration);

                    if (measure_text[i].length() > 0)
                        measure_text[i].erase(0, 1);
                }
            }
        }

        chart->transient->measures.push_back(Msr);
    }
}

bool LoadTracksSM(ChartGroup *out, Chart *chart, const TimingData& timing, std::string line)
{
    std::string CommandContents = line.substr(line.find_first_of(":") + 1);

    /* Remove newlines and comments */
    CommandContents = remove_comments(CommandContents);

    const auto Mainline = util::token_split(CommandContents, ":");

    if (Mainline.size() < 6) // No, like HELL I'm loading this.
    {
        // The first time I found this it was because a ; was used as a separator instead of a :
        // Which means a rewrite is what probably should be done to fix that particular case.
        wprintf(L"Corrupt simfile (%zd entries instead of 6)", Mainline.size());
        return false;
    }

    /* What we'll work with */
    const std::string NoteString = Mainline[5];
    const int Keys = get_tracks_by_mode(Mainline[0]);

    if (!Keys)
        return false;

    chart->level = atoi(Mainline[3].c_str());
    chart->channels = Keys;
    chart->meta->name = Mainline[2] + "(" + Mainline[0] + ")";

    /* Now we should have our notes within NoteGString.
    We'll split them by measure using , as a separator.*/
    auto measure_text = util::token_split(NoteString);

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

void DoCommonSMCommands(std::string command, std::string CommandContents, ChartGroup* out)
{
    OnCommand(#TITLE)
    {
        if (utf8::is_valid(CommandContents.begin(), CommandContents.end()))
        {
#ifdef WIN32
            out->title = CommandContents;
#else
            out->title = CommandContents;
            try
            {
                std::vector<int> cp;
                utf8::utf8to16(CommandContents.begin(), CommandContents.end(), std::back_inserter(cp));
            }
            catch (utf8::not_enough_room &e)
            {
                out->title = locale::sjis_to_u8(CommandContents);
            }
#endif
        }
        else
            out->title = locale::sjis_to_u8(CommandContents);
    }

    _OnCommand(#SUBTITLE)
    {
        if (utf8::is_valid(CommandContents.begin(), CommandContents.end()))
        {
#ifdef WIN32
            out->subtitle = CommandContents;
#else
            out->subtitle = CommandContents;
            try
            {
                std::vector<int> cp;
                utf8::utf8to16(CommandContents.begin(), CommandContents.end(), std::back_inserter(cp));
            }
            catch (utf8::not_enough_room &e)
            {
                out->subtitle = locale::sjis_to_u8(CommandContents);
            }
#endif
        }
        else
            out->artist = locale::sjis_to_u8(CommandContents);
    }

    _OnCommand(#ARTIST)
    {
        if (utf8::is_valid(CommandContents.begin(), CommandContents.end()))
        {
#ifdef WIN32
            out->artist = CommandContents;
#else
            out->artist = CommandContents;
            try
            {
                std::vector<int> cp;
                utf8::utf8to16(CommandContents.begin(), CommandContents.end(), std::back_inserter(cp));
            }
            catch (utf8::not_enough_room &e)
            {
                out->artist = locale::sjis_to_u8(CommandContents);
            }
#endif
        }
        else
            out->artist = locale::sjis_to_u8(CommandContents);
    }

    _OnCommand(#BACKGROUND)
    {
        out->background_filename = CommandContents;
    }

    _OnCommand(#MUSIC)
    {
        if (utf8::is_valid(CommandContents.begin(), CommandContents.end()))
            out->song_filename = CommandContents;
        else
            out->song_filename = locale::sjis_to_u8(CommandContents);

        out->song_preview_source = out->song_filename;
    }

    _OnCommand(#SAMPLESTART)
    {
        out->preview_time = latof(CommandContents);
    }
}

// Convert SSC warps into Raindrop warps.
// Warning: This doesn't follow the beat-integration model dtinth programmed into Stepmania.
// It's not interpreted as "infinite bpm" but as literally jumping around.
// Should work for most cases. Haven't seen anyone put stops in the middle
// of their warps, fortunately.
TimingData reinterpret_warp_data(const Chart* chart, const TimingData& timing, const TimingData& Warps)
{
	TimingData Ret;
	for (auto warp : Warps)
	{
		// Since we use real song time instead of warped time to calculate warped time
		// no need for worry about having to align these.
		double Time = timing.integrate_beats_to_seconds(chart->offset, warp.time, true) +
			chart->transient->stops.elapsed_stop_time_at_beat(warp.time);

		double Value = warp.value * 60 / timing.section_value(warp.time);

		Ret.push_back(TimingSegment(Time, Value));
	}

	return Ret;
}

// DRY etc I'll see it later.
TimingData CalculateRaindropScrollData(const Chart* chart, const TimingData& timing, const TimingData& SCd)
{
	TimingData Ret;
	for (auto SC : SCd)
	{
		// No need to align these either, but since offset is applied at processing time for speed change
		// we need to set the offset at 0.
		double Time = timing.integrate_beats_to_seconds(0, SC.time, true) +
			chart->transient->stops.elapsed_stop_time_at_beat(SC.time);

		Ret.push_back(TimingSegment(Time, SC.value));
	}

	return Ret;
}

// Transform the time data from beats to seconds
VectorInterpolatedSpeedMultipliers copy_speed_data(Chart *chart, const TimingData& timing, const SpeedData& In)
{
	VectorInterpolatedSpeedMultipliers Ret;

	for (const auto scroll : In)
	{
		const double Time = timing.integrate_beats_to_seconds(0, scroll.time, true) +
			chart->transient->stops.elapsed_stop_time_at_beat(scroll.time);
		double TimeEnd;

		if (scroll.mode == StepmaniaSpeed::Beats)
		{
			TimeEnd = timing.integrate_beats_to_seconds(0, scroll.time + scroll.duration, true) +
				chart->transient->stops.elapsed_stop_time_at_beat(scroll.time + scroll.duration);
		}
		else
			TimeEnd = Time + scroll.duration;

		SpeedSection newscroll;
		newscroll.time = Time;
		newscroll.duration = TimeEnd - Time;
		newscroll.value = scroll.value;

		Ret.push_back(newscroll);
	}

	return Ret;
}

SpeedData parse_scrolls(std::string line)
{
    const auto ScrollLines = util::token_split(line);
    SpeedData Ret;

    for (auto segment : ScrollLines)
    {
        auto data = util::token_split(segment, "=");

        if (data.size() == 4)
        {
            StepmaniaSpeed newscroll;
            newscroll.time = latof(data[0]);
            newscroll.value = latof(data[1]);
            newscroll.duration = latof(data[2]);
            newscroll.mode = static_cast<StepmaniaSpeed::ModeType>(int(latof(data[3])));

            Ret.push_back(newscroll);
        }
    }

    return Ret;
}

void NoteLoaderSSC::LoadObjectsFromFile(const std::filesystem::path &filename, ChartGroup *out)
{
    std::ifstream filein(filename.string());

    TimingData BPMData;
    TimingData StopsData;
    TimingData WarpsData;
    TimingData ScrollData;
    TimingData chartTiming;
    SpeedData speedData;
    SpeedData diffSpeedData;
    double Offset = 0;

    std::unique_ptr<Chart> chart = nullptr;

    if (!filein.is_open())
        throw std::runtime_error(std::format("couldn't open {} for reading", filename.c_str()).c_str());

    std::string Banner;

    std::string line;
    while (filein)
    {
        getline(filein, line, ';');

        if (line.length() < 3)
            continue;

        std::string command;
        size_t iHash = line.find_first_of("#");
        size_t iColon = line.find_first_of(":");
        if (iHash != std::string::npos && iColon != std::string::npos)
            command = line.substr(iHash, iColon - iHash);
        else
            continue;

        util::replace_all(command, "\n", "");

        std::string CommandContents = line.substr(line.find_first_of(":") + 1);

        OnCommand(#NOTEDATA)
        {
            chart = std::make_unique<Chart>();
            chart->meta.emplace();
            chart->transient = std::make_shared<ChartTransient>();
            chartTiming.clear();
            diffSpeedData.clear(); // Clear this in particular since we're using a temp for diffs unlike the rest of the data.
        }

        DoCommonSMCommands(command, CommandContents, out);

        OnCommand(#OFFSET)
        {
            Offset = latof(CommandContents);
        }

        _OnCommand(#BPMS)
        {
            if (!chart)
                BPMData.load_list(line);
            else
                chartTiming.load_list(line);
        }

        _OnCommand(#STOPS)
        {
            if (!chart)
                StopsData.load_list(line);
            else
                chart->transient->stops.load_list(line);
        }

        _OnCommand(#BANNER)
        {
            Banner = CommandContents;
        }

        _OnCommand(#WARPS)
        {
            if (!chart)
                WarpsData.load_list(CommandContents);
            else
                chart->transient->warps.load_list(CommandContents);
        }

        _OnCommand(#SCROLLS)
        {
            if (!chart)
                ScrollData.load_list(CommandContents, true);
            else
                chart->transient->scrolls.load_list(CommandContents, true);
        }

        _OnCommand(#SPEEDS)
        {
            if (!chart)
                speedData = parse_scrolls(CommandContents);
            else
                diffSpeedData = parse_scrolls(CommandContents);
        }

        // Notedata only here

        if (!chart) continue;

        _OnCommand(#CREDIT)
        {
            chart->meta->author = CommandContents;
        }

        _OnCommand(#METER)
        {
            chart->level = atoi(CommandContents.c_str());
        }

        _OnCommand(#DIFFICULTY)
        {
            chart->meta->name = CommandContents;
        }

        _OnCommand(#CHARTSTYLE)
        {
            chart->meta->name += " " + CommandContents;
        }

        _OnCommand(#STEPSTYPE)
        {
            chart->channels = get_tracks_by_mode(CommandContents);
            if (chart->channels == 0)
                chart = nullptr;
        }

        _OnCommand(#NOTES)
        {
            if (!chartTiming.size())
                chartTiming = BPMData;

            if (!chart->transient->stops.size())
                chart->transient->stops = StopsData;

            if (!chart->transient->warps.size())
                chart->transient->warps = WarpsData;

            if (!chart->transient->scrolls.size())
                chart->transient->scrolls = ScrollData;

            if (!diffSpeedData.size())
                chart->transient->interpolated_speed_multipliers = copy_speed_data(chart.get(), chartTiming, speedData);
            else
                chart->transient->interpolated_speed_multipliers = copy_speed_data(chart.get(), chartTiming, diffSpeedData);

            chart->offset = -Offset;
            chart->duration = 0;
            chart->meta->path = filename;
            chart->transient->specialized_info = std::make_shared<StepmaniaChartInfo>();
            chart->transient->stage_file = Banner;

            chart->transient->warps = reinterpret_warp_data(chart.get(), chartTiming, chart->transient->warps);
            chart->transient->scrolls = CalculateRaindropScrollData(chart.get(), chartTiming, chart->transient->scrolls);
            chart->transient->bps = bps_from_beat_timing(chartTiming, chart->transient->stops, chart->offset);

            CommandContents = remove_comments(CommandContents);
            auto Measures = util::token_split(CommandContents);
            load_notes_sm(out, chart.get(), chartTiming, Measures);
            out->charts.push_back(std::move(chart));
        }
    }
}
// Convert all negative stops to warps.
void convert_stops_to_warps(Chart* chart, const TimingData& timing)
{
    TimingData tmpWarps;
    TimingData &Stops = chart->transient->stops;

    for (auto i = Stops.begin(); i != Stops.end();)
    {
        if (i->value < 0) // Warpahead.
        {
            tmpWarps.push_back(*i);
            i = Stops.erase(i);
            if (i == Stops.end()) break;
            else continue;
        }
        ++i;
    }

    for (auto warp : tmpWarps)
    {
        double Time = timing.integrate_beats_to_seconds(chart->offset, warp.time, true) +
            chart->transient->stops.elapsed_stop_time_at_beat(warp.time);

        TimingSegment New;
        New.time = Time;
        New.value = -warp.value;
        chart->transient->warps.push_back(New);
    }
}

// We need stops to already be warps by this point. Like with the other times, no need to account for the warps themselves.
void WarpifyTiming(Chart* chart, TimingData& timing)
{
    for (auto i = timing.begin(); i != timing.end(); ++i)
    {
        if (i->value < 0)
        {
            auto currentSection = i;

            // for all negative sections between i and the next positive section
            // add up their duration in seconds
            double totalWarpDuration = 0;
            while (currentSection->value < 0)
            {
                if (currentSection != timing.end())
                {
                    // add the duration of section, if there's one to determine it.
                    auto nextSection = currentSection + 1;
                    if (nextSection != timing.end())
                    {
						auto sectionDurationBeats = (nextSection->time - currentSection->time);
						auto sectionSecondsPerBeat = spb(abs(currentSection->value));
                        totalWarpDuration += sectionSecondsPerBeat * sectionDurationBeats;
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
                    currentSection->value = -currentSection->value;
                }
                else break;
                ++currentSection;
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
            const auto warpTime = timing.integrate_beats_to_seconds(chart->offset, i->time, true) +
				chart->transient->stops.elapsed_stop_time_at_beat(i->time);

            chart->transient->warps.push_back(TimingSegment(warpTime, totalWarpDuration * 2));
            // And now that we're done, there's no need to check the negative BPMs inbetween this one and the next positive BPM, so...
            i = currentSection;
            if (i == timing.end()) break;
        }
    }
}

void NoteLoaderSM::LoadObjectsFromFile(const std::filesystem::path &filename, ChartGroup *out)
{
	std::ifstream filein (filename, std::ios::in);

    TimingData BPMData;
    TimingData StopsData;
    double Offset = 0;

    auto chart = std::make_unique<Chart>();
    chart->meta.emplace();

    if (!filein.is_open())
        throw std::runtime_error(std::format("couldn't open {} for reading", filename.string()).c_str());

    std::string Banner;

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
        size_t iHash = line.find_first_of("#");
        size_t iColon = line.find_first_of(":");
        if (iHash != std::string::npos && iColon != std::string::npos)
            command = line.substr(iHash, iColon - iHash);
        else
            continue;

        util::replace_all(command, "\n", "");

        std::string CommandContents = line.substr(line.find_first_of(":") + 1);

        DoCommonSMCommands(command, CommandContents, out);

        OnCommand(#OFFSET)
        {
            Offset = latof(CommandContents);
        }

        OnCommand(#BPMS)
        {
            BPMData.load_list(line);
        }

        OnCommand(#STOPS)
        {
            StopsData.load_list(line);
        }

        /* Stops: TBD */

        OnCommand(#NOTES)
        {
            TimingData chartTiming = BPMData;
            chart->transient = std::make_shared<ChartTransient>();
            chart->transient->stops = StopsData;
            chart->offset = -Offset;
            chart->duration = 0;
            chart->meta->path = filename;
            chart->transient->specialized_info = std::make_shared<StepmaniaChartInfo>();
            chart->transient->stage_file = Banner;
            convert_stops_to_warps(chart.get(), chartTiming);
            WarpifyTiming(chart.get(), chartTiming);
            chart->transient->bps = bps_from_beat_timing(chartTiming, chart->transient->stops, chart->offset);

            if (LoadTracksSM(out, chart.get(), chartTiming, line))
            {
                out->charts.push_back(std::move(chart));
                chart = std::make_unique<Chart>();
                chart->meta.emplace();
            }
        }
    }
}
