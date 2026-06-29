#include <ChartGroup.h>

using namespace otoworm;

ChartClass ChartInfo::get_class() const
{
    return type;
}

Chart* ChartGroup::get_chart(const uint32_t i) const {
    if (i >= charts.size())
        return nullptr;
    else
        return charts.at(i).get();
}

uint8_t ChartGroup::get_chart_count() const {
	return charts.size();
}

void Chart::reset_transient()
{
    if (transient)
        transient = nullptr;

}


uint32_t ChartTransient::get_total_note_count() const {
	uint32_t cnt = 0;
	for (const auto measure : measures) {
		for (const auto & Note : measure.notes) {
			for ([[maybe_unused]] auto note : Note) {
				cnt++;
			}
		}
	}

	return cnt;
}

uint32_t ChartTransient::get_scorable_note_count() const {
	uint32_t cnt = 0;
	for (auto measure : measures) {
		for (auto & Note : measure.notes) {
			for (const auto note : Note) {
				if (note.type == NK_FAKE ||
					note.type == NK_INVISIBLE ||
					note.type == NK_MINE)
					continue;

				if (note.end_time != 0 && 
					note.type == NK_NORMAL)
					cnt += 2;
				else
					cnt++;
			}
		}
	}

	return cnt;
}

double Chart::time_for_beat(const double beat) const
{
	if (!transient)
		return 0;

	return transient->bps.time_at_integrated_value(beat);
}

double Chart::beat_at_time(const double time) const
{
	if (!transient)
		return 0;

	return transient->bps.integrate_to_time(time);
}
