#pragma once

#include <timing.h>
#include <constants.h>
#include <track_note.h>
#include <vector>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>

namespace otoworm
{

struct AutoplaySound : public TimedEvent < AutoplaySound, double >
{
    uint32_t sound;

    AutoplaySound() : TimedEvent(0), sound(0) {};
    AutoplaySound(double T, uint32_t V) : TimedEvent(T), sound(V) {}
};

struct AutoplayBMP : public TimedEvent < AutoplayBMP, float >
{
    int bmp;
    AutoplayBMP() : TimedEvent(0), bmp(0) {};
    AutoplayBMP(double T, int V) : TimedEvent(T), bmp(V) {};
};

template<class T>
 bool TimeSegmentCompare(const T &t, const double &v) {
	return t.time < v;
}

struct SliceInfo
{
    double start, end;
};

struct SliceContainer
{
    std::map<int, std::string> audio_files; // int := snd index, std::string := file
    std::map<int, std::map<int, SliceInfo>> slices; // 1st int := wav index, 2nd int := snd index, Slice Info, where to cut for 2nd int for wav 1st int
};

    constexpr size_t MAX_CHANNELS = 16;

    struct Measure
    {
        std::vector<NoteData> notes[MAX_CHANNELS];
        double length; // In beats. 4 by default.

        Measure()
        {
            length = 4;
        }
    };

    struct SpeedSection : TimedEvent<SpeedSection, double>
    {
        double duration;
        double value;
        bool use_beat_integration; // if true, integrate by beats, if false, by time.

        SpeedSection() {
            duration = 0;
            value = 0;
            use_beat_integration = false;
        }
    };

    typedef std::vector<SpeedSection> VectorInterpolatedSpeedMultipliers;

    typedef std::vector<Measure> VectorMeasure;

    typedef std::vector<TrackNote> VectorTrackNote[MAX_CHANNELS];

    class ChartInfo
    {
    protected:
        ChartClass type;
    public:
        ChartInfo()
        {
            type = CC_NULL;
        }

        virtual ~ChartInfo() {}

        ChartClass get_class() const;
    };

    class BMSChartInfo : public ChartInfo
    {
    public:
        float judge_rank;

        float gauge_total;

        // neccesary because of regular BMS DEFEXRANK
        bool percentual_judgerank;

        // Whether this uses BMSON features.
        // (Also makes GaugeTotal a rate instead of an absolute)
        bool is_bmson;

        BMSChartInfo()
        {
            type = CC_BMS;
            judge_rank = 3;
            gauge_total = -1;
            is_bmson = false;
            percentual_judgerank = false;
        }
    };

    class OsumaniaChartInfo : public ChartInfo
    {
    public:
        float hp, overall_difficulty;
        OsumaniaChartInfo()
        {
            type = CC_OSUMANIA;
            hp = 5;
            overall_difficulty = 5;
        }
    };

    class O2JamChartInfo : public ChartInfo
    {
    public:
        enum
        {
            O2_EX,
            O2_NX,
            O2_HX
        } difficulty;

        O2JamChartInfo()
        {
            type = CC_O2JAM;
            difficulty = O2_HX;
        }
    };

    class StepmaniaChartInfo : public ChartInfo
    {
    public:
        StepmaniaChartInfo()
        {
            type = CC_STEPMANIA;
        }
    };

    struct BMPEventsDetail
    {
        std::map<int, std::string> bmp_list;
        std::vector<AutoplayBMP> layer_base;
        std::vector<AutoplayBMP> layer_upper;
        std::vector<AutoplayBMP> layer_upper2;
        std::vector<AutoplayBMP> layer_miss;
    };

    struct ChartTransient
    {
        // Time-domain timing. value is beats per second at chart time.
        TimingData bps;

        // Contains stops data.
        TimingData stops;

        // For scroll changes, as obvious as it sounds.
        TimingData scrolls;

        // At Time, warp Value seconds forward.
        TimingData warps;

        // Notes (Up to MAX_CHANNELS tracks)
        VectorMeasure measures;

        // For Speed changes.
        VectorInterpolatedSpeedMultipliers interpolated_speed_multipliers;

        // Autoplay Sounds
        std::vector<AutoplaySound> bgm_events;

        // Autoplay BMP
        std::optional<BMPEventsDetail> bmp_events;

        // Timing Info
        std::shared_ptr<ChartInfo> specialized_info;

        // id/file sound list map;
        std::map<uint32_t, std::string> sound_list;

        // Background/foreground to show when loading.
        std::string stage_file;

        // Genre (Display only, for the most part)
        std::string genre;

        // Whether this difficulty uses the scratch channel (being channel/index 0 always used for this)
        bool has_turntable;

        // Identification purposes
        std::string file_hash;
        int index_in_file;

        // Audio slicing data
        SliceContainer slice_data;

        uint32_t get_total_note_count() const;
        uint32_t get_scorable_note_count() const;

        ChartTransient()
        {
            has_turntable = false;
            index_in_file = -1;
        }

        virtual ~ChartTransient() = default;
    };

    struct OsumaniaChartTransient : public ChartTransient
    {
        // osu!mania storyboard/event content, saved for later parsing.
        std::string osb_sprites;
    };

    struct ChartMetadata {
        // Metadata
        std::string name;
        std::filesystem::path path;
        std::string author;
    };

    struct Chart
    {
        /** @brief time to beat 0 of measure 0.
         * displaces all notes by this amount relative to
         * mp3 if applicable **/
        double offset;
        /** @brief duration of the song in warped time;
         * warped time to use for considering a song "finished" */
        double duration; // In warped time, not judgment time

        /** @brief internal chart list ID **/
        size_t id;
        std::optional<ChartMetadata> meta;

        // VSRG
        std::shared_ptr<ChartTransient> transient;

        long long level;
        unsigned char channels;
        bool has_no_audio_stream;

        Chart() {
            id = -1;
            duration = 0;
            offset = 0;
            has_no_audio_stream = false;
            channels = 0;
            level = 0;
            transient = nullptr;
        }

        void reset_transient(); // remove non-metadata
        double time_for_beat(double beat) const;
        double beat_at_time(double time) const;
    };

    class ChartGroup
    {
    public:

        int id;
        std::vector<std::shared_ptr<Chart>> charts;

        /* Song title */
        std::string title;

        /* Song Author */
        std::string artist;

        /* Directory where files are contained */
        std::filesystem::path path;

        /* Relative Paths */
        std::filesystem::path song_filename, background_filename;

        /* Song Audio for Preview*/
        std::filesystem::path song_preview_source;

        /* Time to start preview */
        float preview_time;

        // Song subtitle
        std::string subtitle;

        // Song genre
        std::string genre;

        // returns pointer owned by Song class, so don't delete.
        Chart* get_chart(uint32_t i) const;

        uint8_t get_chart_count() const;

        ChartGroup() { id = -1; preview_time = 0; };
        virtual ~ChartGroup() = default;
    };
/* Song Timing */
inline double spb(double bpm) { return 60 / bpm; } // Return seconds per beat.
inline double bpm_to_bps(double bpm) { return bpm / 60; } // Return beats per second.

TimingData bps_from_beat_timing(const TimingData& timing, const TimingData& stops, double offset = 0);
TimingData bps_from_time_timing(const TimingData& timing, double offset = 0);
TimingData bps_from_beatspace_timing(const TimingData& timing, double offset = 0);

// Quantizes fraction to a beat's maximum resolution (1/48th of a beat)
double quantize_fraction_beat(double Frac);

// Quantizes fraction to a measure's maximum resolution (1/192nd of a measure)
double quantize_fraction_measure(double Frac);

// Quantizes beat to a beat's maximum resolution (1/48th of a beat)
double quantize_beat(double Beat);

}
