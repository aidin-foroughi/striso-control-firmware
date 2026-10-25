// EngineSet<E...>: a fixed set of VoiceEngines chosen at compile time (the board's VOICE=all
// firmware runs one of them at a time), as three plain-data objects the host places itself:
//
//   EngineSet<...>::States   every engine object           (Placement::tcm:   the board's DTCM)
//   EngineSet<...>::Fast     every engine's FastMemory     (Placement::fast:  AHB SRAM1/2)
//   EngineSet<...>::Large    every engine's LargeMemory    (Placement::large: AXI SRAM)
//
// and visit(k, f): f(engine k) with engine k's own type, as a chain of compares the compiler
// inlines (no function pointers: the board's stack check follows every call).
#ifndef STRISO_DSP_ENGINE_SET_H_
#define STRISO_DSP_ENGINE_SET_H_

#include <cstddef>
#include <utility>

#include "engine.h"

namespace striso::dsp {

namespace detail {

template <size_t I, class T> struct Slot {
	[[no_unique_address]] T value;
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

template <VoiceEngine... E> struct EngineSet {
	static constexpr int count = sizeof...(E);
	template <size_t I> using engine = typename detail::Nth<I, E...>::type;

	using States = Plain<E...>;
	using Fast = Plain<typename E::FastMemory...>;
	using Large = Plain<typename E::LargeMemory...>;

	// f(engine k) for 0 <= k < count (nothing otherwise); f gets the engine and its index as a
	// std::integral_constant
	template <class F> static void visit(int k, States& s, F&& f) {
		visit_(k, s, f, std::index_sequence_for<E...>());
	}
	// the same with the engine's memory regions: f(engine, fast, large, index)
	template <class F> static void visit(int k, States& s, Fast& fast, Large& large, F&& f) {
		visit_mem_(k, s, fast, large, f, std::index_sequence_for<E...>());
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
	template <class F, size_t... I>
	static void visit_mem_(int k, States& s, Fast& fast, Large& large, F& f, std::index_sequence<I...>) {
		(void)((k == static_cast<int>(I) && (f(s.template get<I>(), fast.template get<I>(), large.template get<I>(), std::integral_constant<size_t, I>()), true)) ||
		    ...);
	}
};

} // namespace striso::dsp

#endif // STRISO_DSP_ENGINE_SET_H_
