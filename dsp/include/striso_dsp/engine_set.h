// EngineSet<E...>: a fixed set of VoiceEngines chosen at compile time (the board's VOICE=all
// firmware runs one of them at a time), as three plain-data objects the host places itself:
//
//   EngineSet<...>::States   every engine object               (Placement::tcm:   the board's DTCM)
//   EngineSet<...>::Fast     one FastMemory region they share  (Placement::fast:  AHB SRAM1/2)
//   EngineSet<...>::Large    one LargeMemory region they share (Placement::large: AXI SRAM)
//
// Only one engine runs at a time, so the engines' memory regions are one arena per region class,
// as large as the largest engine's region: an engine's FastMemory and LargeMemory are the start of
// the arena while it runs. start(k, ...) hands the arenas to engine k: its share of each is zeroed
// (the state at power on, whatever ran before), then its init() runs. The engine objects stay
// separate (their parameters persist across switches).
//
// visit(k, f): f(engine k) with engine k's own type, as a chain of compares the compiler inlines
// (no function pointers: the board's stack check follows every call).
#ifndef STRISO_DSP_ENGINE_SET_H_
#define STRISO_DSP_ENGINE_SET_H_

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <new>
#include <utility>

#include "engine.h"

namespace striso::dsp {

namespace detail {

template <size_t I, class T> struct Slot {
	STRISO_NO_UNIQUE_ADDRESS T value;
};
template <class Seq, class... T> struct Slots;
template <size_t... I, class... T> struct Slots<std::index_sequence<I...>, T...> : Slot<I, T>... {};

template <size_t I, class T, class... R> struct Nth {
	using type = typename Nth<I - 1, R...>::type;
};
template <class T, class... R> struct Nth<0, T, R...> {
	using type = T;
};

} // namespace detail

// a plain-data tuple (an aggregate: zero-filled storage is a valid value, no constructor runs)
template <class... T> struct Plain : detail::Slots<std::index_sequence_for<T...>, T...> {
	template <size_t I> using type = typename detail::Nth<I, T...>::type;
	template <size_t I> [[nodiscard]] type<I>& get() { return static_cast<detail::Slot<I, type<I>>&>(*this).value; }
	template <size_t I> [[nodiscard]] const type<I>& get() const {
		return static_cast<const detail::Slot<I, type<I>>&>(*this).value;
	}
};

// One memory region the engines take turns in: Bytes of storage, aligned for every region type it
// holds. Plain data (zero-filled storage is valid). as<M>() is the region of type M at its start.
template <size_t Bytes, size_t Align> struct Arena {
	static constexpr size_t size = Bytes;
	static constexpr size_t align = Align;
	alignas(Align) std::byte bytes[Bytes];

	template <class M> static constexpr bool holds() { return sizeof(M) <= Bytes && alignof(M) <= Align; }

	template <class M> [[nodiscard]] M& as() {
		static_assert(holds<M>(), "an engine's memory region doesn't fit the arena");
		// the arena's bytes provide the storage; M is plain data (an implicit-lifetime type)
		return *std::launder(reinterpret_cast<M*>(bytes));
	}
	// the state at power on for a region of type M: its bytes zeroed
	template <class M> void clear() { std::memset(bytes, 0, sizeof(M)); }
};

// the arena for every region type M...: as large and as aligned as the largest
template <class... M> using SharedArena = Arena<std::max({sizeof(M)...}), std::max({alignof(M)...})>;

// Fits<Arena, Engine, Region>::value: a check that names the engine (and the region) in the
// compiler's message when its region doesn't fit an arena of a fixed size
template <class A, class Engine, class Region> struct Fits {
	static constexpr bool value = A::template holds<Region>();
	static_assert(value, "this engine's region (the template arguments: arena, engine, region) is larger than the arena");
};

template <VoiceEngine... E> struct EngineSet {
	static constexpr int count = sizeof...(E);
	template <size_t I> using engine = typename detail::Nth<I, E...>::type;

	using States = Plain<E...>;
	using Fast = SharedArena<typename E::FastMemory...>;
	using Large = SharedArena<typename E::LargeMemory...>;
	static_assert((Fits<Fast, E, typename E::FastMemory>::value && ...));
	static_assert((Fits<Large, E, typename E::LargeMemory>::value && ...));

	// f(engine k) for 0 <= k < count (nothing otherwise); f gets the engine and its index as a
	// std::integral_constant
	template <class F> static void visit(int k, States& s, F&& f) {
		visit_(k, s, f, std::index_sequence_for<E...>());
	}
	// the same with the engine's memory regions in the arenas: f(engine, fast, large, index).
	// Only for the engine the arenas were started for (start below).
	template <class F> static void visit(int k, States& s, Fast& fast, Large& large, F&& f) {
		visit_mem_(k, s, fast, large, f, std::index_sequence_for<E...>());
	}
	// engine k takes over the arenas: its regions zeroed, then init(engine, fast, large, index)
	// (the engine's init with them). Call it where the engines run (the synth thread): the engine
	// that ran before must not touch the arenas again.
	template <class F> static void start(int k, States& s, Fast& fast, Large& large, F&& init) {
		visit_mem_(k, s, fast, large, init, std::index_sequence_for<E...>(), true);
	}

	// EngineInfo of engine k (nullptr outside)
	[[nodiscard]] static const EngineInfo* info(int k) {
		static constexpr const EngineInfo* infos[] = {&E::info...};
		return k >= 0 && k < count ? infos[k] : nullptr;
	}

private:
	template <class F, size_t... I> static void visit_(int k, States& s, F& f, std::index_sequence<I...>) {
		(void)((k == static_cast<int>(I) && (f(s.template get<I>(), std::integral_constant<size_t, I>()), true)) || ...);
	}
	template <size_t I, class F> static void with_mem_(States& s, Fast& fast, Large& large, F& f, bool clear) {
		using FastM = typename engine<I>::FastMemory;
		using LargeM = typename engine<I>::LargeMemory;
		if (clear) {
			fast.template clear<FastM>();
			large.template clear<LargeM>();
		}
		f(s.template get<I>(), fast.template as<FastM>(), large.template as<LargeM>(), std::integral_constant<size_t, I>());
	}
	template <class F, size_t... I>
	static void visit_mem_(int k, States& s, Fast& fast, Large& large, F& f, std::index_sequence<I...>, bool clear = false) {
		(void)((k == static_cast<int>(I) && (with_mem_<I>(s, fast, large, f, clear), true)) || ...);
	}
};

} // namespace striso::dsp

#endif // STRISO_DSP_ENGINE_SET_H_
