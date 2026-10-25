// Mono legato's note stack (awva/legato.h): last-note priority, the newest note's pitch and
// tilt, the strongest note's pressure. The firmware's use (one voice, no new attack) is
// synth_control.legato_* and control_replay.*.legato.
#include "awva/legato.h"

#include "doctest/doctest.h"

namespace {
using Signals = awva::ump::NoteSignals;
Signals held(float note, float pres, float vpres = 0.0f, float x = 0.0f) { return {note, pres, vpres, x, 0.0f, true}; }
const Signals released = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false};
} // namespace

TEST_CASE("legato: the newest held note plays, a release returns to the newest still held") {
	awva::voices::Legato<16> l;
	CHECK(l.empty());
	l.note(3, held(60, 0.5f));
	l.note(7, held(64, 0.2f, 0.8f, 0.3f));
	CHECK(l.top() == 7);
	Signals s = l.signals();
	CHECK(s.note == 64);
	CHECK(s.but_x == 0.3f);
	CHECK(s.pres == 0.5f);  // the stronger note's dynamic
	CHECK(s.vpres == 0.0f); // and its velocity: the new note's strike doesn't tongue the slur
	l.note(7, held(64, 0.7f, 0.1f));
	CHECK(l.signals().pres == 0.7f);
	CHECK(l.signals().vpres == 0.1f);
	l.note(9, held(67, 0.4f));
	l.note(7, released); // a middle note leaves: the newest still plays
	CHECK(l.top() == 9);
	l.note(9, held(67, 0.0f)); // pressure 0 is a release too
	CHECK(l.top() == 3);
	CHECK(l.signals().note == 60);
	l.note(3, released);
	CHECK(l.empty());
	CHECK(l.top() == -1);
}

TEST_CASE("legato: past its size the oldest note is forgotten, clear() forgets all") {
	awva::voices::Legato<4> l;
	for (int id = 0; id < 6; id++) l.note(id, held(60.0f + static_cast<float>(id), 0.5f));
	CHECK(l.size() == 4);
	CHECK(!l.held(0));
	CHECK(!l.held(1));
	CHECK(l.held(2));
	CHECK(l.top() == 5);
	l.clear();
	CHECK(l.empty());
}
