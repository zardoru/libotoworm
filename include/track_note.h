#pragma once

#include "timing.h"

namespace otoworm {
    // REMEMBER WE MUST BE 8-BYTE ALIGNED. It's either 16, 24 or 32...
    // note data: 28 bytes
    // Going to be 32 bytes on its own with alignment...
        struct NoteData
        {
            // start time and end time are 8 bytes each - 16 bytes.
            double start, end_time;
            uint32_t sound; // Do we really need more than this?
            uint32_t tail_sound;
            uint8_t type; // To be used with ENoteKind.
            // 7 bytes wasted

            NoteData()
            {
                start = end_time = 0;
                sound = tail_sound = 0;
                type = NK_NORMAL;
            }

            NoteData(double start_time, double end_time) : NoteData() {
                start = start_time;
                this->end_time = end_time;
            }
        };

		class TrackNote : public TimedEvent<TrackNote, double>
		{
            // 16 bytes (Implied 8 with inherited TimedEvent)
			double end_time_;

			// 16 bytes
			double b_pos_;
			double b_pos_holdend_;

			// 8 bytes
			uint32_t sound_;
			uint32_t tail_sound_;

			// 3 bytes
			uint8_t note_kind_; // To be used with ENoteKind.
			uint8_t fraction_kind_;

			// 48 bytes aligned
		public:
			TrackNote();
			explicit TrackNote(const NoteData &data);

			// Build this tracknote from this NoteData.
			void assign_note_data(const NoteData &data);

			// az: These return by references are completely useless and kinda stupid.

			// Get the start time.
			double &get_data_start_time();

			// Get the actual end time regardless of start time.
			double &get_data_end_time();

			// Get what sound this note has. (Redundant?)
			uint32_t &get_data_sound();

			// Get the kind of head this note has.
			uint8_t  get_data_note_kind() const;

			// Get what fraction of a beat this note belongs to.
			uint8_t  get_data_fraction_kind() const;

			// Set this note's position on the vertical track.
			void assign_position(double position, double end_position = 0);

			// Assign a fraction of a beat to this note.
			void assign_fraction(double frc); // frc = fraction of a beat

			// Add this much drift to the note. Doesn't reset.
			void add_time(double time);

			// Get the position on the track of the note/hold head.
			float get_vertical() const;

			// Get the position on the track of the hold's tail.
			float get_vertical_hold() const;

			// Get whether this note is a hold.
			bool is_hold() const;

			// Get whether this note is a judgable kind of note. Doesn't depend on failure state.
			// It also doesn't apply mechanical rules (i.e. hit notes can't be judged twice)
			bool is_judgable() const;

			// Get whether this note should be drawn - independant of whether it was hit.
			bool is_visible() const;

			// Get the sound associated to this note.
			uint32_t get_sound() const;

			// Get the sound associated to this note's tail.
			uint32_t get_tail_sound() const;

			// Get the maximum between the start and end times. If not a hold, Start = End.
			double get_end_time() const;

			// Get the start time of this note. If not a hold, Start = End.
			double get_start_time() const;

			// Get the beat fraction kind.
			int get_frac_kind() const;

			~TrackNote();

			// Get the size of this hold on the unmodified track.
			float get_hold_size() const;

			// Get the position of the tail of this hold on the unmodified track.
			float get_hold_end_vertical() const;
		};

		inline bool operator<(const TrackNote& tn, double time)
		{
			return tn.get_start_time() < time;
		}
}
