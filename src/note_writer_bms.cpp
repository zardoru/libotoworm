#include <format>
#include <fstream>
#include <sstream>
#include <cassert>
#include <filesystem>
#include "rmath.h"
#include "text_and_file_util.h"
#include <ChartGroup.h>
#include <cmath>
#include <converter.h>

class BMSConverter : public RowifiedChart {
	otoworm::ChartGroup *song_;

	std::stringstream out_file_;

	struct TimingMeasure {
		std::vector<Event> bpm_events;
		std::vector<Event> stop_events;
		std::vector<Event> scroll_events;
	};

	std::vector<TimingMeasure> timing_measures_;
	std::vector<double> bpms_;
	std::vector<int> stops_;
	std::vector<double> scrolls_;

	void resize_timing_measures(const size_t new_max_index) {
		if (timing_measures_.size() < new_max_index + 1) {
			timing_measures_.resize(new_max_index + 1);
		}
	}

	template <class T>
	uint32_t get_index_for_value(std::vector<T> &vec, T value)
	{
		bool found = false;
		uint32_t index = 0;
		for (size_t i = 0; i < vec.size(); i++) {
			if (vec[i] == value) {
				index = i;
				found = true;
				break;
			}
		}

		// Didn't find it. Push it.
		if (!found) {
			vec.push_back(value);
			index = vec.size() - 1;
		}

		return index;
	}

	void process_bpm_events()
	{
		/*
		Group all different bpm changes that are different.
		Use that index as the event value. When outputting, check if result can be represented as an integer in base 16.
		If it can, output as bpm, otherwise, as EXbpm.

		Or don't. Whatever.

		Assumptions: There are no BPS events before time 0.
		*/
		for (const auto t : bps)
		{
			assert(t.time >= 0);
			if (t.value != 0) // Not a stop.
			{
				const auto beat = quantize_function(bps.integrate_to_time(t.time));
				const auto bpm = 60 * t.value;

				// Check redundant bpms.
				const auto index = get_index_for_value(bpms_, bpm);

				// Create new event at measure.
				const auto measure_for_event = get_measure_from_beat(beat);
				resize_timing_measures(measure_for_event); // Make sure we've got space on the measures std::vector
				timing_measures_[measure_for_event].bpm_events.push_back({
					get_fraction_from_beat(measure_for_event, beat),
					index + 1
				});
			}
		}
	}

	void process_stop_events()
	{
		for (auto t = bps.begin(); t != bps.end(); ++t)
		{
			assert(t->time >= 0);
			if (t->value <= 0.0001) // A stop.
			{
				const auto restbpm = t + 1;
				const auto beat = quantize_function(bps.integrate_to_time(t->time));
				// By song processing law, any stop is followed by a restoration of the original bpm.
				const auto duration = restbpm->time - t->time;
				const auto bps_at_stop = restbpm->value;
				// We need to know how long in beats this stop lasts for bps_at_stop.
				const auto stop_bm_sdurationbeats = bps_at_stop * duration;
				// Now the duration in BMS stops..
				const int stopduration_bms = round(stop_bm_sdurationbeats * 48.0);

				// Check redundant stops.
				const auto index = get_index_for_value(stops_, stopduration_bms);

				const auto measure_for_event = get_measure_from_beat(beat);
				resize_timing_measures(measure_for_event);
				timing_measures_[measure_for_event].stop_events.push_back({
					get_fraction_from_beat(measure_for_event, beat),
					index + 1
				});
			}
		}
	}

	void process_scroll_events()
	{
		for (const auto s : parent->transient->scrolls)
		{
			const auto beat = quantize_function(bps.integrate_to_time(s.time));
			const auto index = get_index_for_value(scrolls_, s.value);

			if (beat < 0) continue;

			const auto measure_for_event = get_measure_from_beat(beat);
			resize_timing_measures(measure_for_event);
			timing_measures_[measure_for_event].scroll_events.push_back({
				get_fraction_from_beat(measure_for_event, beat),
				index + 1
			});
		}
	}


	static std::string to_base36(int n);

	void write_header()
	{
		using std::endl;
        const auto chart_name = parent->meta ? parent->meta->name : std::string();
        const auto chart_author = parent->meta ? parent->meta->author : std::string();
		//out_file << "-- " << RAINDROP_WINDOWTITLE << RAINDROP_VERSIONTEXT << " converter to BMS" << endl;
		out_file_ << "-- HEADER" << endl;
		out_file_ << "#ARTIST " << song_->artist << endl;
		out_file_ << "#TITLE " << song_->title << endl;
		out_file_ << "#MUSIC " << otoworm::locale::wstring_to_utf8(song_->song_filename.wstring()) << endl;
		out_file_ << "#OFFSET " << parent->offset << endl;
		out_file_ << "#bpm " << get_start_bpm() << endl;
		out_file_ << "#PREVIEWPOINT " << song_->preview_time << endl;
		out_file_ << "#STAGEFILE " << parent->transient->stage_file << endl;
		out_file_ << "#DIFFICULTY " << chart_name << endl;
		out_file_ << "#PREVIEW " << otoworm::locale::wstring_to_utf8(song_->song_preview_source.wstring()) << endl;
		out_file_ << "#PLAYLEVEL " << parent->level << endl;
		out_file_ << "#MAKER " << chart_author << endl;

		out_file_ << endl << "-- WAVs" << endl;
		for (const auto& i : parent->transient->sound_list) {
			out_file_ << "#WAV" << to_base36(i.first) << " " << i.second << endl;
		}

		out_file_ << endl << "-- bpms" << endl;
		for (size_t i = 0; i < bpms_.size(); i++) {
			out_file_ << "#bpm" << to_base36(i + 1) << " " << bpms_[i] << endl;
		}

		out_file_ << endl << "-- STOPs" << endl;
		for (size_t i = 0; i < stops_.size(); i++) {
			out_file_ << "#STOP" << to_base36(i + 1) << " " << stops_[i] << endl;
		}

		out_file_ << endl << "-- SCROLLs" << endl;
		for (size_t i = 0; i < scrolls_.size(); i++)
		{
			out_file_ << "#SCROLL" << to_base36(i + 1) << " " << scrolls_[i] << endl;
		}
	}

	void WriteVectorToMeasureChannel(std::vector<Event> &event_list, const int measure, const int channel, const bool allow_multiple = false)
	{
		if (event_list.empty()) return; // Nothing to write.

		const auto vec_lcm = get_row_count(event_list);
		sort(event_list.begin(), event_list.end(), [](const Event& a, const Event&b)
			-> bool {
			return (static_cast<double>(a.sect.num) / a.sect.den) < (static_cast<double>(b.sect.num) / b.sect.den);
		});

		// first of the pair is numerator aka row given veclcm as a denominator
		// second is event at numerator aka row
		std::map<int, std::vector<int>> rowified;

		// simultaneous event count
		size_t linecount = 0;

		// Now that we have the LCM we can easily just place the objects exactly as we want to output them.
		for (const auto &obj : event_list) {

			// We convert to a numerator that fits with the LCM.
			auto new_numerator = obj.sect.num * vec_lcm / obj.sect.den;
			auto &it = rowified[new_numerator];

			if (!it.size() || allow_multiple) {
				it.push_back(obj.evt);

				// max amount of simultaneous events are given by the largest vector
				linecount = std::max(linecount, it.size());
			}
		}

		std::vector<std::stringstream> lines(linecount);

		// add the tag to all lines.
		for (size_t i = 0; i < linecount; i++) {
			auto &line = lines[i];
			line << std::format("#{:03}{}:", measure, to_base36(channel));
		}

		if (rowified.find(0) == rowified.end())
			rowified[0].resize(0);

		for (auto row = rowified.begin(); row != rowified.end(); row++) {

			// ith line gets the ith column at fraction row->first / LCM
			for (size_t i = 0; i < linecount; i++) {
				auto &line = lines[i];

				if (i < row->second.size()) {
					line << to_base36(row->second[i]);
				}
				else
					line << "00";
			}

			size_t next_row;
			auto next = row;
			next++;

			if (next != rowified.end()) {
				next_row = next->first;
			}
			else {
				next_row = vec_lcm;
			}

			// don't count the destination row itself (take 1)
			const size_t zero_fill = next_row - row->first - 1;
			for (size_t i = 0; i < zero_fill; i++) {
				for (auto &line : lines) {
					line << "00";
				}
			}

		}

		for (auto &line : lines) {
			out_file_ << line.str();
			out_file_ << std::endl;
		}
	}

	int GetChannel(int channel) const;

	int GetLNChannel(int channel) const;

	void WriteMeasures()
	{
		uint32_t measure = 0;
		using std::endl;
		for (auto m : measures){
			if (parent->transient->measures[measure].length != 4)
			{
				double bms_length = parent->transient->measures[measure].length / 4;
				out_file_ << std::format("#{:03}02:{}", measure, bms_length) << endl;
			}

			out_file_ << "-- BGM - Measure " << measure << endl;
			WriteVectorToMeasureChannel(m.bgm_events, measure, 1, true);

			if (measure < timing_measures_.size()) {

				if (!timing_measures_[measure].bpm_events.empty()) {
					out_file_ << "-- bpm" << endl;
					WriteVectorToMeasureChannel(timing_measures_[measure].bpm_events, measure, 8); // lol just exbpm. who cares anyway
				}

				if (!timing_measures_[measure].stop_events.empty()) {
					out_file_ << "-- STOPS" << endl;
					WriteVectorToMeasureChannel(timing_measures_[measure].stop_events, measure, 9);
				}

				if (!timing_measures_[measure].scroll_events.empty())
				{
					out_file_ << "-- SCROLLS" << endl;
					WriteVectorToMeasureChannel(timing_measures_[measure].scroll_events, measure, b36toi("SC"));
				}
			}

			out_file_ << "-- OBJ" << endl;
			for (int i = 0; i < parent->channels; i++)
			{
				WriteVectorToMeasureChannel(m.objects[i], measure, GetChannel(i));
				WriteVectorToMeasureChannel(m.ln_objects[i], measure, GetLNChannel(i));
			}

			measure++;
		}
	}

	void WriteBMSOutput()
	{
		write_header();
		WriteMeasures();
	}

public:

	BMSConverter(const bool quantize, otoworm::Chart *source, otoworm::ChartGroup *song)
		: RowifiedChart(source, quantize, true)
	{
		this->song_ = song;

		process_bpm_events();
		process_stop_events();
		process_scroll_events();
	}

	void Output(std::filesystem::path path_out)
	{
		std::filesystem::path name = path_out;

		if (std::filesystem::is_directory(path_out) && std::filesystem::exists(path_out)) {
            const auto chart_name = parent->meta ? parent->meta->name : std::string();
            const auto chart_author = parent->meta ? parent->meta->author : std::string();
		    name = name / std::format("{} ({}) - {}.bms",
                          song_->title, chart_name, chart_author);
		}


		std::ofstream out(name.string());

		if (!out.is_open())
		{
			auto s = std::format("failed to open file {}", otoworm::locale::wstring_to_utf8(name.wstring()));
			throw std::runtime_error(s.c_str());
		}

		if (bps.empty())
			throw std::runtime_error("There are no timing points!");

		WriteBMSOutput();
		out << out_file_.str();
    }
};

std::string BMSConverter::to_base36(int num)
{
    const char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    int i;
    char buf[66];

    /* if num is zero */
    if (!num)
        return "00";

	if (num > 1295) // ZZ in b36
		throw std::runtime_error("EventList of range number for BMS conversion");

    buf[65] = '\0';
    i = 65;

	// az simplification: only positive values
	while (num) { /* until num is 0... */
		/* go left 1 digit, divide by radix, and set digit to remainder */
		buf[--i] = digits[num % 36];
		num /= 36;
	}

	if (i == 64) // 64 only one digit?
		buf[i - 1] = '0';

    return &buf[63]; // 63, 64, 65 (two digits + null terminator indices)
}

int BMSConverter::GetChannel(const int channel) const
{
	switch (channel) {
	case 0:
		return 42; // scratch
	case 1:
		return 37;
	case 2:
		return 38;
	case 3:
		return 39;
	case 4:
		return 40;
	case 5:
		return 41;
	case 6:
		return 44;
	case 7:
		return 45;
	default:
		return GetChannel(channel - 8) - 37 + 73; // move to 2nd side...
	}
}

int BMSConverter::GetLNChannel(const int channel) const
{
	switch (channel) {
	case 0:
		return 186; // scratch
	case 1:
		return 181;
	case 2:
		return 182;
	case 3:
		return 183;
	case 4:
		return 184;
	case 5:
		return 185;
	case 6:
		return 188;
	case 7:
		return 189;
	default:
		return GetLNChannel(channel - 8) - 181 + 217; // move to 2nd side...
	}
}

void ConvertBMSAll(otoworm::ChartGroup *source, std::filesystem::path path_out, const bool quantize)
{
    for (auto &diff : source->charts)
    {
        BMSConverter conv(quantize, diff.get(), source);
        conv.Output(path_out);
    }
}

void export_to_bms(otoworm::ChartGroup* source, std::filesystem::path path_out)
{
    ConvertBMSAll(source, path_out, true);
}

void export_to_bms_unquantized(otoworm::ChartGroup* source, std::filesystem::path path_out)
{
    ConvertBMSAll(source, path_out, false);
}
