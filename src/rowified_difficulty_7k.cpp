#include <filesystem>
#include <functional>
#include <cassert>
#include "rmath.h"

#include <ChartGroup.h>
#include <ProcessedChart.h>
#include <converter.h>


#include "text_and_file_util.h"

using namespace otoworm;

static double passthrough(const double d) {
    return d;
}


RowifiedChart::RowifiedChart(Chart *source, const bool quantize, const bool calculate_all)
        : quantizing(quantize), parent(source) {
    assert(source != nullptr);

    if (quantize)
        quantize_function = [](auto && PH1) { return quantize_beat(std::forward<decltype(PH1)>(PH1)); };
    else
        quantize_function = [](auto && PH1) { return passthrough(std::forward<decltype(PH1)>(PH1)); };

    bps = ProcessedChart::from(source).bps;

    calculate_measure_start_beat();

    if (calculate_all) {
        process_bgm_events();
        process_measures();
    }
}

int RowifiedChart::get_row_count(const std::vector<Event> &event_list) {
    // literally the only hard part of this
    // We have to find the LCM of the set of fractions given by the Fraction of all objects in the vector.
    std::vector<int> denominators;

    // Find all different denominators.
    for (auto i : event_list) {
        for (const auto k : denominators) {
            if (i.sect.den == k)
                goto next_object;
        }

        denominators.push_back(i.sect.den);
        next_object:;
    }

    if (denominators.size() == 1) return denominators.at(0);

    // Now get the LCM.
    return lcm(denominators);
}

bool RowifiedChart::is_quantizing_enabled() const {
    return quantizing;
}

void RowifiedChart::calculate_measure_start_beat() {
    double accumulated_length = 0;

    assert(parent != nullptr);

    measure_start_beat.clear();
    for (const auto m : parent->transient->measures) {
        // Note: Acom. doesn't need to be quantized.
        // Only contents within measure.
        measure_start_beat.push_back(accumulated_length);
        accumulated_length += m.length;
    }
}

IFraction RowifiedChart::get_fraction_from_beat(const int measure, const double beat) const {
    const double m_start = measure_start_beat[measure];
    const double m_len = parent->transient->measures[measure].length;
    const double m_frac = (beat - m_start) / m_len;
    IFraction frac;

    if (!is_quantizing_enabled())
        frac.fromDouble(m_frac);
    else
        frac.fromDouble(quantize_fraction_measure(m_frac));

    return frac;
}

int RowifiedChart::get_measure_from_beat(const double beat) {
    const auto it = std::ranges::upper_bound(measure_start_beat, quantize_function(beat));
    auto measure = it - measure_start_beat.begin() - 1;

    if (measure >= 0) {
        const size_t m = measure; // eh, do we need more 2^31-1 measures? anyway shut up compiler
        if (m < measure_start_beat.size())
            return measure;
    }

    const auto s = std::format("Beat {} (Measure {}) outside of bounds (size = {}).",
                             beat, measure, measure_start_beat.size());
    throw std::runtime_error(s);
}

void RowifiedChart::update_measure_size(const size_t new_size) {
    if (measures_.size() < new_size + 1)
        measures_.resize(new_size + 1);
}

void RowifiedChart::process_bgm_events() {
    for (const auto bgm : parent->transient->bgm_events) {
        const double beat = quantize_function(bps.integrate_to_time(bgm.time));

        const int measure_for_event = get_measure_from_beat(beat);
        update_measure_size(measure_for_event);
        measures_[measure_for_event].bgm_events.push_back({get_fraction_from_beat(measure_for_event, beat), bgm.sound});
    }
}

void RowifiedChart::process_measures() {
    for (const auto msr : parent->transient->measures) {
        for (int chan = 0; chan < parent->channels; chan++) {
            for (const auto note : msr.notes[chan]) {
                const double start_beat = quantize_function(bps.integrate_to_time(note.start));

                if (start_beat < 0) {
                    // Log::Printf("Object at negative beat (%f), discarded\n", StartBeat);
                    continue;
                }

                if (note.end_time == 0) { // Non-hold. Emit channels 11-...
                    const int measure_for_event = get_measure_from_beat(start_beat);
                    update_measure_size(measure_for_event);

                    const auto snd = note.sound ? note.sound : 1;
                    measures_[measure_for_event].objects[chan].push_back(
                            {
                                    get_fraction_from_beat(measure_for_event, start_beat),
                                    snd
                            }
                    );
                } else { // Hold. Emit channels 51-...
                    const double end_beat = quantize_function(bps.integrate_to_time(note.end_time));
                    const int measure_for_event = get_measure_from_beat(start_beat);
                    const int measure_for_event_end = get_measure_from_beat(end_beat);
                    update_measure_size(measure_for_event_end);

                    const auto snd = note.sound ? note.sound : 1;
                    measures_[measure_for_event].ln_objects[chan].push_back(
                            {get_fraction_from_beat(measure_for_event, start_beat), snd});
                    measures_[measure_for_event_end].ln_objects[chan].push_back(
                            {get_fraction_from_beat(measure_for_event_end, end_beat), snd});
                }
            }
        }
    }
}

double RowifiedChart::get_start_bpm() const {
    return bps[0].value * 60;
}

