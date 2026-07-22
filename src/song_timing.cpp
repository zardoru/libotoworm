#include <algorithm>

#include "rmath.h"

#include "text_and_file_util.h"
#include <ChartGroup.h>

#include <numeric>
#include <cassert>
#include <cmath>
#include <ranges>


int lcm(const std::vector<int> &set)
{
    return std::accumulate(set.begin() + 1, set.end(), *set.begin(), lcm<int>);
}

namespace otoworm {

size_t TimingData::section_index(const double point) const
{
    return std::ranges::upper_bound(*this, point) - begin() - 1;
}

double TimingData::section_value(const double point) const
{
    if (empty()) return -1;

    if (point < (*this)[0].time)
        return (*this)[0].value;
    else
    {
        const auto idx = section_index(point);
#ifndef NDEBUG
        if (idx == SIZE_MAX)
            otoworm::util::debug_break();
#endif
        return (*this)[idx].value;
    }
}

double TimingData::integrate_beats_to_seconds(const double offset, const double beat, const bool abs) const
{
    const auto end = section_index(beat) + 1;
    double time = offset;

    if (beat == 0) return time;

    for (uint32_t i = 0; i < end; i++)
    {
        const double seconds_per_beat = otoworm::spb((*this)[i].value);

        // This behaviour is used whenever we're dealing with a beats-based system that skips negative beats, that is to say, stepmania for instance...
        double interval_seconds_per_beat = seconds_per_beat;
        if (abs) interval_seconds_per_beat = ::abs(interval_seconds_per_beat);

        if (i + 1 < end) // Get how long the current timing goes.
        {
            const double duration_beats = (*this)[i + 1].time - (*this)[i].time; // Section lasts this much.
            double duration_time = duration_beats * interval_seconds_per_beat;

            if (beat < (*this)[i + 1].time && beat > (*this)[i].time)
            {
                // If this is our interval, stop summing time of previous intervals before this one
                duration_time = (beat - (*this)[i].time) * interval_seconds_per_beat;
            }
            time += duration_time;
        }
        else
            time += (beat - (*this)[i].time) * interval_seconds_per_beat;
    }
    return time;
}

void TimingData::changes_in_interval(const double point_a, const double point_b, TimingData &out) const
{
    out.clear();

    for (auto i : *this)
    {
        if (i.time >= point_a && i.time < point_b)
        {
            out.push_back(i);
        }
    }
}

void TimingData::load_list(std::string line, const bool allow_zeros)
{
    std::string list_string = line.substr(line.find_first_of(':') + 1);
    TimingSegment segment;

    clear();
    // Remove whitespace.
    otoworm::util::replace_all(list_string, "\n", "");
    const auto items = otoworm::util::token_split(list_string); // Separate List of BPMs.
    for (const auto& item : items)
    { // Separate Time=Value pairs.

        if (auto pair = otoworm::util::token_split(item, "="); pair.size() == 1) // Assume only one BPM on the whole list.
        {
            segment.value = atof(pair[0].c_str());
            segment.time = 0;
        }
        else // Multiple BPMs.
        {
            segment.time = atof(pair[0].c_str());
            segment.value = atof(pair[1].c_str());
        }

        if (allow_zeros || segment.value)
            push_back(segment);
    }
}

double TimingData::elapsed_stop_time_at_beat(const double beat) const
{
    double time = 0;

    if (beat == 0 || empty()) return time;

    for (const auto i : *this)
    {
        if (i.time < beat)
            time += i.value;
    }

    return time;
}

double TimingData::integrate_to_time(const double time) const
{
    if (empty()) return 0;

    const auto end = section_index(time);
    double out = 0;

    if (time <= (*this)[0].time) // Time is behind all.
    {
        out = -((*this)[0].time - time) * (*this)[0].value;
    }
    else // Time comes after first entry.
    {
        for (auto i = 0; i < end; i++)
            out += ((*this)[i + 1].time - (*this)[i].time) * (*this)[i].value;

        out += (time - (*this)[end].time) * (*this)[end].value;
    }

    return out;
}

double TimingData::time_at_integrated_value(const double value) const
{
    if (empty()) return 0;

    double integrated = 0;
    for (auto i = begin(); i != end(); ++i)
    {
        const auto next = i + 1;
        if (next == end())
            break;

        const auto section_duration = next->time - i->time;
        const auto section_value = section_duration * i->value;
        if (value <= integrated + section_value)
        {
            if (i->value == 0)
                return next->time;
            return i->time + (value - integrated) / i->value;
        }

        integrated += section_value;
    }

    const auto& last = back();
    if (last.value == 0)
        return last.time;
    return last.time + (value - integrated) / last.value;
}

TimingData bps_from_beat_timing(const TimingData& timing, const TimingData& stops, const double offset)
{
    TimingData bps;
    for (const auto& section : timing)
    {
        bps.emplace_back(
            timing.integrate_beats_to_seconds(offset, section.time) + stops.elapsed_stop_time_at_beat(section.time),
            bpm_to_bps(section.value));
    }

    for (const auto& stop : stops)
    {
        const auto stop_start = timing.integrate_beats_to_seconds(offset, stop.time) + stops.elapsed_stop_time_at_beat(stop.time);
        const auto stop_end = stop_start + stop.value;
        auto restore_bps = bpm_to_bps(timing.section_value(stop.time));

        for (auto i = bps.begin(); i != bps.end();)
        {
            if (i->time == stop_start || (i->time > stop_start && i->time <= stop_end))
            {
                restore_bps = i->value;
                i = bps.erase(i);
                continue;
            }
            ++i;
        }

        bps.emplace_back(stop_start, 0);
        bps.emplace_back(stop_end, restore_bps);
    }

    std::ranges::sort(bps);
    return bps;
}

TimingData bps_from_time_timing(const TimingData& timing, const double offset)
{
    TimingData bps;
    for (const auto& section : timing)
        bps.emplace_back(section.time + offset, bpm_to_bps(section.value));
    return bps;
}

TimingData bps_from_beatspace_timing(const TimingData& timing, const double offset)
{
    TimingData bps;
    for (const auto& section : timing)
        bps.emplace_back(section.time + offset, bpm_to_bps(60000.0 / section.value));
    return bps;
}

double quantize_fraction_beat(const double frac)
{
	assert(frac < 1.0);
    return std::min(48.0, floor(frac * 49.0)) / 48.0;
}

double quantize_fraction_measure(const double frac)
{
	assert(frac < 1.0);
    return  floor(frac * 192.0) / 192.0;
}

double quantize_beat(const double beat)
{
    const double dec = quantize_fraction_beat(beat - floor(beat));
    return dec + floor(beat);
}

#define FRACKIND(x,y) if(Row%x==0)fracKind=y

int get_fraction_kind_beat(const double frac)
{
    int fracKind = 1;
    const int Row = quantize_fraction_beat(frac) * 48.0;

    FRACKIND(1, 48);

    // placed on 1/24th of a beat
    FRACKIND(2, 24);

    // placed on 1/16th of a beat
    FRACKIND(3, 16);

    // placed on 1/8th of a beat
    FRACKIND(6, 8);

    // placed on 1/6th of a beat
    FRACKIND(8, 6);

    // placed on 1/4th of a beat
    FRACKIND(12, 4);

    // placed on 1/3rd of a beat
    FRACKIND(16, 3);

    // placed on 1/2nd of a beat
    FRACKIND(24, 2);

    // placed on 1/1 of a beat
    FRACKIND(48, 1);

    return fracKind;
}

}
