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
    std::string get_subtitles(const std::string& s_line, std::unordered_set<std::string>& out)
    {
        std::string ret = s_line;
        if (!ret.empty() && subtitle_closers.find(ret.back()) != std::string_view::npos)
        {
            const auto subtitle_start = ret.find_first_of(subtitle_openers);
            if (subtitle_start != std::string::npos)
            {
                out.insert(ret.substr(subtitle_start));
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
            return max_channels + 1;
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
        FilenameListIndex sounds_;
        FilenameListIndex bitmaps_;

        FilenameUsedIndex used_sounds_;

        BpmListIndex bpms_;
        BpmListIndex stops_list_;
        BpmListIndex scrolls_list_;

        /*
            Used channels will be bound to this list.
            The first integer is the channel.
            Second integer is the actual measure
            Syntax in other words is
            Measures[Measure].events[Channel].Stuff
            */
        BMSMeasureList measures_;
        ChartGroup* song_;
        Chart* chart_;
        TimingData timing_;
        std::vector<double> measure_start_beat_;

        int lower_bound_{}, upper_bound_{};

        double start_time_[max_channels]{};

        NoteData* last_notes_[max_channels]{};

        std::set<int> ln_obj_;
        int side_b_offset_{};

        bool is_pms_;

        bool has_bmp_events_;
        bool has_turntable_; // Uses the turntable?

        void initialize_measure_start_times()
        {
            const int ei = (measures_.rbegin()->first) + 1;
            measure_start_beat_.clear();
            measure_start_beat_.reserve(ei);
            measure_start_beat_.push_back(0);
            for (int i = 1; i < ei; i++)
                measure_start_beat_.push_back(measure_start_beat_[i - 1] + measures_[i - 1].beat_duration * 4);
        }

        double beat_from_measure_fraction(const int measure, const double fraction)
        {
            return measure_start_beat_[measure] + fraction * measures_[measure].beat_duration * 4;
        }

        double get_time_for_measure_fraction(const int measure, const double fraction)
        {
            assert(chart_ != nullptr);
            const double beat = beat_from_measure_fraction(measure, fraction);
            const double time = timing_.integrate_beats_to_seconds(chart_->offset, beat) +
                chart_->transient->stops.elapsed_stop_time_at_beat(beat);

            return time;
        }

        void for_all_events_in_channel(
            const std::function<void(BMSEvent, int)>& fn,
            const bms::channel event_channel)
        {
            for (auto& measure : measures_)
            {
                for (const auto& [channel_value, events] : measure.second.events)
                {
                    if (channel_value.kind != event_channel)
                        continue;

                    for (const auto ev : events)
                        fn(ev, measure.first);
                }
            }
        }

        void process_bmp_events(
            std::vector<AutoplayBMP>& bmp_events,
            const bms::channel event_channel)
        {
            for_all_events_in_channel([&](BMSEvent ev, const int measure)
            {
                bmp_events.emplace_back(
                    get_time_for_measure_fraction(measure, ev.fraction),
                    ev.event
                );
            }, event_channel);
        }

        void process_bpm_events()
        {
            for_all_events_in_channel([&](const BMSEvent ev, const int measure)
            {
                const double bpm = b16toi(tob36(ev.event).c_str());
                const double beat = beat_from_measure_fraction(measure, ev.fraction);

                timing_.push_back(TimingSegment(beat, bpm));
            }, bms::channel::bpm);

            for_all_events_in_channel([&](const BMSEvent ev, const int measure)
            {
                double bpm;
                if (bpms_.contains(ev.event))
                    bpm = bpms_[ev.event];
                else
                    return;

                if (bpm == 0) return; // ignore 0 events

                const double beat = beat_from_measure_fraction(measure, ev.fraction);
                timing_.push_back(TimingSegment(beat, bpm));
            }, bms::channel::ex_bpm);

            // Make sure ExBPM events are in front using stable_sort.
            std::ranges::stable_sort(timing_);
        }

        void process_stop_events()
        {
            for_all_events_in_channel([&](const BMSEvent ev, const int measure)
            {
                const double beat = beat_from_measure_fraction(measure, ev.fraction);
                const double stop_time_beats = stops_list_[ev.event] / 48;
                const double section_value_stop = timing_.section_value(beat);
                const double spb_section = spb(section_value_stop);
                const double stop_duration = stop_time_beats * spb_section; // A value of 1 is... a 48th of a beat.

                chart_->transient->stops.push_back(TimingSegment(beat, stop_duration));
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

                int track = 0;

                if (!is_pms_)
                    track = translate_track_bme(info) - lower_bound_;
                else
                    track = translate_track_pms(info);

                //if (!(track >= 0 && track < max_channels)) util::DebugBreak();
                if (track >= chart_->channels || track < 0) continue;

                for (const auto ev : events)
                    action(ev, track);
            }
        }

        void process_measure_side(BMSMeasureList::iterator& i, const int track_offset,
                                  const bms::play_side side, Measure& measure,
                                  const int min_relative = 0)
        {
            // Standard events
            for_note_channels_in_measure([&](const BMSEvent ev, int track)
            {
                track += track_offset;

                if (track >= chart_->channels) return; // UNUSABLE event

                const double time = get_time_for_measure_fraction(i->first, ev.fraction);

                chart_->duration = std::max(static_cast<double>(chart_->duration), time);

                auto make_note = [&]()
                {
                    NoteData note;

                    note.start = time;
                    note.sound = ev.event;
                    used_sounds_[ev.event] = true;


                    measure.notes[track].push_back(note);
                    /*
                        For future reference:
                        no, we can't get rid of last_notes_[x]
                        otherwise you'd be bounding longnotes to
                        only one measure.
                    */
                    last_notes_[track] = &measure.notes[track].back();
                };

                // is this event on the lnobj set?
                if (ln_obj_.find(ev.event) == ln_obj_.end())
                    make_note();
                else // we got that this terminates a ln obj
                {
                    if (last_notes_[track])
                    {
                        last_notes_[track]->end_time = time;
                        last_notes_[track] = nullptr;
                    }
                    else
                        make_note();
                }
            }, side, bms::note_channel::visible, i, min_relative);

            // LN events
            for_note_channels_in_measure([&](const BMSEvent ev, int track)
            {
                track += track_offset;

                const double time = get_time_for_measure_fraction(i->first, ev.fraction);

                if (start_time_[track] == -1)
                    start_time_[track] = time;
                else
                {
                    NoteData note;

                    chart_->duration = std::max(static_cast<double>(chart_->duration), time);

                    note.start = start_time_[track];
                    note.end_time = time;

                    note.sound = ev.event;
                    used_sounds_[ev.event] = true;

                    measure.notes[track].push_back(note);

                    start_time_[track] = -1;
                }
            }, side, bms::note_channel::long_note, i, min_relative);


            for_note_channels_in_measure([&](const BMSEvent ev, int track)
            {
                const double time = get_time_for_measure_fraction(i->first, ev.fraction);
                NoteData note;
                note.start = time;
                note.sound = ev.event / 2; // Mine explosion value.
                // todo: finish mine mechanics
            }, side, bms::note_channel::mine, i, min_relative);

            for_note_channels_in_measure([&](const BMSEvent ev, const int track)
            {
                const double time = get_time_for_measure_fraction(i->first, ev.fraction);
                NoteData note;
                note.start = time;
                note.sound = ev.event; // Sound.
                note.type = NK_INVISIBLE;

                used_sounds_[ev.event] = true;
                measure.notes[track].push_back(note);
            }, side, bms::note_channel::invisible, i, min_relative);
        }

        void process_measure(BMSMeasureList::iterator& bms_measure)
        {
            Measure msr;

            msr.length = 4 * bms_measure->second.beat_duration;

            int base_offset = 0;

            // xxx: kind of a hack, but unlikely to break unless done on purpose
            if (!chart_->transient->has_turntable && (chart_->channels == 12 || chart_->channels == 16))
            {
                base_offset = 1;
            }

            // see both sides, p1 and p2
            if (!is_pms_) // or BME-type PMS
            {
                process_measure_side(
                    bms_measure,
                    base_offset,
                    bms::play_side::p1,
                    msr
                );
                process_measure_side(
                    bms_measure,
                    side_b_offset_ + base_offset,
                    bms::play_side::p2,
                    msr
                );
            }
            else
            {
                process_measure_side(
                    bms_measure,
                    0,
                    bms::play_side::p1,
                    msr
                );
                process_measure_side(
                    bms_measure,
                    5,
                    bms::play_side::p2,
                    msr,
                    1
                );
            }

            // insert it into the difficulty structure
            chart_->transient->measures.push_back(msr);

            for (uint8_t k = 0; k < max_channels; k++)
            {
                // Our old pointers are invalid by now since the Msr structures are going to go out of scope
                // Which means we must renew them, and that's better done here.
                auto iter = chart_->transient->measures.rbegin();
                while (iter != chart_->transient->measures.rend())
                {
                    if (!iter->notes[k].empty())
                    {
                        last_notes_[k] = &iter->notes[k].back();
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

                    used_sounds_[img_index] = true;
                    chart_->transient->bgm_events.emplace_back(time_secs, img_index);
                }
            }
        }

        void detect_channels(const int offset, int used_channels[max_channels_per_side],
                             const bms::play_side side)
        {
            // Actual autodetection
            auto mask_from_channel_at_measure = [&](const BMSMeasureList::iterator& i)
            {
                for (const auto& [channel, events] : i->second.events)
                {
                    if (events.empty())
                        continue;

                    const auto info = bms::playable_channel_from(channel);
                    if (!info.valid || info.side != side || info.kind == bms::note_channel::none)
                        continue;

                    int offs;

                    if (!is_pms_)
                    {
                        offs = translate_track_bme(info) + offset;
                        if (info.relative == relative_scratch_channel) // Turntable is going.
                            has_turntable_ = true;
                    }
                    else
                    {
                        offs = translate_track_pms(info) + offset;
                    }

                    if (offs < max_channels_per_side)
                        // A few BMSes use the foot pedal, so we need to not overflow the array.
                        used_channels[offs] = 1;
                }
            };

            for (auto i = measures_.begin(); i != measures_.end(); ++i)
            {
                mask_from_channel_at_measure(i);
            }
        }

        int get_channel_count()
        {
            int used_channels[max_channels_per_side] = {};
            int used_channels_b[max_channels_per_side] = {};

            if (is_pms_)
                return 9;

            lower_bound_ = -1;
            upper_bound_ = 0;

            /* Autodetect channel count based off channel information */
            detect_channels(0, used_channels, bms::play_side::p1);

            /* Find the last channel we've used's index */
            int first_index = -1;
            int last_index = 0;
            for (int i = 0; i < max_channels_per_side; i++)
            {
                if (used_channels[i] != 0)
                {
                    if (first_index == -1) // Lowest channel being used. Used for translation back to track 0.
                        first_index = i;

                    last_index = i;
                }
            }

            // Use that information to add the p2 side right next to the p1 side and have a continuous thing.
            detect_channels(0, used_channels_b, bms::play_side::p2);

            // Correct if second side starts at an offset different from zero.
            int side_b_index = -1;

            for (int i = 0; i < max_channels_per_side; i++)
                if (used_channels_b[i])
                {
                    side_b_index = i;
                    break;
                }

            // We found where it starts; append that starting point to the end of the left side.

            if (side_b_index >= 0)
            {
                for (int i = last_index + 1; i < max_channels_per_side; i++)
                    used_channels[i] |= used_channels_b[i - last_index - 1];
            }

            // if (FirstIndex >= 0 && sideBIndex >= 0); // This means, when working with the second side, add the offset to the current track.

            /* Find new boundaries for used channels. This means the first channel will be the Lower Bound. */
            for (int i = 0; i < max_channels_per_side; i++)
            {
                if (used_channels[i] != 0)
                {
                    if (lower_bound_ == -1) // Lowest channel being used. Used for translation back to track 0.
                        lower_bound_ = i;

                    upper_bound_ = i;
                }
            }

            // We pick the range of channels we're going to use.
            int range = upper_bound_ - lower_bound_ + 1;

            // This means, Side B offset starts from here.
            // If the last index was 7 for instance, and the first was 0, our side B offset would be 8, first channel of second side.
            // If the last index was 5 and the first was 0, side B offset would be 6.
            // While other cases would really not make much sense, they're theorically supported, anyway.
            side_b_offset_ = last_index + 1;

            // We modify it for completely unused key modes to not appear
            if (range < 4) // 1, 2, 3
                range = 6;

            if (range > 9 && range < 12) // 10, 11
                range = 12;

            if (range > 12 && range < 16) // 13, 14, 15
                range = 16;

            return range;
        }

        std::shared_ptr<BMSChartInfo> info_;

    public:
        BMSLoader(ChartGroup* song, std::unique_ptr<Chart>& diff, const bool ispms)
        {
            for (auto k = 0; k < max_channels; k++)
            {
                start_time_[k] = -1;
                last_notes_[k] = nullptr;
            }

            has_bmp_events_ = false;
            has_turntable_ = false;

            is_pms_ = ispms;
            chart_ = diff.get();
            song_ = song;
            chart_->has_no_audio_stream = true;

            info_ = std::make_shared<BMSChartInfo>();
            chart_->transient->specialized_info = info_;
        }

        void AddEvents(const bms::command& command)
        {
            if (command.event_channel.kind != bms::channel::meter)
            {
                auto& events = measures_[command.measure].events[command.event_channel];
                events.reserve(events.size() + command.events.size());

                if (command.event_channel.kind == bms::channel::bga_base ||
                    command.event_channel.kind == bms::channel::bga_layer ||
                    command.event_channel.kind == bms::channel::bga_layer_2 ||
                    command.event_channel.kind == bms::channel::bga_poor)
                    has_bmp_events_ = true;

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
                measures_[command.measure].beat_duration = latof(command.content);
            }
        }


        void process_scroll_events()
        {
            for_all_events_in_channel([&](const BMSEvent ev, const int measure)
            {
                const auto time = get_time_for_measure_fraction(measure, ev.fraction);
                if (scrolls_list_.find(ev.event) != scrolls_list_.end())
                    chart_->transient->scrolls.push_back(TimingSegment(time, scrolls_list_[ev.event]));
                else
                    chart_->transient->scrolls.push_back(TimingSegment(time, 1));
            }, bms::channel::scrolls);
        }

        void CompileBMS()
        {
            /* To be done. */
            auto& m = measures_;
            if (m.empty()) return; // what
            initialize_measure_start_times();

            process_bpm_events();
            process_stop_events();
            chart_->transient->bps = bps_from_beat_timing(timing_, chart_->transient->stops, chart_->offset);
            process_scroll_events();

            if (has_bmp_events_)
            {
                chart_->transient->bmp_events.emplace();
                auto& bmp = *chart_->transient->bmp_events;
                process_bmp_events(bmp.layer_base, bms::channel::bga_base);
                process_bmp_events(bmp.layer_miss, bms::channel::bga_poor);
                process_bmp_events(bmp.layer_upper, bms::channel::bga_layer);
                process_bmp_events(bmp.layer_upper2, bms::channel::bga_layer_2);
            }

            chart_->channels = get_channel_count();

            // Check turntable on 5/7 key singles or doubles key count
            if (chart_->channels == 6 || chart_->channels == 8 ||
                chart_->channels == 12 || chart_->channels == 16)
                chart_->transient->has_turntable = has_turntable_;

            for (auto i = m.begin(); i != m.end(); ++i)
                process_measure(i);

            std::ranges::sort(chart_->transient->bgm_events);

            /* Copy only used sounds to the sound list */
            for (auto& [idx, snd] : sounds_)
                if (used_sounds_.contains(idx) && used_sounds_[idx]) // This sound is used.
                    chart_->transient->sound_list[idx] = snd;

            if (has_bmp_events_)
                chart_->transient->bmp_events->bmp_list = bitmaps_;
        }

        void SetLNObject(const int lnobj)
        {
            ln_obj_.insert(lnobj);
        }

        void SetTotal(const double total)
        {
            info_->gauge_total = total;
        }

        void SetDefexRank(const double defex) const
        {
            info_->judge_rank = defex;
            info_->percentual_judgerank = true;
        }

        void SetJudgeRank(const double judgerank) const
        {
            info_->judge_rank = judgerank;
            info_->percentual_judgerank = false;
        }

        void SetSound(const int index, std::string command_contents)
        {
            sounds_[index] = std::move(command_contents);
        }

        void SetBMP(const int index, std::string command_contents)
        {
            bitmaps_[index] = std::move(command_contents);
        }

        void SetBPM(const int index, const double bpm)
        {
            bpms_[index] = bpm;
        }

        void SetInitialBPM(const double bpm)
        {
            timing_.push_back(TimingSegment(0, bpm));
        }

        void SetStop(const int index, const double stopval)
        {
            stops_list_[index] = stopval;
        }

        void SetScroll(const int index, const double scrollval)
        {
            scrolls_list_[index] = scrollval;
        }
    };

    std::string command_subcontents(const std::string& command, const std::string& line)
    {
        const uint32_t len = command.length();
        return line.substr(len);
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
            auto current = *i;
            util::to_lower(current);
            const char* s = current.c_str();

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

    void load_chart_from_stream(std::istream& filein, ChartGroup* out, const bool is_pms)
    {
        auto chart = std::make_unique<Chart>();

        chart->meta.emplace();
        chart->transient = std::make_shared<ChartTransient>();

        auto info = std::make_unique<BMSLoader>(out, chart, is_pms);

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

        std::unordered_set<std::string> subs; // Subtitle list
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
        auto parsed_tree = bms::tree::from_string(bms_text, &tree_error);
        if (!parsed_tree)
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
                    subs.insert(command.content);
                }
            },
            {"#title", false, [&](const bms::command& command) { out->title = command.content; }},
            {
                "#artist", false, [&](const bms::command& command)
                {
                    out->artist = command.content;
                    split_chart_author(out->artist, chart->meta->author);
                }
            },
            {
                "#bpm", false, [&](const bms::command& command)
                {
                    info->SetInitialBPM(latof(command.content));
                }
            },
            {
                "#music", false, [&](const bms::command& command)
                {
                    out->song_filename = command.content;
                    chart->has_no_audio_stream = false;
                    if (!out->song_preview_source.string().length())
                        out->song_preview_source = command.content;
                }
            },
            {"#offset", false, [&](const bms::command& command) { chart->offset = latof(command.content); }},
            {"#previewpoint", false, [&](const bms::command& command) { out->preview_time = latof(command.content); }},
            {"#previewtime", false, [&](const bms::command& command) { out->preview_time = latof(command.content); }},
            {"#defexrank", false, [&](const bms::command& command) { info->SetDefexRank(latof(command.content)); }},
            {"#stagefile", false, [&](const bms::command& command) { chart->transient->stage_file = command.content; }},
            {"#lnobj", false, [&](const bms::command& command) { info->SetLNObject(b36toi(command.content.c_str())); }},
            {
                "#difficulty", false,
                [&](const bms::command& command) { chart->meta->name = parse_difficulty_name(command.content); }
            },
            {"#backbmp", false, [&](const bms::command& command) { chart->transient->stage_file = command.content; }},
            {"#preview", false, [&](const bms::command& command) { out->song_preview_source = command.content; }},
            {"#total", false, [&](const bms::command& command) { info->SetTotal(latof(command.content)); }},
            {
                "#playlevel", false, [&](const bms::command& command)
                {
                    if (util::is_numeric(command.content.c_str()))
                        chart->level = std::stoll(command.content);
                }
            },
            {"#rank", false, [&](const bms::command& command) { info->SetJudgeRank(latof(command.content)); }},
            {"#maker", false, [&](const bms::command& command) { chart->meta->author = command.content; }},
            {
                "#wav", true, [&](const bms::command& command)
                {
                    const std::string index_str = command_subcontents("#WAV", command.name);
                    info->SetSound(b36toi(index_str.c_str()), command.content);
                }
            },
            {
                "#bmp", true, [&](const bms::command& command)
                {
                    const std::string index_str = command_subcontents("#BMP", command.name);
                    const int index = b36toi(index_str.c_str());
                    info->SetBMP(index, command.content);
                    if (index == 1)
                        out->background_filename = command.content;
                }
            },
            {
                "#bpm", true, [&](const bms::command& command)
                {
                    const std::string index_str = command_subcontents("#BPM", command.name);
                    info->SetBPM(b36toi(index_str.c_str()), latof(command.content));
                }
            },
            {
                "#stop", true, [&](const bms::command& command)
                {
                    const std::string index_str = command_subcontents("#STOP", command.name);
                    info->SetStop(b36toi(index_str.c_str()), latof(command.content));
                }
            },
            {
                "#exbpm", true, [&](const bms::command& command)
                {
                    const std::string index_str = command_subcontents("#EXBPM", command.name);
                    info->SetBPM(b36toi(index_str.c_str()), latof(command.content));
                }
            },
            {
                "#scroll", true, [&](const bms::command& command)
                {
                    const std::string index_str = command_subcontents("#SCROLL", command.name);
                    info->SetScroll(b36toi(index_str.c_str()), latof(command.content));
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
                info->AddEvents(command);
        };

        for (const auto& tree_command : parsed_tree->evaluate())
            dispatch_command(tree_command);

        /* When all's said and done, "compile" the bms. */
        info->CompileBMS();

        // First try to find a suiting subtitle
        std::string new_title = get_subtitles(out->title, subs);
        if (chart->meta->name.empty())
            chart->meta->name = get_chart_name_from_subtitles(subs);
        else
            get_chart_name_from_subtitles(subs); // has side-effects and removes difficulty name if applicable

        // If we've got a title that's usuable then why not use it.
        if (!new_title.empty())
            out->title = new_title.substr(0, new_title.find_last_not_of(' ') + 1);

        if (subs.size() > 1)
            out->subtitle = util::join(subs, " ");
        else if (subs.size() == 1)
            out->subtitle = *subs.begin();

        out->charts.push_back(std::move(chart));
    }
}
