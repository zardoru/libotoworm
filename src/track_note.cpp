#include <ChartGroup.h>

namespace otoworm {
    int get_fraction_kind_beat(double frac);

    TrackNote::TrackNote() {
    }

    TrackNote::TrackNote(const NoteData &data) {
        assign_note_data(data);
    }

    TrackNote::~TrackNote() {
    }

    float TrackNote::get_hold_size() const {
        return std::abs((b_pos_holdend_ - b_pos_));
    }

    float TrackNote::get_hold_end_vertical() const {
        return b_pos_holdend_;
    }

    void TrackNote::assign_note_data(const NoteData &notedata) {
        time = notedata.start;
        end_time_ = notedata.end_time;
        sound_ = notedata.sound;
        tail_sound_ = notedata.tail_sound;
        note_kind_ = notedata.type;
    }


    /* calculate the beat snap for this fraction */
    void TrackNote::assign_fraction(const double frac) {
        fraction_kind_ = get_fraction_kind_beat(frac);
    }

    void TrackNote::assign_position(const double position, const double end_position) {
        b_pos_ = position;
        b_pos_holdend_ = end_position;
    }

    bool TrackNote::is_hold() const {
        return end_time_ != 0;
    }

    float TrackNote::get_vertical() const {
        return b_pos_;
    }

    void TrackNote::add_time(const double time) {
        this->time += time;

        if (is_hold())
            end_time_ += time;
    }

    double TrackNote::get_end_time() const {
        return std::max(time, end_time_);
    }

    double TrackNote::get_start_time() const {
        return time;
    }

    float TrackNote::get_vertical_hold() const {
        return b_pos_ + get_hold_size() / 2;
    }

    uint32_t TrackNote::get_sound() const {
        return sound_;
    }

    uint32_t TrackNote::get_tail_sound() const {
        return tail_sound_;
    }

    int TrackNote::get_frac_kind() const {
        return fraction_kind_;
    }

    double &TrackNote::get_data_start_time() {
        return time;
    }

    double &TrackNote::get_data_end_time() {
        return end_time_;
    }

    uint32_t &TrackNote::get_data_sound() {
        return sound_;
    }

    uint8_t TrackNote::get_data_note_kind() const {
        return note_kind_;
    }

    uint8_t TrackNote::get_data_fraction_kind() const {
        return fraction_kind_;
    }

    bool TrackNote::is_judgable() const {
        return note_kind_ != NK_INVISIBLE && note_kind_ != NK_FAKE;
    }

    bool TrackNote::is_visible() const {
        return note_kind_ != NK_INVISIBLE;
    }
}
