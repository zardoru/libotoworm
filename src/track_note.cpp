#include <ChartGroup.h>

namespace otoworm {
    int get_fraction_kind_beat(double frac);

    TrackNote::TrackNote() {
    }

    TrackNote::TrackNote(const NoteData &Data) {
        assign_note_data(Data);
    }

    TrackNote::~TrackNote() {
    }

    float TrackNote::get_hold_size() const {
        return std::abs((b_pos_holdend - b_pos));
    }

    float TrackNote::get_hold_end_vertical() const {
        return b_pos_holdend;
    }

    void TrackNote::assign_note_data(const NoteData &Notedata) {
        time = Notedata.start;
        end_time = Notedata.end_time;
        sound = Notedata.sound;
        tail_sound = Notedata.tail_sound;
        note_kind = Notedata.type;
    }


    /* calculate the beat snap for this fraction */
    void TrackNote::assign_fraction(const double frac) {
        fraction_kind = get_fraction_kind_beat(frac);
    }

    void TrackNote::assign_position(const double Position, const double endPosition) {
        b_pos = Position;
        b_pos_holdend = endPosition;
    }

    bool TrackNote::is_hold() const {
        return end_time != 0;
    }

    float TrackNote::get_vertical() const {
        return b_pos;
    }

    void TrackNote::add_time(const double Time) {
        this->time += Time;

        if (is_hold())
            end_time += Time;
    }

    double TrackNote::get_end_time() const {
        return std::max(time, end_time);
    }

    double TrackNote::get_start_time() const {
        return time;
    }

    float TrackNote::get_vertical_hold() const {
        return b_pos + get_hold_size() / 2;
    }

    uint32_t TrackNote::get_sound() const {
        return sound;
    }

    uint32_t TrackNote::get_tail_sound() const {
        return tail_sound;
    }

    int TrackNote::get_frac_kind() const {
        return fraction_kind;
    }

    double &TrackNote::get_data_start_time() {
        return time;
    }

    double &TrackNote::get_data_end_time() {
        return end_time;
    }

    uint32_t &TrackNote::get_data_sound() {
        return sound;
    }

    uint8_t TrackNote::get_data_note_kind() const {
        return note_kind;
    }

    uint8_t TrackNote::get_data_fraction_kind() const {
        return fraction_kind;
    }

    bool TrackNote::is_judgable() const {
        return note_kind != NK_INVISIBLE && note_kind != NK_FAKE;
    }

    bool TrackNote::is_visible() const {
        return note_kind != NK_INVISIBLE;
    }
}
