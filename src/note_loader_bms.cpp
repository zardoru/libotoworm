#include <algorithm>
#include <filesystem>
#include <rmath.h>

#include <bms.h>
#include <ChartGroup.h>
#include "text_and_file_util.h"

#include <set>
#include <unordered_set>
#include <random>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string_view>

#include <utf8.h>
#include <map>
#include <functional>
#include <utility>
#include <cassert>
#include <cstring>

/*
	Source for implemented commands:
	http://hitkey.nekokan.dyndns.info/cmds.htm

	A huge lot of not-frequently-used commands are not going to be implemented.

	On 5K BMS, scratch is usually channel 16/26 for P1/P2
	17/27 for foot pedal- no exceptions
	keys 6 and 7 are 21/22 in bms
	keys 6 and 7 are 18/19, 28/29 in bme

	two additional extensions to BMS are planned for raindrop to introduce compatibility
	with SV changes:
	#SCROLLxx <value>
	#SPEEDxx <value> <duration>

	and are to be put under channel SV (base 36)

	Since most information is in japanese it's likely the implementation won't be perfect at the start.
*/
namespace NoteLoaderBMS
{
    constexpr int max_channels_per_side = 25; // 24k + 1
    using namespace otoworm;

    constexpr std::string_view subtitle_openers = "~-([<\"";
    constexpr std::string_view subtitle_closers = "])~>\"-";

    bool starts_with_case_insensitive(
        const std::string_view text,
        const size_t offset,
        const std::string_view prefix)
    {
        if (text.size() - offset < prefix.size())
            return false;

        for (size_t i = 0; i < prefix.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(text[offset + i])) != prefix[i])
                return false;
        }

        return true;
    }

    bool split_chart_author(std::string& artist, std::string& chart_author)
    {
        const auto skip_whitespace = [&artist](size_t& position)
        {
            while (position < artist.size() &&
                   std::isspace(static_cast<unsigned char>(artist[position])))
                ++position;
        };

        for (size_t start = 0; start < artist.size(); ++start)
        {
            size_t position = start;
            skip_whitespace(position);

            if (position < artist.size() && (artist[position] == '/' || artist[position] == '_'))
                ++position;

            skip_whitespace(position);

            size_t marker_length = 0;
            if (starts_with_case_insensitive(artist, position, "obj"))
                marker_length = 3;
            else if (starts_with_case_insensitive(artist, position, "note"))
                marker_length = 4;
            else
                continue;

            position += marker_length;
            if (position < artist.size() && artist[position] == '.')
                ++position;

            skip_whitespace(position);
            if (position < artist.size() && (artist[position] == ':' || artist[position] == '_'))
                ++position;

            skip_whitespace(position);
            chart_author = artist.substr(position);
            artist.erase(start);
            return true;
        }

        return false;
    }

    // Returns: Out: a std::vector with all the subtitles, std::string: The title without the subtitles.
    std::string get_subtitles(const std::string& SLine, std::unordered_set<std::string>& Out)
    {
        std::string ret = SLine;
        if (!ret.empty() && subtitle_closers.find(ret.back()) != std::string_view::npos)
        {
            const auto subtitle_start = ret.find_first_of(subtitle_openers);
            if (subtitle_start != std::string::npos)
            {
                Out.insert(ret.substr(subtitle_start));
                ret.erase(subtitle_start);
            }
        }

        util::trim(ret);
        return ret;
    }

    /* literally pasted from wikipedia */
    constexpr std::string tob36(long unsigned int value)
    {
        char buffer[14];
        size_t offset = sizeof(buffer);

        buffer[--offset] = '\0';
        do
        {
            constexpr char base36[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
            buffer[--offset] = base36[value % 36];
        }
        while (value /= 36);

        return {&buffer[offset]};
    }

    constexpr int relative_scratch_channel = 5;

    // End channels are usually xZ where X is the start (1, 2, 3 all the way to Z)
    // The first wav will always be WAV01, not WAV00 since 00 is a reserved value for "nothing"

    int translate_track_bme(const bms::playable_channel_info& channel)
    {
        switch (const int relative_channel = channel.relative)
        {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
            return relative_channel + 1;
        case 5:
            return 0;
        case 6: // foot pedal is ignored.
            return MAX_CHANNELS + 1;
        case 7:
            return 6;
        case 8:
            return 7;
        default: // Undefined
            return relative_channel + 1;
        }
    }

    int translate_track_pms(const bms::playable_channel_info& channel)
    {
        const int rel_track = channel.relative;

        if (channel.side == bms::play_side::p1 &&
            (channel.kind == bms::note_channel::visible || channel.kind == bms::note_channel::long_note))
        {
            switch (rel_track)
            {
            case 0:
            case 1:
            case 2:
            case 3:
            case 4:
                return rel_track;
            case 5:
                return 7;
            case 6:
                return 8;
            case 7:
                return 5;
            case 8:
                return 6;
            default: // Undefined
                return rel_track;
            }
        }
        else
        {
            switch (rel_track)
            {
            case 0:
            case 1:
            case 2:
            case 3:
                return rel_track;
            default: // Undefined
                return rel_track;
            }
        }
    }

    struct BMSEvent
    {
        int event;
        double fraction;

        bool operator<(const BMSEvent& rhs) const
        {
            return fraction < rhs.fraction;
        }
    };

    typedef std::vector<BMSEvent> BMSEventList;

    struct BMSMeasure
    {
        // first argument is channel, second is event list
        std::map<bms::channel_value, BMSEventList> events;
        double beat_duration;

        BMSMeasure()
        {
            beat_duration = 1; // Default duration
        }
    };

    typedef std::map<int, std::string> FilenameListIndex;
    typedef std::map<int, bool> FilenameUsedIndex;
    typedef std::map<int, double> BpmListIndex;
    typedef std::vector<NoteData> NoteVector;
    typedef std::map<int, BMSMeasure> BMSMeasureList;

    class BMSLoader
    {
        FilenameListIndex sounds;
        FilenameListIndex bitmaps;

        FilenameUsedIndex used_sounds;

        BpmListIndex bpms;
        BpmListIndex stops_list;
        BpmListIndex scrolls_list;

        /*
            Used channels will be bound to this list.
            The first integer is the channel.
            Second integer is the actual measure
            Syntax in other words is
            Measures[Measure].events[Channel].Stuff
            */
        BMSMeasureList measures;
        ChartGroup* Song;
        Chart* chart;
        TimingData timing;
        std::vector<double> measure_start_beat;

        int LowerBound{}, UpperBound{};

        double startTime[MAX_CHANNELS]{};

        NoteData* LastNotes[MAX_CHANNELS]{};

        std::set<int> LNObj;
        int SideBOffset{};

        bool is_pms;

        bool has_bmp_events;
        bool has_turntable; // Uses the turntable?

        void initialize_measure_start_times()
        {
            const int ei = (measures.rbegin()->first) + 1;
            measure_start_beat.clear();
            measure_start_beat.reserve(ei);
            measure_start_beat.push_back(0);
            for (int i = 1; i < ei; i++)
                measure_start_beat.push_back(measure_start_beat[i - 1] + measures[i - 1].beat_duration * 4);
        }

        double beat_from_measure_fraction(const int measure, const double Fraction)
        {
            return measure_start_beat[measure] + Fraction * measures[measure].beat_duration * 4;
        }

        double get_time_for_measure_fraction(const int Measure, const double Fraction)
        {
            assert(chart != nullptr);
            const double Beat = beat_from_measure_fraction(Measure, Fraction);
            const double Time = timing.integrate_beats_to_seconds(chart->offset, Beat) +
                chart->transient->stops.elapsed_stop_time_at_beat(Beat);

            return Time;
        }

        void for_all_events_in_channel(const std::function<void(BMSEvent, int)>& fn, const bms::channel Channel)
        {
            for (auto& Measure : measures)
            {
                for (const auto& [channel, events] : Measure.second.events)
                {
                    if (channel.kind != Channel)
                        continue;

                    for (const auto ev : events)
                        fn(ev, Measure.first);
                }
            }
        }

        void process_bmp_events(std::vector<AutoplayBMP>& BMPEvents, const bms::channel Channel)
        {
            for_all_events_in_channel([&](BMSEvent ev, const int Measure)
            {
                BMPEvents.emplace_back(
                    get_time_for_measure_fraction(Measure, ev.fraction),
                    ev.event
                );
            }, Channel);
        }

        void process_bpm_events()
        {
            for_all_events_in_channel([&](const BMSEvent ev, const int Measure)
            {
                const double BPM = b16toi(tob36(ev.event).c_str());
                const double Beat = beat_from_measure_fraction(Measure, ev.fraction);

                timing.push_back(TimingSegment(Beat, BPM));
            }, bms::channel::bpm);

            for_all_events_in_channel([&](const BMSEvent ev, const int Measure)
            {
                double BPM;
                if (bpms.contains(ev.event))
                    BPM = bpms[ev.event];
                else
                    return;

                if (BPM == 0) return; // ignore 0 events

                const double Beat = beat_from_measure_fraction(Measure, ev.fraction);
                timing.push_back(TimingSegment(Beat, BPM));
            }, bms::channel::ex_bpm);

            // Make sure ExBPM events are in front using stable_sort.
            std::ranges::stable_sort(timing);
        }

        void process_stop_events()
        {
            for_all_events_in_channel([&](const BMSEvent ev, const int Measure)
            {
                const double beat = beat_from_measure_fraction(Measure, ev.fraction);
                const double stop_time_beats = stops_list[ev.event] / 48;
                const double section_value_stop = timing.section_value(beat);
                const double spb_section = spb(section_value_stop);
                const double stop_duration = stop_time_beats * spb_section; // A value of 1 is... a 48th of a beat.

                chart->transient->stops.push_back(TimingSegment(beat, stop_duration));
            }, bms::channel::stops);
        }

        void for_note_channels_in_measure(
            const std::function<void(BMSEvent, int)>& action,
            const bms::play_side side,
            const bms::note_channel kind,
            const BMSMeasureList::iterator& bms_measure,
            const int min_relative = 0) const
        {
            for (const auto& [channel, events] : bms_measure->second.events)
            {
                const auto info = bms::playable_channel_from(channel);
                if (!info.valid || info.side != side || info.kind != kind || info.relative < min_relative)
                    continue;

                int Track = 0;

                if (!is_pms)
                    Track = translate_track_bme(info) - LowerBound;
                else
                    Track = translate_track_pms(info);

                //if (!(Track >= 0 && Track < MAX_CHANNELS)) util::DebugBreak();
                if (Track >= chart->channels || Track < 0) continue;

                for (const auto ev : events)
                    action(ev, Track);
            }
        }

        void process_measure_side(BMSMeasureList::iterator& i, const int TrackOffset,
                                  const bms::play_side side, Measure& measure,
                                  const int min_relative = 0)
        {
            // Standard events
            for_note_channels_in_measure([&](const BMSEvent ev, int Track)
            {
                Track += TrackOffset;

                if (Track >= chart->channels) return; // UNUSABLE event

                const double Time = get_time_for_measure_fraction(i->first, ev.fraction);

                chart->duration = std::max(static_cast<double>(chart->duration), Time);

                auto make_note = [&]()
                {
                    NoteData Note;

                    Note.start = Time;
                    Note.sound = ev.event;
                    used_sounds[ev.event] = true;


                    measure.notes[Track].push_back(Note);
                    /*
                        For future reference:
                        no, we can't get rid of LastNotes[x]
                        otherwise you'd be bounding longnotes to
                        only one measure.
                    */
                    LastNotes[Track] = &measure.notes[Track].back();
                };

                // is this event on the lnobj set?
                if (LNObj.find(ev.event) == LNObj.end())
                    make_note();
                else // we got that this terminates a ln obj
                {
                    if (LastNotes[Track])
                    {
                        LastNotes[Track]->end_time = Time;
                        LastNotes[Track] = nullptr;
                    }
                    else
                        make_note();
                }
            }, side, bms::note_channel::visible, i, min_relative);

            // LN events
            for_note_channels_in_measure([&](const BMSEvent ev, int Track)
            {
                Track += TrackOffset;

                const double Time = get_time_for_measure_fraction(i->first, ev.fraction);

                if (startTime[Track] == -1)
                    startTime[Track] = Time;
                else
                {
                    NoteData Note;

                    chart->duration = std::max(static_cast<double>(chart->duration), Time);

                    Note.start = startTime[Track];
                    Note.end_time = Time;

                    Note.sound = ev.event;
                    used_sounds[ev.event] = true;

                    measure.notes[Track].push_back(Note);

                    startTime[Track] = -1;
                }
            }, side, bms::note_channel::long_note, i, min_relative);


            for_note_channels_in_measure([&](const BMSEvent ev, int Track)
            {
                const double Time = get_time_for_measure_fraction(i->first, ev.fraction);
                NoteData Note;
                Note.start = Time;
                Note.sound = ev.event / 2; // Mine explosion value.
                // todo: finish mine mechanics
            }, side, bms::note_channel::mine, i, min_relative);

            for_note_channels_in_measure([&](const BMSEvent ev, const int Track)
            {
                const double Time = get_time_for_measure_fraction(i->first, ev.fraction);
                NoteData note;
                note.start = Time;
                note.sound = ev.event; // Sound.
                note.type = NK_INVISIBLE;

                used_sounds[ev.event] = true;
                measure.notes[Track].push_back(note);
            }, side, bms::note_channel::invisible, i, min_relative);
        }

        void process_measure(BMSMeasureList::iterator& bms_measure)
        {
            Measure Msr;

            Msr.length = 4 * bms_measure->second.beat_duration;

            int baseOffset = 0;

            // xxx: kind of a hack, but unlikely to break unless done on purpose
            if (!chart->transient->has_turntable && (chart->channels == 12 || chart->channels == 16))
            {
                baseOffset = 1;
            }

            // see both sides, p1 and p2
            if (!is_pms) // or BME-type PMS
            {
                process_measure_side(
                    bms_measure,
                    baseOffset,
                    bms::play_side::p1,
                    Msr
                );
                process_measure_side(
                    bms_measure,
                    SideBOffset + baseOffset,
                    bms::play_side::p2,
                    Msr
                );
            }
            else
            {
                process_measure_side(
                    bms_measure,
                    0,
                    bms::play_side::p1,
                    Msr
                );
                process_measure_side(
                    bms_measure,
                    5,
                    bms::play_side::p2,
                    Msr,
                    1
                );
            }

            // insert it into the difficulty structure
            chart->transient->measures.push_back(Msr);

            for (uint8_t k = 0; k < MAX_CHANNELS; k++)
            {
                // Our old pointers are invalid by now since the Msr structures are going to go out of scope
                // Which means we must renew them, and that's better done here.
                auto iter = chart->transient->measures.rbegin();
                while (iter != chart->transient->measures.rend())
                {
                    if (!iter->notes[k].empty())
                    {
                        LastNotes[k] = &iter->notes[k].back();
                        break;
                    }

                    ++iter;
                }
            }


            const auto bgm_events = bms_measure->second.events.find({bms::channel::bgm, 1});
            if (bgm_events != bms_measure->second.events.end() && !bgm_events->second.empty()) // There are some BGM events?
            {
                for (auto [img_index, time] : bgm_events->second)
                {
                    double time_secs = get_time_for_measure_fraction(bms_measure->first, time);

                    used_sounds[img_index] = true;
                    chart->transient->bgm_events.emplace_back(time_secs, img_index);
                }
            }
        }

        void detect_channels(const int offset, int usedChannels[max_channels_per_side],
                             const bms::play_side side)
        {
            // Actual autodetection
            auto maskFromChannelAtMeasure = [&](const BMSMeasureList::iterator& i)
            {
                for (const auto& [channel, events] : i->second.events)
                {
                    if (events.empty())
                        continue;

                    const auto info = bms::playable_channel_from(channel);
                    if (!info.valid || info.side != side || info.kind == bms::note_channel::none)
                        continue;

                    int offs;

                    if (!is_pms)
                    {
                        offs = translate_track_bme(info) + offset;
                        if (info.relative == relative_scratch_channel) // Turntable is going.
                            has_turntable = true;
                    }
                    else
                    {
                        offs = translate_track_pms(info) + offset;
                    }

                    if (offs < max_channels_per_side)
                        // A few BMSes use the foot pedal, so we need to not overflow the array.
                        usedChannels[offs] = 1;
                }
            };

            for (auto i = measures.begin(); i != measures.end(); ++i)
            {
                maskFromChannelAtMeasure(i);
            }
        }

        int get_channel_count()
        {
            int usedChannels[max_channels_per_side] = {};
            int usedChannelsB[max_channels_per_side] = {};

            if (is_pms)
                return 9;

            LowerBound = -1;
            UpperBound = 0;

            /* Autodetect channel count based off channel information */
            detect_channels(0, usedChannels, bms::play_side::p1);

            /* Find the last channel we've used's index */
            int FirstIndex = -1;
            int LastIndex = 0;
            for (int i = 0; i < max_channels_per_side; i++)
            {
                if (usedChannels[i] != 0)
                {
                    if (FirstIndex == -1) // Lowest channel being used. Used for translation back to track 0.
                        FirstIndex = i;

                    LastIndex = i;
                }
            }

            // Use that information to add the p2 side right next to the p1 side and have a continuous thing.
            detect_channels(0, usedChannelsB, bms::play_side::p2);

            // Correct if second side starts at an offset different from zero.
            int sideBIndex = -1;

            for (int i = 0; i < max_channels_per_side; i++)
                if (usedChannelsB[i])
                {
                    sideBIndex = i;
                    break;
                }

            // We found where it starts; append that starting point to the end of the left side.

            if (sideBIndex >= 0)
            {
                for (int i = LastIndex + 1; i < max_channels_per_side; i++)
                    usedChannels[i] |= usedChannelsB[i - LastIndex - 1];
            }

            // if (FirstIndex >= 0 && sideBIndex >= 0); // This means, when working with the second side, add the offset to the current track.

            /* Find new boundaries for used channels. This means the first channel will be the Lower Bound. */
            for (int i = 0; i < max_channels_per_side; i++)
            {
                if (usedChannels[i] != 0)
                {
                    if (LowerBound == -1) // Lowest channel being used. Used for translation back to track 0.
                        LowerBound = i;

                    UpperBound = i;
                }
            }

            // We pick the range of channels we're going to use.
            int Range = UpperBound - LowerBound + 1;

            // This means, Side B offset starts from here.
            // If the last index was 7 for instance, and the first was 0, our side B offset would be 8, first channel of second side.
            // If the last index was 5 and the first was 0, side B offset would be 6.
            // While other cases would really not make much sense, they're theorically supported, anyway.
            SideBOffset = LastIndex + 1;

            // We modify it for completely unused key modes to not appear
            if (Range < 4) // 1, 2, 3
                Range = 6;

            if (Range > 9 && Range < 12) // 10, 11
                Range = 12;

            if (Range > 12 && Range < 16) // 13, 14, 15
                Range = 16;

            return Range;
        }

        std::shared_ptr<BMSChartInfo> info;

    public:
        BMSLoader(ChartGroup* song, std::unique_ptr<Chart>& diff, const bool ispms)
        {
            for (auto k = 0; k < MAX_CHANNELS; k++)
            {
                startTime[k] = -1;
                LastNotes[k] = nullptr;
            }

            has_bmp_events = false;
            has_turntable = false;

            is_pms = ispms;
            chart = diff.get();
            Song = song;
            chart->has_no_audio_stream = true;

            info = std::make_shared<BMSChartInfo>();
            chart->transient->specialized_info = info;
        }

        void AddEvents(const bms::command& command)
        {
            if (command.event_channel.kind != bms::channel::meter)
            {
                auto& events = measures[command.measure].events[command.event_channel];
                events.reserve(events.size() + command.events.size());

                if (command.event_channel.kind == bms::channel::bga_base ||
                    command.event_channel.kind == bms::channel::bga_layer ||
                    command.event_channel.kind == bms::channel::bga_layer_2 ||
                    command.event_channel.kind == bms::channel::bga_poor)
                    has_bmp_events = true;

                for (size_t i = 0; i < command.events.size(); ++i)
                {
                    const int event = command.events[i];
                    const double fraction = static_cast<double>(i) / command.events.size();

                    if (event == 0) // Nothing to see here?
                        continue;

                    events.push_back({event, fraction});
                }
            }
            else // Channel 2 is a measure length event.
            {
                measures[command.measure].beat_duration = latof(command.content);
            }
        }


        void process_scroll_events()
        {
            for_all_events_in_channel([&](const BMSEvent ev, const int Measure)
            {
                const auto Time = get_time_for_measure_fraction(Measure, ev.fraction);
                if (scrolls_list.find(ev.event) != scrolls_list.end())
                    chart->transient->scrolls.push_back(TimingSegment(Time, scrolls_list[ev.event]));
                else
                    chart->transient->scrolls.push_back(TimingSegment(Time, 1));
            }, bms::channel::scrolls);
        }

        void CompileBMS()
        {
            /* To be done. */
            auto& m = measures;
            if (m.empty()) return; // what
            initialize_measure_start_times();

            process_bpm_events();
            process_stop_events();
            chart->transient->bps = bps_from_beat_timing(timing, chart->transient->stops, chart->offset);
            process_scroll_events();

            if (has_bmp_events)
            {
                chart->transient->bmp_events.emplace();
                auto& BMP = *chart->transient->bmp_events;
                process_bmp_events(BMP.layer_base, bms::channel::bga_base);
                process_bmp_events(BMP.layer_miss, bms::channel::bga_poor);
                process_bmp_events(BMP.layer_upper, bms::channel::bga_layer);
                process_bmp_events(BMP.layer_upper2, bms::channel::bga_layer_2);
            }

            chart->channels = get_channel_count();

            // Check turntable on 5/7 key singles or doubles key count
            if (chart->channels == 6 || chart->channels == 8 ||
                chart->channels == 12 || chart->channels == 16)
                chart->transient->has_turntable = has_turntable;

            for (auto i = m.begin(); i != m.end(); ++i)
                process_measure(i);

            std::ranges::sort(chart->transient->bgm_events);

            /* Copy only used sounds to the sound list */
            for (auto& [idx, snd] : sounds)
                if (used_sounds.contains(idx) && used_sounds[idx]) // This sound is used.
                    chart->transient->sound_list[idx] = snd;

            if (has_bmp_events)
                chart->transient->bmp_events->bmp_list = bitmaps;
        }

        void SetLNObject(const int lnobj)
        {
            LNObj.insert(lnobj);
        }

        void SetTotal(const double total)
        {
            info->gauge_total = total;
        }

        void SetDefexRank(const double defex) const
        {
            info->judge_rank = defex;
            info->percentual_judgerank = true;
        }

        void SetJudgeRank(const double judgerank) const
        {
            info->judge_rank = judgerank;
            info->percentual_judgerank = false;
        }

        void SetSound(const int index, std::string command_contents)
        {
            sounds[index] = std::move(command_contents);
        }

        void SetBMP(const int index, std::string command_contents)
        {
            bitmaps[index] = std::move(command_contents);
        }

        void SetBPM(const int index, const double bpm)
        {
            bpms[index] = bpm;
        }

        void SetInitialBPM(const double bpm)
        {
            timing.push_back(TimingSegment(0, bpm));
        }

        void SetStop(const int index, const double stopval)
        {
            stops_list[index] = stopval;
        }

        void SetScroll(const int index, const double scrollval)
        {
            scrolls_list[index] = scrollval;
        }
    };

    std::string command_subcontents(const std::string& Command, const std::string& Line)
    {
        const uint32_t len = Command.length();
        return Line.substr(len);
    }

    bool is_utf8_encoded(const std::string_view data)
    {
        return utf8::starts_with_bom(data.begin(), data.end()) ||
            utf8::is_valid(data.begin(), data.end());
    }

    std::string normalize_bms_line(std::string line, const bool source_is_utf8)
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (!source_is_utf8)
            return locale::sjis_to_u8(std::move(line));

        if (utf8::is_valid(line.begin(), line.end()))
            return line;

        std::string normalized;
        utf8::replace_invalid(line.begin(), line.end(), std::back_inserter(normalized));
        return normalized;
    }

    std::string get_chart_name_from_subtitles(std::unordered_set<std::string>& subtitles)
    {
        std::string candidate;
        for (auto i = subtitles.begin();
             i != subtitles.end();
             ++i)
        {
            auto Current = *i;
            util::to_lower(Current);
            const char* s = Current.c_str();

            if (strstr(s, "normal") || strstr(s, "5key") || strstr(s, "7key") || strstr(s, "10key"))
            {
                candidate = "Normal";
            }
            if (strstr(s, "another"))
            {
                candidate = "Another";
            }
            if (strstr(s, "ex"))
            {
                candidate = "EX";
            }
            if (strstr(s, "hyper") || strstr(s, "hard"))
            {
                candidate = "Hyper";
            }
            if (strstr(s, "light"))
            {
                candidate = "Light";
            }
            if (strstr(s, "beginner"))
            {
                candidate = "Beginner";
            }

            if (candidate.length())
            {
                subtitles.erase(i);
                return candidate;
            }
        }

        // Oh, we failed then..
        return "";
    }

    std::optional<bms::tree> ParseTreeFromString(
        const std::string& data,
        bms::parse_error* error)
    {
        return bms::tree::from_string(data, error);
    }

    std::optional<bms::tree> ParseTreeFromStream(
        std::istream& input,
        bms::parse_error* error)
    {
        std::ostringstream data;
        data << input.rdbuf();
        return bms::tree::from_string(data.str(), error);
    }

    void load_chart_from_stream(std::istream& filein, ChartGroup* Out, const bool IsPMS)
    {
        auto chart = std::make_unique<Chart>();

        chart->meta.emplace();
        chart->transient = std::make_shared<ChartTransient>();

        auto Info = std::make_unique<BMSLoader>(Out, chart, IsPMS);

        if (!filein)
            throw std::runtime_error("NoteLoaderBMS: input stream is not readable.");

        /*
            BMS files are separated always one file, one difficulty, so it'd make sense
            that every BMS 'set' might have different timing information per chart.
            While BMS specifies no 'set' support it's usually implied using folders.

            The default BME specifies is 8 channels when #PLAYER is unset, however
            the modern BMS standard specifies to ignore #PLAYER and try to figure it out
            from the amount of used channels.

            And that's what we're going to try to do.
            */

        std::unordered_set<std::string> Subs; // Subtitle list
        std::array<char, 1024> encoding_probe{};
        filein.read(encoding_probe.data(), encoding_probe.size());
        const auto encoding_probe_size = static_cast<size_t>(filein.gcount());
        filein.clear();
        filein.seekg(0);
        if (!filein)
            throw std::runtime_error("NoteLoaderBMS: input stream is not seekable.");

        // Sonorous UTF-8 extension
        const bool is_utf8 = is_utf8_encoded(
            std::string_view(encoding_probe.data(), encoding_probe_size));

        std::string bms_text;
        std::string line;
        while (std::getline(filein, line))
        {
            if (line.find('#') == std::string::npos)
                continue;

            bms_text += normalize_bms_line(std::move(line), is_utf8);
            bms_text.push_back('\n');
        }

        bms::parse_error tree_error{};
        auto ParsedTree = bms::tree::from_string(bms_text, &tree_error);
        if (!ParsedTree)
        {
            throw std::runtime_error("NoteLoaderBMS: failed to parse BMS control tree.");
        }

        struct command_dispatch
        {
            std::string_view command;
            bool prefix;
            std::function<void(const bms::command&)> action;
        };

        auto parse_difficulty_name = [](const std::string& value)
        {
            if (!util::is_numeric(value.c_str()))
                return value;

            switch (std::stoi(value))
            {
            case 1: return std::string("Beginner");
            case 2: return std::string("Normal");
            case 3: return std::string("Hard");
            case 4: return std::string("Another");
            case 5: return std::string("Another+");
            default: return std::string("???");
            }
        };

        std::function<void(const bms::command&)> dispatch_command;

        const std::vector<command_dispatch> command_dispatch_table = {
            {
                "#ext", false, [&](const bms::command& command)
                {
                    const auto ext_command = bms::parse_line(command.content, command.line_number);
                    if (ext_command)
                        dispatch_command(*ext_command);
                }
            },
            {"#genre", false, [&](const bms::command& command) { chart->transient->genre = command.content; }},
            {
                "#subtitle", false, [&](const bms::command& command)
                {
                    Subs.insert(command.content);
                }
            },
            {"#title", false, [&](const bms::command& command) { Out->title = command.content; }},
            {
                "#artist", false, [&](const bms::command& command)
                {
                    Out->artist = command.content;
                    split_chart_author(Out->artist, chart->meta->author);
                }
            },
            {
                "#bpm", false, [&](const bms::command& command)
                {
                    Info->SetInitialBPM(latof(command.content));
                }
            },
            {
                "#music", false, [&](const bms::command& command)
                {
                    Out->song_filename = command.content;
                    chart->has_no_audio_stream = false;
                    if (!Out->song_preview_source.string().length())
                        Out->song_preview_source = command.content;
                }
            },
            {"#offset", false, [&](const bms::command& command) { chart->offset = latof(command.content); }},
            {"#previewpoint", false, [&](const bms::command& command) { Out->preview_time = latof(command.content); }},
            {"#previewtime", false, [&](const bms::command& command) { Out->preview_time = latof(command.content); }},
            {"#defexrank", false, [&](const bms::command& command) { Info->SetDefexRank(latof(command.content)); }},
            {"#stagefile", false, [&](const bms::command& command) { chart->transient->stage_file = command.content; }},
            {"#lnobj", false, [&](const bms::command& command) { Info->SetLNObject(b36toi(command.content.c_str())); }},
            {
                "#difficulty", false,
                [&](const bms::command& command) { chart->meta->name = parse_difficulty_name(command.content); }
            },
            {"#backbmp", false, [&](const bms::command& command) { chart->transient->stage_file = command.content; }},
            {"#preview", false, [&](const bms::command& command) { Out->song_preview_source = command.content; }},
            {"#total", false, [&](const bms::command& command) { Info->SetTotal(latof(command.content)); }},
            {
                "#playlevel", false, [&](const bms::command& command)
                {
                    if (util::is_numeric(command.content.c_str()))
                        chart->level = std::stoll(command.content);
                }
            },
            {"#rank", false, [&](const bms::command& command) { Info->SetJudgeRank(latof(command.content)); }},
            {"#maker", false, [&](const bms::command& command) { chart->meta->author = command.content; }},
            {
                "#wav", true, [&](const bms::command& command)
                {
                    const std::string index_str = command_subcontents("#WAV", command.name);
                    Info->SetSound(b36toi(index_str.c_str()), command.content);
                }
            },
            {
                "#bmp", true, [&](const bms::command& command)
                {
                    const std::string index_str = command_subcontents("#BMP", command.name);
                    const int index = b36toi(index_str.c_str());
                    Info->SetBMP(index, command.content);
                    if (index == 1)
                        Out->background_filename = command.content;
                }
            },
            {
                "#bpm", true, [&](const bms::command& command)
                {
                    const std::string index_str = command_subcontents("#BPM", command.name);
                    Info->SetBPM(b36toi(index_str.c_str()), latof(command.content));
                }
            },
            {
                "#stop", true, [&](const bms::command& command)
                {
                    const std::string index_str = command_subcontents("#STOP", command.name);
                    Info->SetStop(b36toi(index_str.c_str()), latof(command.content));
                }
            },
            {
                "#exbpm", true, [&](const bms::command& command)
                {
                    const std::string index_str = command_subcontents("#EXBPM", command.name);
                    Info->SetBPM(b36toi(index_str.c_str()), latof(command.content));
                }
            },
            {
                "#scroll", true, [&](const bms::command& command)
                {
                    const std::string index_str = command_subcontents("#SCROLL", command.name);
                    Info->SetScroll(b36toi(index_str.c_str()), latof(command.content));
                }
            },
        };

        dispatch_command = [&](const bms::command& command)
        {
            for (const auto& entry : command_dispatch_table)
            {
                if ((!entry.prefix && command.name == entry.command) ||
                    (entry.prefix && command.name.starts_with(entry.command) &&
                        command.name.size() > entry.command.size()))
                {
                    entry.action(command);
                    return;
                }
            }

            if (command.type == bms::command_type::events)
                Info->AddEvents(command);
        };

        for (const auto& tree_command : ParsedTree->evaluate())
            dispatch_command(tree_command);

        /* When all's said and done, "compile" the bms. */
        Info->CompileBMS();

        // First try to find a suiting subtitle
        std::string NewTitle = get_subtitles(Out->title, Subs);
        if (chart->meta->name.empty())
            chart->meta->name = get_chart_name_from_subtitles(Subs);
        else
            get_chart_name_from_subtitles(Subs); // has side-effects and removes difficulty name if applicable

        // If we've got a title that's usuable then why not use it.
        if (!NewTitle.empty())
            Out->title = NewTitle.substr(0, NewTitle.find_last_not_of(' ') + 1);

        if (Subs.size() > 1)
            Out->subtitle = util::join(Subs, " ");
        else if (Subs.size() == 1)
            Out->subtitle = *Subs.begin();

        Out->charts.push_back(std::move(chart));
    }
}
