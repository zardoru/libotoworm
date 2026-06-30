#pragma once


#include "ChartGroup.h"

namespace otoworm {
    struct ProcessedChart {
        TimingData           speeds;
        TimingData		     bps;
        TimingData		     warps;
        VectorInterpolatedSpeedMultipliers   interpolated_speed_multipliers;
        VectorTrackNote       notes;
        bool has_negative_scroll;
        bool has_turntable;

        double wait_time;
        Chart* chart;

        ProcessedChart(const double _waitTime);

        // Chart data functions
        double real_to_warped_time(double song_time) const;
        double get_warp_amount(double time) const;
        bool is_warping_at(double start_time) const;
        double get_speed_multiplier_at(double time) const; // Time in unwarped song time
        double get_bpm_at(double time) const;
        double get_bps_at(double time) const;
        double get_beat_at(double time) const;
        double get_displacement_at(double time) const;
        double get_displacement_speed_at(double time) const; // in unwarped song time
        double get_time_for_beat(double beat) const;
        double get_chart_offset() const;
        double get_time_at_measure(double msr) const;

        std::map<uint32_t, std::string> get_sound_list() const;
        ChartClass get_chart_type() const;
        bool is_bmson() const;
        bool is_virtual() const;
        bool has_timing_data() const;
        SliceContainer get_bmson_slice_data() const;

        // Drift is an offset to apply to _everything_.
        // Speed is a constant to set the speed to.
        static ProcessedChart from(Chart *diff, double speed = 0);
    };
}
