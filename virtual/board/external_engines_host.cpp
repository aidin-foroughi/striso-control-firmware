// The virtual board's loaded engines (src/synth/synth_external.h): every engine component
// (awva/component/include/nsc/nsc.h) in sv_options.engine_dir, loaded at the synth's first look
// (its voice list at boot), in file name order, after the compiled-in engines. A library that
// doesn't load, isn't a component of this ABI, or has more voices than the board's is skipped.
// Each gets its regions from the heap (this is the PC) and is freed with the board.
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "nsc/load.h"
#include "synth_external.h"
#include "fw_api.h"
#include "sv_host.h"
#include "synth.h"

namespace {

struct Loaded {
	nsc_library lib;
	nsc_instance in;
	std::vector<strisy::ParamDesc> params;
	std::vector<float> values;
	SynthExternal x;
};

std::vector<Loaded*> loaded;
bool scanned = false;

void scan() {
	scanned = true;
	if (!svh_shared || svh_shared->engine_dir[0] == '\0') return;
	namespace fs = std::filesystem;
	std::error_code ec;
	std::vector<fs::path> libs;
	for (const auto& f : fs::directory_iterator(fs::u8path(svh_shared->engine_dir), ec)) {
		const std::string ext = f.path().extension().string();
		if (ext == ".dll" || ext == ".dylib" || ext == ".so") libs.push_back(f.path());
	}
	std::sort(libs.begin(), libs.end());
	for (const fs::path& p : libs) {
		auto* l = new Loaded{};
		if (nsc_load(p.string().c_str(), &l->lib) != 0 || l->lib.c->desc->max_voices < VOICECOUNT ||
		    nsc_instance_new(l->lib.c, &l->in) != 0) {
			nsc_unload(&l->lib);
			delete l;
			continue;
		}
		const nsc_desc& d = *l->lib.c->desc;
		for (int i = 0; i < d.num_params; i++)
			l->params.push_back({d.params[i].name, d.params[i].group, d.params[i].min, d.params[i].max,
			    static_cast<strisy::ParamKind>(d.params[i].kind)});
		l->values.assign(static_cast<size_t>(d.num_params) + 1, 0.0f);
		l->x = {l->lib.c, l->in.state, l->in.fast, l->in.large, l->params.data(), l->values.data()};
		loaded.push_back(l);
	}
}

} // namespace

int synth_external_count(void) {
	if (!scanned) scan();
	return static_cast<int>(loaded.size());
}

SynthExternal* synth_external(int i) {
	if (!scanned) scan();
	return i >= 0 && i < static_cast<int>(loaded.size()) ? &loaded[static_cast<size_t>(i)]->x : nullptr;
}

extern "C" void svh_external_free(void) {
	for (Loaded* l : loaded) {
		nsc_instance_free(&l->in);
		nsc_unload(&l->lib);
		delete l;
	}
	loaded.clear();
	scanned = false;
}
