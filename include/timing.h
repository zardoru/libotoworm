#pragma once

#include <string>
#include <vector>

namespace otoworm {

template <class T, class U>
struct TimedEvent
{
    U time;

    inline bool operator< (const T &rhs)
    {
        return time < rhs.time;
    }

    inline bool operator>(const T &rhs)
    {
        return time > rhs.time;
    }

    inline bool operator<(const U &rhs)
    {
        return time < rhs;
    }

    inline bool operator>(const U &rhs)
    {
        return time > rhs;
    }

    inline bool compareSegment (const U &lhs, const T &rhs)
    {
        return lhs < rhs.time;
    }

    inline bool compareTime(const T &lhs, const U &rhs)
    {
        return lhs.time < rhs;
    }

    operator U() const
    {
        return time;
    }

    TimedEvent(U val) : time(val) {};
    TimedEvent() = default;
};

struct TimingSegment : TimedEvent < TimingSegment, double >
{
    double value; // in bpm
    TimingSegment(double T, double V) : TimedEvent(T), value(V) {};
    TimingSegment() : TimingSegment(0, 0) {};
};

class TimingData : public std::vector<TimingSegment>
{
public:
    using std::vector<TimingSegment>::vector;

    /* fw: assumes sorted */
    size_t section_index(double point) const;
    double section_value(double point) const;
    double integrate_beats_to_seconds(double offset, double beat, bool abs = false) const;
    double integrate_to_time(double time) const;
    double time_at_integrated_value(double value) const;
    double elapsed_stop_time_at_beat(double beat) const;
    void changes_in_interval(double point_a, double point_b, TimingData& out) const;

    /* serialization */
    void load_list(std::string line, bool allow_zeros = false);
};

}
