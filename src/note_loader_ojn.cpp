#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <rmath.h>
#include <utf8.h>
#include <fstream>

#include <note_loader.h>
#include "text_and_file_util.h"

using namespace otoworm;

//#define lilswap(x) x=((x>>24)&0xFF)|((x<<8)&0xFF0000)|((x<<24)&0xFF00)|((x<<24)&0xFF000000)

// Anything below 8 is a note channel.
#define BPM_CHANNEL 10
#define AUTOPLAY_CHANNEL 9

const char* difficulty_names[] = {"EX", "NX", "HX"};

// based from the ojn documentation at
// http://open2jam.wordpress.com/the-ojn-documentation/

struct OjnHeader
{
    int32_t song_id;
    char signature[4];
    float encode_version;
    int32_t genre;
    float bpm;
    int16_t level[4];
    uint32_t event_count[3];
    uint32_t note_count[3];
    uint32_t measure_count[3];
    uint32_t package_count[3];
    int16_t old_encode_version;
    int16_t old_song_id;
    char old_genre[20];
    int32_t bmp_size;
    int32_t old_file_version;
    char title[64];
    char artist[32];
    char noter[32];
    char ojm_file[32];
    uint32_t cover_size;
    int32_t time[3];
    int32_t note_offset[3];
    int32_t cover_offset;
};

struct OjnPackageHeader
{
    uint32_t measure;
    int16_t channel;
    int16_t events;
};

union OjnPackage
{
    struct
    {
        short note_value;
        char volume_pan;
        char type;
    };

    float float_value;
};

struct OjnExpandedPackage
{
    int channel;
    int note_kind; // undefined if channel is not note or autoplay channel
    float fraction;

    union
    {
        float f_value;
        int int_value;
    };

    bool operator <(const OjnExpandedPackage& other) const
    {
        return fraction < other.fraction;
    }
};

struct OjnMeasure
{
    float length;

    std::vector<OjnExpandedPackage> events;

    OjnMeasure()
    {
        length = 4;
    }
};

class OjnLoadDifficultyContext
{
private:
    double get_beat_for_measure(const int measure)
    {
        double out = 0;

        for (int i = 0; i < measure; i++)
        {
            out += measures_[i].length;
        }

        return out;
    }

    std::vector<OjnMeasure> measures_;

public:
    ChartGroup* s;
    float bpm;
    TimingData timing;

    void read_packages(OjnHeader& header, const int difficulty_index, std::istream& ojnfile)
    {
        // Reserve measures.
        measures_.resize(header.measure_count[difficulty_index] + 1);

        for (size_t package = 0U; package < header.package_count[difficulty_index]; ++package)
        {
            OjnPackageHeader package_header = {};
            ojnfile.read(reinterpret_cast<char*>(&package_header), sizeof(OjnPackageHeader));

            for (size_t event_index = 0U; event_index < package_header.events; ++event_index)
            {
                const auto fraction = static_cast<float>(event_index) / static_cast<float>(package_header.events);
                OjnPackage ojn_package = {};
                OjnExpandedPackage o2evt = {};

                ojnfile.read(reinterpret_cast<char*>(&ojn_package), sizeof(OjnPackage));

                // this happens sometimes, oddly -az
                if (package_header.measure >= measures_.size())
                {
                    measures_.resize(package_header.measure + 1);
                }

                switch (package_header.channel)
                {
                case 0: // fractional measure
                    measures_[package_header.measure].length = 4 * ojn_package.float_value;
                    break;
                case 1: // BPM change
                    o2evt.f_value = ojn_package.float_value;
                    o2evt.channel = BPM_CHANNEL;
                    o2evt.fraction = fraction;
                    measures_[package_header.measure].events.push_back(o2evt);
                    break;
                case 2: // note events (enginechannel = PackageHeader.channel - 2)
                case 3:
                case 4:
                case 5:
                case 6:
                case 7:
                case 8:
                    if (ojn_package.note_value == 0) continue;
                    o2evt.fraction = fraction;
                    o2evt.channel = package_header.channel - 2;
                    o2evt.int_value = ojn_package.note_value;
                    o2evt.note_kind = ojn_package.type;
                    measures_[package_header.measure].events.push_back(o2evt);
                    break;
                default: // autoplay notes
                    if (ojn_package.note_value == 0) continue;
                    o2evt.channel = AUTOPLAY_CHANNEL;
                    o2evt.fraction = fraction;
                    o2evt.int_value = ojn_package.note_value;
                    o2evt.note_kind = ojn_package.type;
                    measures_[package_header.measure].events.push_back(o2evt);
                    break;
                }
            }
        }
    }


    void clean_invalid_events()
    {
        auto current_measure = 0;
        OjnExpandedPackage prev_iter[7] = {-1, -1, -1, -1};

        for (auto& measure : measures_)
        {
            // Sort events. This is very important, since we assume events are sorted!
            std::sort(measure.events.begin(), measure.events.end());

            for (auto& event : measure.events)
            {
                if (event.channel < AUTOPLAY_CHANNEL)
                {
                    if (prev_iter[event.channel].channel != -1) // There is a previous event
                    {
                        if (prev_iter[event.channel].note_kind == 2 &&
                            (event.note_kind == 0 || event.note_kind == 2)) // This note or hold head is in between holds
                        {
                            event.channel = AUTOPLAY_CHANNEL;
                            event.note_kind = 0;
                            continue;
                        }

                        if (prev_iter[event.channel].note_kind != 2 && // Hold tail without ongoing hold
                            event.note_kind == 3)
                        {
                            event.channel = AUTOPLAY_CHANNEL;
                            event.note_kind = 0;
                            continue;
                        }
                    }

                    prev_iter[event.channel] = event;
                }
            }

            current_measure++;
        }
    }

    void write_timing_data(Chart* chart)
    {
        auto current_measure = 0;
        for (const auto& measure : measures_)
        {
            const auto base_beat = get_beat_for_measure(current_measure);

            chart->transient->measures.emplace_back();

            // All fractional measure events were already handled at read time.
            chart->transient->measures[current_measure].length = measure.length;

            for (auto evt : measure.events)
            {
                if (evt.channel != BPM_CHANNEL) continue; // These are the only ones we directly handle.

                /* We calculate beat multiplying fraction by 4 instead of the measure's length,
                because o2jam's measure fractions don't scale the notes inside. */
                auto beat = base_beat + evt.fraction * 4;

                // 0 values must be ignored.
                if (evt.f_value == 0) continue;

                if (!timing.empty())
                {
                    // For some reason, a few BPMs are redundant. Since our events are sorted, there's no need for worry..
                    if (timing.back().value == evt.f_value) // ... We already have this BPM.
                        continue;
                }

                timing.push_back({beat, evt.f_value});
            }

            current_measure++;
        }

        // The BPM info on the header is not for decoration. It's the very first BPM we should be using.
        // A few of the charts already have set BPMs at beat 0, so we only need to add information if it's missing.
        if (timing.empty() || timing[0].time > 0)
        {
            const TimingSegment seg(0, bpm);
            timing.push_back(seg);

            // Since events and measures are ordered already, there's no need to sort
            // timing data unless we insert new information.
            std::ranges::sort(timing, [](const auto& a, const auto& b) { return a.time < b.time; });
        }
    }

    // Based off the O2JAM method at
    // https://github.com/open2jamorg/open2jam/blob/master/parsers/src/org/open2jam/parsers/EventList.java
    void write_to_chart(Chart* out)
    {
        // First, we sort and clear up invalid events.
        clean_invalid_events();

        // Then we need to have just as many measures going out as we've got in here.
        out->transient->measures.reserve(measures_.size());

        // Now to output, we need to process BPM changes and fractional measures.
        write_timing_data(out);

        // Now, we can process notes and long notes.
        write_note_data(out);
    }

    void write_note_data(Chart* out)
    {
        auto current_measure = 0;
        double pending_lns[7] = {};
        int32_t pending_ln_sound[7] = {};

        for (auto msr : measures_)
        {
            const auto measure_base_beat = get_beat_for_measure(current_measure);

            for (auto evt : msr.events)
            {
                if (evt.channel == BPM_CHANNEL) continue;
                const auto beat = measure_base_beat + evt.fraction * 4;
                const auto time = timing.integrate_beats_to_seconds(0, beat);

                if (evt.note_kind % 8 > 3) // Okay... This is obscure. Big thanks to open2jam.
                    evt.int_value += 1000;

                if (evt.channel == AUTOPLAY_CHANNEL) // Ah, autoplay audio.
                {
                    AutoplaySound snd;

                    snd.sound = evt.int_value;
                    snd.time = time;
                    out->transient->bgm_events.push_back(snd);
                }
                else // A note! In this case, we already 'normalized' O2Jam channels into raindrop channels.
                {
                    NoteData note;

                    if (evt.channel >= 7) continue; // Who knows... A buffer overflow may be possible.

                    note.start = time;
                    note.sound = evt.int_value;

                    switch (evt.note_kind)
                    {
                    case 0:
                        out->transient->measures[current_measure].notes[evt.channel].push_back(note);
                        break;
                    case 2:
                        pending_lns[evt.channel] = time;
                        pending_ln_sound[evt.channel] = evt.int_value;
                        break;
                    case 3:
                        note.start = pending_lns[evt.channel];
                        note.end_time = time;
                        note.sound = pending_ln_sound[evt.channel];
                        out->transient->measures[current_measure].notes[evt.channel].push_back(note);
                        break;
                    default: ;
                    }
                }
            }
            current_measure++;
        }
    }
};


bool is_valid_ojn(std::istream& filein, OjnHeader* head)
{
    filein.read(reinterpret_cast<char*>(head), sizeof(OjnHeader));

    if (!filein)
    {
        //if (filein.eofbit)
        //    Log::Printf("NoteLoaderOJN: EOF reached before header could be read\n");
        return false;
    }

    return strcmp(head->signature, "ojn") == 0;
}

const char* load_ojn_cover(const std::filesystem::path& filename, size_t& read)
{
    std::fstream filein(filename.string(), std::ios::binary | std::ios::in);
    OjnHeader head = {};
    char* out;

    if (!filein)
    {
        // Log::Printf("NoteLoaderOJN: %s could not be opened\n", filename.c_str());
        return 0;
    }

    if (!is_valid_ojn(filein, &head))
        return "";

    out = new char[head.cover_size];

    filein.seekg(head.cover_offset, std::ios::beg);
    filein.read(out, head.cover_size);
    read = head.cover_size;

    return out;
}


void NoteLoaderOJN::LoadObjectsFromStream(std::istream& filein, ChartGroup* out)
{
    OjnHeader head = {};

    if (!filein)
    {
        throw std::runtime_error("NoteLoaderOJN: input stream is not readable.");
    }

    if (!is_valid_ojn(filein, &head))
    {
        throw std::runtime_error("NoteLoaderOJN: input stream is not a valid OJN.");
    }

    std::string v_artist;
    std::string v_name;
    std::string noter;
    /*
        These are the only values we display, so we should clean them up so that nobody cries.
        Of course, the right thing to do would be to iconv these, but unless
        I implement some way of detecting encodings, this is the best we can do in here.
    */
    utf8::replace_invalid(head.artist, head.artist + 32, std::back_inserter(v_artist));
    utf8::replace_invalid(head.title, head.title + 64, std::back_inserter(v_name));
    utf8::replace_invalid(head.noter, head.noter + 32, std::back_inserter(noter));

    out->artist = v_artist;
    out->title = v_name;
    out->song_filename = head.ojm_file;

    for (auto i = 0; i < 3; i++)
    {
        OjnLoadDifficultyContext ctx;
        auto chart = std::make_unique<Chart>();

        chart->transient = std::make_shared<ChartTransient>();
        chart->transient->specialized_info = std::make_shared<O2JamChartInfo>();
        auto info = dynamic_cast<O2JamChartInfo*>(chart->transient->specialized_info.get());

        switch (i)
        {
        case 0:
            info->difficulty = O2JamChartInfo::O2_EX;
            break;
        case 1:
            info->difficulty = O2JamChartInfo::O2_NX;
            break;
        case 2:
        default:
            info->difficulty = O2JamChartInfo::O2_HX;
            break;
        }

        chart->level = head.level[i];

        chart->meta.emplace();
        chart->meta->author = noter;

        ctx.s = out;
        filein.seekg(head.note_offset[i]);

        chart->duration = head.time[i];
        chart->meta->name = difficulty_names[i];
        chart->channels = 7;
        chart->has_no_audio_stream = true;
        ctx.bpm = head.bpm;

        /*
            The implications of this structure are interesting.
            Measures may be unordered; but events may not, if only there's one package per channel per measure.
        */
        ctx.read_packages(head, i, filein);

        // Process Info... then push back difficulty.
        ctx.write_to_chart(chart.get());
        chart->transient->bps = bps_from_beat_timing(ctx.timing, chart->transient->stops, chart->offset);
        out->charts.push_back(std::move(chart));
    }
}
