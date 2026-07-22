#include <algorithm>

#include "rmath.h"

#include <ChartGroup.h>
#include <ProcessedChart.h>

#include <cassert>
#include <cmath>

// CfgVar DebugMeasurePosGen("MeasurePosGen", "Debug");

namespace otoworm
{
    ProcessedChart::ProcessedChart(const double wait_time)
    {
        this->wait_time = wait_time;
        has_negative_scroll = false;
        has_turntable = false;
        chart = nullptr;
    }

    TimingData process_chart_speed(TimingData& bps, const double constant_user_speed)
    {
        TimingData speed_data;

        // We're using a CMod, so further processing is pointless
        if (constant_user_speed != 0)
        {
            speed_data.push_back(TimingSegment(0, constant_user_speed));
            return speed_data;
        }
        // End CMod

        // Calculate velocity at time based on BPM at time
        for (const auto section : bps)
        {
            double speed_value;
            if (section.value != 0)
            {
                const auto spb = 1 / section.value;
                speed_value = 1 /* UNITS_PER_MEASURE */ / (spb * 4);
            }
            else
                speed_value = 0;

            // We blindly take the bps time that had offset and drift applied.
            speed_data.push_back(TimingSegment(section.time, speed_value));
        }

        // Let first speed be not-null.
        if (!speed_data.empty() && speed_data[0].value == 0)
        {
            for (auto i = speed_data.begin();
                 i != speed_data.end();
                 ++i)
            {
                if (i->value != 0)
                    speed_data[0].value = i->value;
            }
        }

        return speed_data;
    }

    TimingData apply_speed_changes(TimingData speed, TimingData scroll, const double offset, const bool reset)
    {
        std::ranges::sort(scroll);

        const auto unmodified = speed;

        for (auto change = scroll.begin();
             change != scroll.end();
             ++change)
        {
            auto next_change = (change + 1);
            const double change_time = change->time + offset;

            /*
                Find all
                if there exists a speed change which is virtually happening at the same time as this v_speed
                modify it to be this value * factor
            */

            bool insert = true;
            for (auto& seg : speed)
            {
                if (abs(change_time - seg.time) < 0.00001)
                {
                    seg.value *= change->value;
                    insert = false;
                }
            }


            /*
                There are no collisions- insert a new speed at this time
            */

            if (insert)
            {
                if (change_time < 0)
                    continue;

                const auto speedvalue = unmodified.section_value(change_time) * change->value;

                TimingSegment v_speed;

                v_speed.time = change_time;
                v_speed.value = speedvalue;

                speed.push_back(v_speed);
            }

            /*
                Theorically, if there were a v_speed change after this one (such as a BPM change) we've got to modify them
                if they're between this and the next speed change.

                Apparently, this behaviour is a "bug" since osu!mania resets SV changes
                after a BPM change.
            */

            if (reset) // Okay, we're an osu!mania chart, leave the resetting.
                continue;

            // We're not an osu!mania chart, so it's time to do what should be done.
            // All v_speeds with T > current and T < next is a BPM change speed;
            // multiply it by the value of the current speed
            for (auto& vertical_speed : speed)
            {
                if (vertical_speed.time > change_time)
                {
                    // Two options, between two speed changes, or the last one. Second case, NextChange == Scrolls.end().
                    // Otherwise, just move on
                    // Last speed change
                    if (next_change == scroll.end())
                    {
                        vertical_speed.value = change->value * unmodified.section_value(vertical_speed.time);
                    }
                    else
                    {
                        if (vertical_speed.time < next_change->time) // Between speed changes
                            vertical_speed.value = change->value * unmodified.section_value(vertical_speed.time);
                    }
                }
            }
        }

        std::ranges::sort(speed);
        return speed;
    }

    double ProcessedChart::get_warp_amount(const double time) const
    {
        double w_amt = 0;
        for (const auto warp : warps)
        {
            if (warp.time < time)
                w_amt += warp.value;
        }

        return w_amt;
    }

    bool ProcessedChart::is_warping_at(const double start_time) const
    {
        const auto it = std::lower_bound(warps.begin(), warps.end(), start_time, TimeSegmentCompare<TimingSegment>);
        if (it != warps.end())
            return it->time + it->value > start_time;
        return false;
    }


    ProcessedChart ProcessedChart::from(Chart* diff, double constant_user_speed)
    {
        /* TODO */
        ProcessedChart out(1.5);
        auto& data = diff->transient;
        if (data == nullptr)
            throw std::runtime_error("Tried to pass a metadata-only difficulty to Player Chart data generator.\n");

        out.chart = diff;
        out.bps = data->bps;
        out.speeds = process_chart_speed(out.bps, constant_user_speed);


        out.has_turntable = diff->transient->has_turntable;


        if (constant_user_speed == 0)
        {
            out.speeds = apply_speed_changes(out.speeds, data->scrolls, diff->offset, false);

            out.warps = data->warps;
            out.interpolated_speed_multipliers = data->interpolated_speed_multipliers;
        }

        /* For all channels of this difficulty */
        for (int chan = 0; chan < diff->channels; chan++)
        {
            int measure_nr = 0;
            auto measure_beat = 0.0;

            /* For each measure of this channel */
            for (auto measure : data->measures)
            {
                /* For each note in the measure... */

                for (auto current_note : measure.notes[chan])
                {
                    /*
                        Calculate position. (Change this to TrackNote instead of processing?)
                        issue is not having the speed change data there.
                    */
                    TrackNote new_note;

                    new_note.assign_note_data(current_note);

                    auto vertical_position = out.speeds.integrate_to_time(new_note.get_start_time());
                    auto hold_end_position = out.speeds.integrate_to_time(new_note.get_end_time());

                    // if upscroll change minus for plus as well as matrix at screengameplay
                    new_note.assign_position(vertical_position, hold_end_position);

                    // Okay, now we want to know what fraction of a beat we're dealing with
                    // this way we can display colored (a la Stepmania) notes.
                    // We should do this before changing time by drift.
                    double note_beat = out.bps.integrate_to_time(new_note.get_start_time());
                    double d_beat = note_beat - measure_beat; // do in relation to position from start of measure
                    double beat_fraction = d_beat - floor(d_beat);

                    new_note.assign_fraction(beat_fraction);

                    // Notes use warped time. Unwarp it.
                    double wamt = -out.get_warp_amount(current_note.start);
                    new_note.add_time(wamt);

                    // !Speed: non-constant
                    // Judgable & ! warping: Constant speed, so only add non-warped notes.
                    if (!constant_user_speed || (new_note.is_judgable() && !out.is_warping_at(current_note.start)))
                        out.notes[chan].push_back(new_note);
                }

                measure_nr++;
                measure_beat += measure.length;
            }
        }

        // Toggle whether we can use our guarantees for optimizations or not at rendering/judge time.
        out.has_negative_scroll = false;

        for (auto s : out.interpolated_speed_multipliers)
            if (s.value < 0) out.has_negative_scroll = true;
        for (auto s : out.speeds)
            if (s.value < 0) out.has_negative_scroll = true;
        return out;
    }

    // audio time -> chart time
    double ProcessedChart::real_to_warped_time(const double song_time) const
    {
        auto t = song_time;
        for (const auto warp : warps)
        {
            if (warp.time <= t)
                t += warp.value;
        }

        return t;
    }

    double ProcessedChart::get_bpm_at(const double time) const
    {
        return bps.section_value(time) * 60;
    }

    double ProcessedChart::get_bps_at(const double time) const
    {
        return bps.section_value(time);
    }


    double ProcessedChart::get_beat_at(const double time) const
    {
        return bps.integrate_to_time(time);
    }

    double ProcessedChart::get_speed_multiplier_at(const double time) const
    {
        // Calculate current speed value to apply.
        const auto current_time = real_to_warped_time(time);

        // speedIter: first which time is greater than current
        auto speed_section = std::ranges::lower_bound(interpolated_speed_multipliers
                                                  ,
                                                  current_time
        );

        double previous_value = 1;
        double current_value = 1;
        double speed_time = current_time;
        double duration = 1;
        bool use_beat_integration = false;

        if (speed_section != interpolated_speed_multipliers.begin())
            --speed_section;

        // Do we have a speed that has a value to interpolate from?
        /*
            Elaboration:
            From the speed at the end of the previous speed, interpolate to speedIter->value
            in speedIter->duration time across the current time - speedIter->time.
        */
        if (speed_section != interpolated_speed_multipliers.begin())
        {
            const auto prev_speed = speed_section - 1;
            previous_value = prev_speed->value;
            current_value = speed_section->value;
            duration = speed_section->duration;
            speed_time = speed_section->time;
            use_beat_integration = speed_section->use_beat_integration;
        }
        else // Oh, we don't?
        {
            if (speed_section != interpolated_speed_multipliers.end())
                current_value = previous_value = speed_section->value;
        }

        double speed_progress;
        if (!use_beat_integration)
            speed_progress = Clamp((current_time - speed_time) / duration, 0.0, 1.0);
        else
        {
            // Assume duration in beats if IntegrateByBeats is true
            speed_progress = Clamp((get_beat_at(current_time) - get_beat_at(speed_time)) / duration, 0.0, 1.0);
        }

        const auto lerped_multiplier = speed_progress * (current_value - previous_value) + previous_value;

        return lerped_multiplier;
    }

    // returns chart time
    double ProcessedChart::get_time_for_beat(const double beat) const
    {
        if (chart)
            return chart->time_for_beat(beat);

        return bps.time_at_integrated_value(beat);
    }

    double ProcessedChart::get_chart_offset() const
    {
        return chart->offset;
    }

    double ProcessedChart::get_time_at_measure(const double msr) const
    {
        double beat = 0;

        if (msr <= 0)
        {
            return 0;
        }

        const auto whole = static_cast<int>(floor(msr));
        const auto fraction = msr - static_cast<double>(whole);
        for (auto i = 0; i < whole; i++)
            beat += chart->transient->measures[i].length;
        beat += chart->transient->measures[whole].length * fraction;

        // Log::Logf("Warping to measure measure %d at beat %f.\n", whole, beat);

        return get_time_for_beat(beat);
    }


    std::map<uint32_t, std::string> ProcessedChart::get_sound_list() const
    {
        assert(chart && chart->transient);
        return chart->transient->sound_list;
    }

    ChartClass ProcessedChart::get_chart_type() const
    {
        assert(chart && chart->transient);
        assert(chart->transient->specialized_info);
        return chart->transient->specialized_info->get_class();
    }

    bool ProcessedChart::is_bmson() const
    {
        return get_chart_type() == CC_BMS &&
            dynamic_cast<BMSChartInfo*>(chart->transient->specialized_info.get())->is_bmson;
    }

    bool ProcessedChart::is_virtual() const
    {
        return chart->has_no_audio_stream;
    }

    bool ProcessedChart::has_timing_data() const
    {
        assert(chart != nullptr);
        return chart->transient && !chart->transient->bps.empty();
    }

    SliceContainer ProcessedChart::get_bmson_slice_data() const
    {
        return chart->transient->slice_data;
    }

    // chart time -> note displacement
    double ProcessedChart::get_displacement_at(const double time) const
    {
        return speeds.integrate_to_time(time);
    }

    // song time -> scroll speed
    double ProcessedChart::get_displacement_speed_at(const double time) const
    {
        return speeds.section_value(real_to_warped_time(time));
    }

}
