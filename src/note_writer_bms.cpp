#include <format>
#include <fstream>
#include <sstream>
#include <cassert>
#include <filesystem>
#include "rmath.h"
#include "text_and_file_util.h"
#include <ChartGroup.h>
#include <converter.h>

class BMSConverter : public RowifiedChart {
	otoworm::ChartGroup *song;

	std::stringstream out_file;

	struct TimingMeasure {
		std::vector<Event> bpm_events;
		std::vector<Event> stop_events;
		std::vector<Event> scroll_events;
	};

	std::vector<TimingMeasure> timing_measures;
	std::vector<double> bpms;
	std::vector<int> stops;
	std::vector<double> scrolls;

	void resize_timing_measures(const size_t new_max_index) {
		if (timing_measures.size() < new_max_index + 1) {
			timing_measures.resize(new_max_index + 1);
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
		for (const auto T : bps)
		{
			assert(T.time >= 0);
			if (T.value != 0) // Not a stop.
			{
				const auto beat = quantize_function(bps.integrate_to_time(T.time));
				const auto bpm = 60 * T.value;

				// Check redundant bpms.
				const auto index = get_index_for_value(bpms, bpm);

				// Create new event at measure.
				const auto measure_for_event = get_measure_from_beat(beat);
				resize_timing_measures(measure_for_event); // Make sure we've got space on the measures std::vector
				timing_measures[measure_for_event].bpm_events.push_back({ 
					get_fraction_from_beat(measure_for_event, beat),
					index + 1 
				});
			}
		}
	}

	void process_stop_events()
	{
		for (auto T = bps.begin(); T != bps.end(); ++T)
		{
			assert(T->time >= 0);
			if (T->value <= 0.0001) // A stop.
			{
				const auto Restbpm = T + 1;
				const auto beat = quantize_function(bps.integrate_to_time(T->time));
				// By song processing law, any stop is followed by a restoration of the original bpm.
				const auto duration = Restbpm->time - T->time;
				const auto bps_at_stop = Restbpm->value;
				// We need to know how long in beats this stop lasts for bps_at_stop.
				const auto StopBMSdurationbeats = bps_at_stop * duration;
				// Now the duration in BMS stops..
				const int StopdurationBMS = round(StopBMSdurationbeats * 48.0);

				// Check redundant stops.
				const auto index = get_index_for_value(stops, StopdurationBMS);

				const auto measure_for_event = get_measure_from_beat(beat);
				resize_timing_measures(measure_for_event);
				timing_measures[measure_for_event].stop_events.push_back({ 
					get_fraction_from_beat(measure_for_event, beat),
					index + 1 
				});
			}
		}
	}

	void process_scroll_events()
	{
		for (const auto S : parent->transient->scrolls)
		{
			const auto beat = quantize_function(bps.integrate_to_time(S.time));
			const auto index = get_index_for_value(scrolls, S.value);

			if (beat < 0) continue;

			const auto measure_for_event = get_measure_from_beat(beat);
			resize_timing_measures(measure_for_event);
			timing_measures[measure_for_event].scroll_events.push_back({
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
		out_file << "-- HEADER" << endl;
		out_file << "#ARTIST " << song->artist << endl;
		out_file << "#TITLE " << song->title << endl;
		out_file << "#MUSIC " << otoworm::locale::wstring_to_utf8(song->song_filename.wstring()) << endl;
		out_file << "#OFFSET " << parent->offset << endl;
		out_file << "#bpm " << get_start_bpm() << endl;
		out_file << "#PREVIEWPOINT " << song->preview_time << endl;
		out_file << "#STAGEFILE " << parent->transient->stage_file << endl;
		out_file << "#DIFFICULTY " << chart_name << endl;
		out_file << "#PREVIEW " << otoworm::locale::wstring_to_utf8(song->song_preview_source.wstring()) << endl;
		out_file << "#PLAYLEVEL " << parent->level << endl;
		out_file << "#MAKER " << chart_author << endl;

		out_file << endl << "-- WAVs" << endl;
		for (const auto& i : parent->transient->sound_list) {
			out_file << "#WAV" << to_base36(i.first) << " " << i.second << endl;
		}

		out_file << endl << "-- bpms" << endl;
		for (size_t i = 0; i < bpms.size(); i++) {
			out_file << "#bpm" << to_base36(i + 1) << " " << bpms[i] << endl;
		}

		out_file << endl << "-- STOPs" << endl;
		for (size_t i = 0; i < stops.size(); i++) {
			out_file << "#STOP" << to_base36(i + 1) << " " << stops[i] << endl;
		}

		out_file << endl << "-- SCROLLs" << endl;
		for (size_t i = 0; i < scrolls.size(); i++)
		{
			out_file << "#SCROLL" << to_base36(i + 1) << " " << scrolls[i] << endl;
		}
	}

	void WriteVectorToMeasureChannel(std::vector<Event> &EventList, const int Measure, const int Channel, const bool AllowMultiple = false)
	{
		if (EventList.empty()) return; // Nothing to write.

		const auto VecLCM = get_row_count(EventList);
		sort(EventList.begin(), EventList.end(), [](const Event& A, const Event&B)
			-> bool {
			return (static_cast<double>(A.sect.Num) / A.sect.den) < (static_cast<double>(B.sect.Num) / B.sect.den);
		});

		// first of the pair is numerator aka row given veclcm as a denominator
		// second is event at numerator aka row
		std::map<int, std::vector<int>> rowified;

		// simultaneous event count
		size_t linecount = 0;

		// Now that we have the LCM we can easily just place the objects exactly as we want to output them.
		for (const auto &Obj : EventList) {

			// We convert to a numerator that fits with the LCM.
			auto newNumerator = Obj.sect.Num * VecLCM / Obj.sect.den;
			auto &it = rowified[newNumerator];

			if (!it.size() || AllowMultiple) {
				it.push_back(Obj.evt);

				// max amount of simultaneous events are given by the largest vector
				linecount = std::max(linecount, it.size());
			}
		}

		std::vector<std::stringstream> lines(linecount);

		// add the tag to all lines.
		for (size_t i = 0; i < linecount; i++) {
			auto &line = lines[i];
			line << std::format("#{:03}{}:", Measure, to_base36(Channel));
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
				next_row = VecLCM;
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
			out_file << line.str();
			out_file << std::endl;
		}
	}

	int GetChannel(int channel) const;

	int GetLNChannel(int channel) const;

	void WriteMeasures()
	{
		uint32_t Measure = 0;
		using std::endl;
		for (auto M : measures_){
			if (parent->transient->measures[Measure].length != 4)
			{
				double bmsLength = parent->transient->measures[Measure].length / 4;
				out_file << std::format("#{:03}02:{}", Measure, bmsLength) << endl;
			}

			out_file << "-- BGM - Measure " << Measure << endl;
			WriteVectorToMeasureChannel(M.bgm_events, Measure, 1, true);

			if (Measure < timing_measures.size()) {

				if (!timing_measures[Measure].bpm_events.empty()) {
					out_file << "-- bpm" << endl;
					WriteVectorToMeasureChannel(timing_measures[Measure].bpm_events, Measure, 8); // lol just exbpm. who cares anyway
				}

				if (!timing_measures[Measure].stop_events.empty()) {
					out_file << "-- STOPS" << endl;
					WriteVectorToMeasureChannel(timing_measures[Measure].stop_events, Measure, 9);
				}

				if (!timing_measures[Measure].scroll_events.empty())
				{
					out_file << "-- SCROLLS" << endl;
					WriteVectorToMeasureChannel(timing_measures[Measure].scroll_events, Measure, b36toi("SC"));
				}
			}

			out_file << "-- OBJ" << endl;
			for (int i = 0; i < parent->channels; i++)
			{
				WriteVectorToMeasureChannel(M.objects[i], Measure, GetChannel(i));
				WriteVectorToMeasureChannel(M.ln_objects[i], Measure, GetLNChannel(i));
			}

			Measure++;
		}
	}

	void WriteBMSOutput()
	{
		write_header();
		WriteMeasures();
	}

public:

	BMSConverter(const bool Quantize, otoworm::Chart *Source, otoworm::ChartGroup *song)
		: RowifiedChart(Source, Quantize, true)
	{
		this->song = song;

		process_bpm_events();
		process_stop_events();
		process_scroll_events();
	}

	void Output(std::filesystem::path PathOut)
	{
		std::filesystem::path name = PathOut;

		if (std::filesystem::is_directory(PathOut) && std::filesystem::exists(PathOut)) {
            const auto chart_name = parent->meta ? parent->meta->name : std::string();
            const auto chart_author = parent->meta ? parent->meta->author : std::string();
		    name = name / std::format("{} ({}) - {}.bms",
                          song->title, chart_name, chart_author);
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
		out << out_file.str();        
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

void ConvertBMSAll(otoworm::ChartGroup *Source, std::filesystem::path PathOut, const bool Quantize)
{
    for (auto &Diff : Source->charts)
    {
        BMSConverter Conv(Quantize, Diff.get(), Source);
        Conv.Output(PathOut);
    }
}

void export_to_bms(otoworm::ChartGroup* Source, std::filesystem::path PathOut)
{
    ConvertBMSAll(Source, PathOut, true);
}

void export_to_bms_unquantized(otoworm::ChartGroup* Source, std::filesystem::path PathOut)
{
    ConvertBMSAll(Source, PathOut, false);
}
