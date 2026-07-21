#include <filesystem>
#include <rmath.h>

#include <format>
#include <unordered_set>
#include <regex> 
#include <fstream>
#include <utility>
#include <ChartGroup.h>

#include <text_and_file_util.h>
#include <json.hpp>


using Json = nlohmann::json;
using namespace otoworm;

// All non-standard exceptions are marked with NSE.

namespace NoteLoaderBMS
{
    /* from NoteLoaderBMS */
    std::string get_subtitles(const std::string& SLine, std::unordered_set<std::string> &Out);
}

namespace NoteLoaderBMSON
{
    auto UNSPECIFIED_VERSION = "0.21";
    auto VERSION_1 = "1.0.0";

    // Beat = locate / resolution.
    constexpr double BMSON_DEFAULT_RESOLUTION = 240.0;

    struct BmsonNote
    {
        double length; // in beats
        uint64_t sound; // Slice mapped to.
    };

    struct
    {
        const char* hint;
        size_t keys;
        std::vector<int16_t> mappings;
        bool has_turntable;
    } bmson_layouts[] = {
        { "beat-7k", 8, { 1, 2, 3, 4, 5, 6, 7, 0 }, true },
        { "beat-5k", 6, { 1, 2, 3, 4, 5, -1, -1, 0 }, true },
        { "beat-10k", 12, { 1, 2, 3, 4, 5, -1, -1, 0, 7, 8, 9, 10, 11, -1, -1, 6 }, true },
        { "beat-14k", 16, { 1, 2, 3, 4, 5, 6, 7, 0, 9, 10, 11, 12, 13, 14, 15, 8 }, true },
        { "popn-5k", 5, { 0, 1, 2, 3, 4 }, false },
        { "popn-9k", 9, { 0, 1, 2, 3, 4, 5, 6, 7, 8 }, false }
    };

    class BMSONException : public std::exception
    {
    private:
	std::string msg; 
    public:
        explicit BMSONException(const char * what) : exception(), msg(what) {}
	[[nodiscard]] const char* what() const noexcept override { return msg.c_str(); }
    };

    struct BmsonObject
    {
        int x; // lane
        double y; // pulse
        double l; // length
        bool c; // continuation flag
    };

    class BMSONLoader
    {
        Json root;
        std::istream& input;
        ChartGroup* song;
        std::unique_ptr<Chart> chart;
        BMSChartInfo* timing_info;
        std::unordered_set<std::string> subtitles;
        std::string version;
        TimingData timing;
        double resolution;

        int current_wav;
        std::vector<int16_t> mappings;

        std::map<int, std::map<double, BmsonNote> > notes; // int := mapped lane; double := time in beats of obj (for :mix-note)

        SliceContainer slices;

        std::string get_subartist(const char *string)
        {
            const std::regex sreg(std::format(R"(\s*{}\s*:\s*(.*?)\s*$)", string),
				std::regex_constants::icase | std::regex_constants::ECMAScript);

            for (const auto& s : root["info"]["subartists"])
            {
                std::smatch sm;
                std::string str = s;
                if (regex_search(str, sm, sreg))
                {
                    return sm[1];
                }
            }

            return "";
        }

        void initialize_mappings(Json values)
        {
            const std::regex generic_keys("generic\\-(\\d+)keys");
            const std::regex special_keys("special\\-(\\d+)keys");
            std::smatch sm;

            mappings.clear();

            if (values.is_null())
            {
                // default_layout:
                chart->transient->has_turntable = true;
                chart->channels = 8;
                mappings = bmson_layouts[0].mappings;
                return;
            }

            const std::string s = values;
            if (regex_search(s, sm, generic_keys))
            {
                const int chans = atoi(sm[1].str().c_str());
                if (chans <= MAX_CHANNELS)
                {
                    chart->channels = chans;
                    for (int i = 0; i < chans; i++)
                        mappings.push_back(i);
                    return;
                }
            }

            if (regex_search(s, sm, special_keys))
            {
                const int chans = atoi(sm[1].str().c_str());
                if (chans <= MAX_CHANNELS)
                {
                    chart->channels = chans;
                    chart->transient->has_turntable = true;
                    for (int i = 0; i < chans; i++)
                        mappings.push_back(i);
                    return;
                }
            }

            for (const auto& layout : bmson_layouts)
            {
                if (values == layout.hint)
                {
                    chart->channels = layout.keys;
                    mappings = layout.mappings;
                    chart->transient->has_turntable = layout.has_turntable;
                    return;
                }
            }

            // Okay then, didn't match anything...
            throw BMSONException(std::format("Unknown mode hint: \"{}\"", values.get<std::string>()).c_str());
        }

        void load_meta()
        {
            auto meta = root["info"];
            song->title = NoteLoaderBMS::get_subtitles(meta["title"], subtitles);
            song->artist = meta["artist"];
            song->subtitle = meta["subtitle"].get<std::string>() + util::join(subtitles, " ");

            song->background_filename = meta["back_image"].get<std::string>();

            for (auto &s : meta["subtitles"])
                subtitles.insert(s.get<std::string>());

            if (version == UNSPECIFIED_VERSION)
                timing.push_back(TimingSegment(0, meta["initBPM"]));
            else if (version == VERSION_1)
            {
                if (meta["init_bpm"].is_null())
                    throw BMSONException("Unspecified init_bpm!");
                timing.push_back(TimingSegment(0, meta["init_bpm"]));
            }

            chart->level = meta["level"];

            chart->meta->author = get_subartist("chart");

            if (!meta["chart_name"].is_null())
                chart->meta->name = meta["chart_name"];

            if (!meta["eyecatch_image"].is_null())
                chart->transient->stage_file = meta["eyecatch_image"];
            initialize_mappings(meta["mode_hint"]);

            chart->has_no_audio_stream = true;

            if (!meta["resolution"].is_null())
                resolution = abs(meta["resolution"].get<double>());

            if (resolution == 0) resolution = BMSON_DEFAULT_RESOLUTION;

            song->song_preview_source = meta["preview_music"].get<std::string>();

            // DEFEXRANK!
            double jRank;

            Json jr;
            if (version == UNSPECIFIED_VERSION)
                jr = meta["judgeRank"];
            else
                jr = meta["judge_rank"];

            if (!jr.is_null())
                jRank = jr;
            else
                jRank = 100;

			timing_info->judge_rank = jRank;
			timing_info->percentual_judgerank = true;

            timing_info->gauge_total = meta["total"];
        }

        double find_last_note_beat()
        {
            double last_y = -std::numeric_limits<double>::infinity();
            Json *sc;
            get_sound_channels(sc);
            for (auto &&s : *sc)
            {
                auto notes = s["notes"];
                for (auto &&note : notes)
                {
                    last_y = std::max(note["y"].get<double>(), last_y);
                }
            }

            if (std::isinf(last_y)) return 0;

            return last_y / resolution;
        }

        void load_measure_lengths()
        {
            size_t Measure = 0;
            auto& lines = root["lines"];
            auto& Measures = chart->transient->measures;

            if (!lines.is_array()) return;

            if (!lines.empty())
            {
                // ommitted 0- first measure is...
                if (lines[0]["y"].get<double>() != 0)
                {
                    Measures.resize(1);
                    Measures[0].length = lines[0]["y"].get<double>() / resolution;
                    Measure++;
                }

                // now get measure lengths appropietly.
                // the first is not getting accounted for since that was looked at just before.
                for (auto msr = lines.begin(); msr != lines.end(); ++msr)
                {
                    auto next = msr; ++next;
                    if (next != lines.end())
                    {
                        const double duration = ((*next)["y"].get<double>() - (*msr)["y"].get<double>()) / resolution;

                        if (Measure >= Measures.size())
                            Measures.resize(Measure + 1);

                        Measures[Measure].length = duration;
                    }
                    Measure++;
                }
            }

            // make Measure point to the last added measure
            Measure -= 1;
            if (Measure == -1) // none? so it's an array huh
            {
                Measure++;
                Measures.resize(1);
            }

            double prev = 0;

            for (size_t i = 0; i < Measure; i++) // set length of last measure to difference between last note and last measure's beats.
                prev += chart->transient->measures[i].length;

            chart->transient->measures[chart->transient->measures.size() - 1].length = find_last_note_beat() - prev;
        }

        void load_timing()
        {
            Json bpm_notes;
            Json stop_notes;

            if (version == UNSPECIFIED_VERSION)
            {
                bpm_notes = root["bpmNotes"];
                stop_notes = root["stopNotes"];
            }
            else if (version == VERSION_1)
            {
                bpm_notes = root["bpm_events"];
                stop_notes = root["stop_events"];
            }

            for (auto bpm : bpm_notes)
            {
                const double y = bpm["y"].get<double>() / resolution;
                double val;

                if (version == UNSPECIFIED_VERSION)
                    val = bpm["v"];
                else
                    val = bpm["bpm"];

                timing.push_back(TimingSegment(y, val));
            }

            for (auto stop : stop_notes)
            {
                double y = stop["y"].get<double>() / resolution;
                double val;

                if (version == UNSPECIFIED_VERSION)
                    val = spb(timing.section_value(y)) * stop["v"].get<double>() / resolution;
                else
                    val = spb(timing.section_value(y)) * stop["duration"].get<double>() / resolution;

                chart->transient->stops.push_back(TimingSegment(y, val));
            }
        }

        double time_for_obj(const double beat)
        {
            return timing.integrate_beats_to_seconds(0, beat) + chart->transient->stops.elapsed_stop_time_at_beat(beat);
        }

        size_t measure_for_beat(const double beat)
        {
            // stub
            double acom = 0;
            size_t Measure = 0;
            while (acom < beat)
            {
                if (chart->transient->measures.size() > Measure)
                    acom += chart->transient->measures[Measure].length;
                else
                    acom += 4;
                Measure++;
            }

            return Measure;
        }

        void get_sound_channels(Json* &snd_channel)
        {
            if (version == UNSPECIFIED_VERSION)
                snd_channel = &root["soundChannel"];
            else
                snd_channel = &root["sound_channels"];
        }

        int get_mapped_lane(BmsonObject& note)
        {
            int lane = note.x - 1;
            if (lane < 0)
                return lane;
            if (lane < mappings.size())
                return mappings[lane];
            throw BMSONException(std::format("x = {} out of bounds for mode hint", lane).c_str());
        }

        int slice(const int sound_index, double &last_time, std::vector<BmsonObject>& objects, std::vector<BmsonObject>::iterator& note)
        {
            double st = 0, et = std::numeric_limits<double>::infinity();
            const auto next = note + 1;
            if (note->c)
            {
                if (note != objects.begin())
                {
                    st = last_time;
                    // if it's not the first note, use the previous note's time if this doesn't reset.
                }

                // if the next is the last note, use what remains of the audio, else
                // use the time between this and the next note as the end cue point
                if (next != objects.end())
                    last_time = et = st + time_for_obj(next->y / resolution) - time_for_obj(note->y / resolution);
            }
            else
            {
                st = 0;
                // if there's a note up ahead, use that as the end time
                // otherwise, use the entire audio
                if (next != objects.end())
                {
                    const double duration = time_for_obj(next->y / resolution) - time_for_obj(note->y / resolution);
                    last_time = et = duration;
                }
            }

            // Add this slice as an object or to an existing object
            double note_time = note->y / resolution;
            const int lane = get_mapped_lane(*note);

            // is there a note already at this line, at this time?
            // (:mix-note)
            if (notes[lane].find(note_time) != notes[lane].end())
            {
                // get the slices for the wav of this note, add this sound's slice to it
                auto &slice = slices.slices[notes[lane][note_time].sound];

                // check if the slice duration is similar?
                // w/e lol. keep this slice as the last declared if overlap exists.
                slice[sound_index].start = st;
                slice[sound_index].end = et;

                return notes[lane][note_time].sound;
            }
            else // there's no note on this lane or at this time
            {
                const int wav = current_wav;
                // a: TODO: find good criteria to reuse slice

                // b: the slice is different (wav == current_wav is true)
                auto &slice = slices.slices[wav];
                slice[sound_index].start = st;
                slice[sound_index].end = et;

                // add a new object.
                add_object(note, wav);

                if (wav == current_wav)
                    current_wav++;

                return wav;
            }
        }

        void add_object(std::vector<BmsonObject>::iterator& note, const int wav_index)
        {
            const int lane = get_mapped_lane(*note);
            const double beat = note->y / resolution;
            const double len = note->l / resolution;

            if (lane < -1) // only for slice purposes?
                return;

            notes[lane][beat].sound = wav_index;
            notes[lane][beat].length = len;            
        }

        std::string clean_filename(std::string nam)
        {
            nam = regex_replace(nam, std::regex("\\.\\./?"), "");
            // remove C:/... or / or C:\...

            nam = regex_replace(nam, std::regex(R"(^\s*(.:)?[/\\])"), "");

            return nam;
        }

        void add_global_slice_sound(std::string nam, const int sound_index)
        {
            // remove ".."
            nam = clean_filename(nam);

            // Add to index
            slices.audio_files[sound_index] = nam;
        }

        static void join_bgm_slices(std::vector<BmsonObject> &objs)
        {
            for (auto obj = objs.begin(); obj != objs.end(); )
            {
                if (obj->x == 0)
                {
                    // BGM track? No need to have them separated, join them.
                    auto ni = obj + 1;

                    // It's a BGM track and it doesn't restart audio?
                    while (ni != objs.end() && ni->x == 0 && ni->c)
                    {
                        ni = objs.erase(ni);
                    }

                    obj = ni;
                    continue;
                }

                ++obj;
            }
        }

        void load_notes()
        {
            int sound_index = 1;
            Json *snd_channel;
            get_sound_channels(snd_channel);

            for (auto & audio : *snd_channel)
            {
                double last_time = 0;
                add_global_slice_sound(audio["name"], sound_index);

                std::vector<BmsonObject> objs;

                auto notes = audio["notes"];
                for (auto &note : notes)
                    objs.push_back(BmsonObject
				{ 
					note["x"], 
					note["y"], 
					note["l"], 
					note["c"]
				});

                std::stable_sort(objs.begin(), objs.end(), 
					[](const BmsonObject& l, const BmsonObject& r) -> bool { 
					return l.y < r.y; 
				});
                join_bgm_slices(objs);

                for (auto note = objs.begin(); note != objs.end(); ++note)
                    slice(sound_index, last_time, objs, note);
                sound_index += 1;
            }
        }

        void load_bga()
        {
            if (version != VERSION_1) return; // BGA unsupported on version 0.21
            const auto out = std::make_shared<BMPEventsDetail>();

            auto& bga = root["bga"];
            if (!bga.is_null())
            {
				bool hasData = false;
                for (auto &bgi : bga["bga_header"])
                    out->bmp_list[bgi["id"]] = clean_filename(bgi["name"]);

				if (version != VERSION_1) {
					for (auto &bg0 : bga["bga_notes"]) {
						out->layer_base.emplace_back(
                            AutoplayBMP(
                                time_for_obj(bg0["y"].get<double>() / resolution), 
                                bg0["id"]
                            )
                        );
						hasData = true;
					}
					for (auto &bg0 : bga["layer_notes"]) {
						out->layer_upper.emplace_back(
                            AutoplayBMP(
                                time_for_obj(bg0["y"].get<double>() / resolution), 
                                bg0["id"]
                            )
                        );
						hasData = true;
					}
					for (auto &bg0 : bga["poor_notes"]) {
						out->layer_miss.emplace_back(
                            AutoplayBMP(
                                time_for_obj(bg0["y"].get<double>() / resolution), 
                                bg0["id"]
                            )
                        );

						hasData = true;
					}
				}
				else {
					for (auto &bg0 : bga["bga_events"]) {
						out->layer_base.emplace_back(
                            AutoplayBMP(
                                time_for_obj(bg0["y"].get<double>() / resolution), 
                                bg0["id"]
                            )
                        )
                        ;
						hasData = true;
					}
					for (auto &bg0 : bga["layer_events"]) {
						out->layer_upper.emplace_back(
                            AutoplayBMP(
                                time_for_obj(bg0["y"].get<double>() / resolution), 
                                bg0["id"]
                            )
                        );
						hasData = true;
					}
					for (auto &bg0 : bga["poor_events"]) {
						out->layer_miss.emplace_back(
                            AutoplayBMP(
                                time_for_obj(bg0["y"].get<double>() / resolution), 
                                bg0["id"]
                            )
                        );
						hasData = true;
					}
				}

				if (hasData)
					chart->transient->bmp_events = *out;
            }
        }
    public:

        BMSONLoader(std::istream& inp, ChartGroup* out) : input(inp), timing_info(nullptr)
        {
            input >> root;
            song = out;

            resolution = BMSON_DEFAULT_RESOLUTION;
            current_wav = 1;
        }

        void add_notes_to_chart()
        {
            // for (auto &s : slices.audio_files)
            //    Log::Logf("Track %d: %s\n", s.first, s.second.c_str());
            for (const auto& lane : notes)
            {
                for (auto note : lane.second)
                {
                    if (lane.first == -1) // lane # is bgm
                    {
                        // Log::Logf("Add BGM track at %f (%f/%f) wav: %d slices: %d\n", time_for_obj(note.first), note.first, note.first * resolution, note.second.sound, slices.slices[note.second.sound].size());
                        for (auto &s : slices.slices[note.second.sound])
                        {
                            // Log::Logf("\tTrack %d Start/End: %f/%f\n", s.first, s.second.Start, s.second.End);
                        }
                        chart->transient->bgm_events.emplace_back(AutoplaySound(time_for_obj(note.first), note.second.sound));
                        continue;
                    }

                    NoteData new_note;
                    new_note.start = time_for_obj(note.first);
                    if (note.second.length)
                    {
                        new_note.end_time = time_for_obj(note.first + note.second.length);
                    }

                    new_note.sound = note.second.sound;

                    auto Measure = measure_for_beat(note.first);
                    if (Measure >= chart->transient->measures.size())
                        chart->transient->measures.resize(Measure + 1);
                    chart->transient->measures[measure_for_beat(note.first)].notes[lane.first].push_back(new_note);

                    chart->duration = std::max(std::max(chart->duration, new_note.start), new_note.end_time);
                }
            }
        }

        void load()
        {
            chart = std::make_unique<otoworm::Chart>();
            chart->meta.emplace();
            chart->transient = std::make_shared<ChartTransient>();
            chart->transient->specialized_info = std::make_shared<BMSChartInfo>();
            timing_info = dynamic_cast<BMSChartInfo*>(chart->transient->specialized_info.get());
            timing_info->is_bmson = true;

            if (root["version"].is_null()) // NSE (check member to be == 1.0.0 for VERSION_1!)
                version = UNSPECIFIED_VERSION;
            else
            {
                if (root["version"].get<std::string>() == VERSION_1)
                    version = VERSION_1;
                else
                    throw BMSONException(
                        std::format(
                            "Unknown BMSON version ({})", 
                        root["version"].get<std::string>()
                        ).c_str()
                        );
            }

            load_meta();
            load_measure_lengths();
            load_timing();
            load_notes();
            add_notes_to_chart();
            chart->transient->bps = bps_from_beat_timing(timing, chart->transient->stops, chart->offset);

            chart->transient->slice_data = slices;

            load_bga();
            song->charts.push_back(std::move(chart));
        }
    };

    void load_bmson_stream(std::istream& input, ChartGroup* Out)
    {
        BMSONLoader bmson(input, Out);
        bmson.load();
    }
}
