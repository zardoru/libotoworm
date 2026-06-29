#pragma once

#include <functional>

class RowifiedChart
{
public:
    struct Event
    {
        IFraction sect;
        uint32_t evt;
    };

    struct Measure
    {
        std::vector<Event> objects[otoworm::MAX_CHANNELS];
        std::vector<Event> ln_objects[otoworm::MAX_CHANNELS];
        std::vector<Event> bgm_events;
    };

private:
    bool quantizing;
    std::vector<double> measure_start_beat;

protected:
    std::function <double(double)> quantize_function;
    static int get_row_count(const std::vector<Event> &event_list);

    void calculate_measure_start_beat();
    IFraction get_fraction_from_beat(int measure, double beat) const;

    int get_measure_from_beat(double beat);
    void update_measure_size(size_t new_size);

    void process_bgm_events();
    void process_measures();

    std::vector<Measure> measures_;
    TimingData bps;

    otoworm::Chart *parent{};

    RowifiedChart(otoworm::Chart *source, bool quantize, bool calculate_all);

    friend class otoworm::ChartGroup;
public:
    double get_start_bpm() const;
    bool is_quantizing_enabled() const;
};

void convert_to_om(otoworm::ChartGroup *sng, std::filesystem::path path_out, std::string author); /* pathout is a directory */
void convert_to_bms(otoworm::ChartGroup *sng, std::filesystem::path path_out);  /* pathout is a directory */
void convert_to_sm_timing(otoworm::ChartGroup *sng, std::filesystem::path path_out); /* pathout is a file */
void convert_to_nps_graph(otoworm::ChartGroup *sng, std::filesystem::path path_out); /* pathout is a directory */
void export_to_bms_unquantized(otoworm::ChartGroup* source, std::filesystem::path path_out);