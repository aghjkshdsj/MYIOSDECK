// SPDX-License-Identifier: GPL-3.0-or-later
// MYIOSDECK's C API over innoextract (App/Sources/Native/inno_unpack.h): inspect a GOG offline
// installer and unpack its game files on the phone. innoextract does all the reading; this file
// sets its options the way `innoextract -e -m -I /app` would, collects its log lines and reports
// progress (both through the hooks myiosdeck.patch adds).

#include "inno_unpack.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ios>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include "cli/extract.hpp"
#include "cli/gog.hpp"
#include "loader/offsets.hpp"
#include "setup/data.hpp"
#include "setup/file.hpp"
#include "setup/filename.hpp"
#include "setup/header.hpp"
#include "setup/info.hpp"
#include "setup/language.hpp"
#include "setup/version.hpp"
#include "stream/slice.hpp"
#include "util/boostfs_compat.hpp"
#include "util/console.hpp"
#include "util/fstream.hpp"
#include "util/log.hpp"

#ifndef MYIOSDECK_INNO_SHA
#define MYIOSDECK_INNO_SHA "unknown"
#endif

namespace fs = boost::filesystem;

namespace {

// innoextract keeps its options and counters in globals: one call at a time.
std::mutex g_lock;
std::vector<std::string> * g_log = NULL;
mid_inno_progress * g_progress = NULL;

void log_sink(logger::log_level level, const std::string & message) {
	if(!g_log || g_log->size() >= 500) {
		return;
	}
	const char * prefix = level == logger::Error ? "error: " : level == logger::Warning ? "warning: " : "";
	g_log->push_back(prefix + message);
}

bool progress_hook(boost::uint64_t value, boost::uint64_t max) {
	if(!g_progress) {
		return true;
	}
	g_progress->done = value;
	g_progress->total = max;
	return !g_progress->cancel;
}

//! Routes innoextract's log and progress bar to us for the lifetime of one call.
struct session {
	std::vector<std::string> log;
	explicit session(mid_inno_progress * progress) {
		color::init(color::disable, color::disable);
		g_log = &log;
		g_progress = progress;
		logger::sink = log_sink;
		logger::quiet = false;
		logger::total_errors = 0;
		logger::total_warnings = 0;
		::progress::hook = progress_hook;
	}
	~session() {
		::progress::hook = NULL;
		logger::sink = NULL;
		g_progress = NULL;
		g_log = NULL;
	}
};

std::string quote(const std::string & s) {
	std::string out = "\"";
	for(size_t i = 0; i < s.size(); i++) {
		unsigned char c = static_cast<unsigned char>(s[i]);
		switch(c) {
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default: {
				if(c < 0x20) {
					char buf[8];
					std::snprintf(buf, sizeof(buf), "\\u%04x", c);
					out += buf;
				} else {
					out += char(c);
				}
			}
		}
	}
	return out + "\"";
}

std::string quote_list(const std::vector<std::string> & list) {
	std::string out = "[";
	for(size_t i = 0; i < list.size(); i++) {
		out += (i ? "," : "") + quote(list[i]);
	}
	return out + "]";
}

char * result(const std::string & json) {
	char * out = static_cast<char *>(std::malloc(json.size() + 1));
	if(out) {
		std::memcpy(out, json.c_str(), json.size() + 1);
	}
	return out;
}

char * failure(const std::string & error, const std::vector<std::string> & log) {
	return result("{\"ok\":false,\"error\":" + quote(error) + ",\"log\":" + quote_list(log) + "}");
}

//! A file in dir with this name, in any case (Files keeps the case the user's PC gave it).
bool exists_any_case(const fs::path & dir, const std::string & name) {
	if(fs::exists(dir / name)) {
		return true;
	}
	boost::system::error_code ec;
	for(fs::directory_iterator i(dir, ec), end; !ec && i != end; i.increment(ec)) {
		if(boost::iequals(util::as_string(i->path().filename()), name)) {
			return true;
		}
	}
	return false;
}

bool starts_with_rar(const fs::path & file) {
	util::ifstream ifs;
	ifs.open(file, std::ios_base::in | std::ios_base::binary);
	char magic[4] = { 0, 0, 0, 0 };
	return ifs.is_open() && ifs.read(magic, 4) && std::memcmp(magic, "Rar!", 4) == 0;
}

} // anonymous namespace

extern "C" bool mid_inno_linked(void) {
	return true;
}

extern "C" const char * mid_inno_version(void) {
	return "innoextract " MYIOSDECK_INNO_SHA;
}

extern "C" char * mid_inno_inspect(const char * setup_exe) {

	std::lock_guard<std::mutex> lock(g_lock);
	session s(NULL);

	try {

		fs::path installer(setup_exe);
		util::ifstream ifs;
		ifs.open(installer, std::ios_base::in | std::ios_base::binary);
		if(!ifs.is_open()) {
			return failure("Could not open " + installer.string(), s.log);
		}

		loader::offsets offsets;
		offsets.load(ifs);
		ifs.seekg(offsets.header_offset);

		setup::info info;
		info.load(ifs, setup::info::Files | setup::info::Directories | setup::info::DataEntries
		               | setup::info::Languages | setup::info::RegistryEntries);

		// What lands in the game folder: files whose destination expands to app/...
		setup::filename_map names;
		names.set_expand(true);
		boost::uint64_t app_size = 0, total_size = 0;
		size_t app_files = 0;
		for(size_t i = 0; i < info.data_entries.size(); i++) {
			total_size += info.data_entries[i].uncompressed_size;
		}
		for(size_t i = 0; i < info.files.size(); i++) {
			const setup::file_entry & file = info.files[i];
			if(file.location >= info.data_entries.size()) {
				continue; // copies of files already on the PC, nothing to unpack
			}
			std::string path = names.convert(file.destination);
			if(!boost::istarts_with(path, std::string("app") + setup::path_sep) && !boost::iequals(path, "app")) {
				continue;
			}
			app_files++;
			app_size += info.data_entries[file.location].uncompressed_size;
			for(size_t j = 0; j < file.additional_locations.size(); j++) {
				if(file.additional_locations[j] < info.data_entries.size()) {
					app_size += info.data_entries[file.additional_locations[j]].uncompressed_size;
				}
			}
		}

		// The .bin slices the data is in, named the way stream::slice_reader looks for them.
		fs::path dir = installer.parent_path();
		std::string stem = util::as_string(installer.stem());
		std::string base2 = info.header.base_filename;
		std::replace(base2.begin(), base2.end(), '/', '_');
		std::replace(base2.begin(), base2.end(), '\\', '_');
		bool embedded = offsets.data_offset != 0;
		std::string parts = "[";
		if(!embedded && !info.data_entries.empty()) {
			boost::uint32_t last = 0;
			for(size_t i = 0; i < info.data_entries.size(); i++) {
				last = std::max(last, info.data_entries[i].chunk.last_slice);
			}
			size_t per_disk = info.header.slices_per_disk ? info.header.slices_per_disk : 1;
			for(boost::uint32_t slice = 0; slice <= last; slice++) {
				std::string name = stream::slice_reader::slice_filename(stem, slice, per_disk);
				bool present = exists_any_case(dir, name);
				if(!present && !base2.empty()) {
					present = exists_any_case(dir, stream::slice_reader::slice_filename(base2, slice, per_disk));
				}
				parts += std::string(slice ? "," : "") + "{\"name\":" + quote(name)
				         + ",\"present\":" + (present ? "true" : "false") + "}";
			}
		}
		parts += "]";

		// Older GOG installers keep the game in a RAR archive next to the .exe (setup_x-1.bin),
		// which innoextract hands to unrar: not available here.
		bool rar = false;
		if(embedded) {
			fs::path bin = dir / (stem + "-1.bin");
			rar = fs::exists(bin) && starts_with_rar(bin);
		}

		std::vector<std::string> languages;
		for(size_t i = 0; i < info.languages.size(); i++) {
			languages.push_back(info.languages[i].name);
		}

		std::ostringstream version;
		version << info.version;
		std::ostringstream sizes;
		sizes << ",\"app_files\":" << app_files << ",\"app_size\":" << app_size << ",\"total_size\":" << total_size;

		return result(std::string("{\"ok\":true")
			+ ",\"app_name\":" + quote(info.header.app_name)
			+ ",\"app_versioned_name\":" + quote(info.header.app_versioned_name)
			+ ",\"default_dir_name\":" + quote(info.header.default_dir_name)
			+ ",\"publisher\":" + quote(info.header.app_publisher)
			+ ",\"inno_version\":" + quote(version.str())
			+ ",\"gog_id\":" + quote(gog::get_game_id(info))
			+ ",\"embedded_data\":" + (embedded ? "true" : "false")
			+ ",\"rar_data\":" + (rar ? "true" : "false")
			+ ",\"encrypted\":" + ((info.header.options & setup::header::EncryptionUsed) ? "true" : "false")
			+ sizes.str()
			+ ",\"languages\":" + quote_list(languages)
			+ ",\"parts\":" + parts
			+ ",\"log\":" + quote_list(s.log) + "}");

	} catch(const setup::version_error &) {
		return failure("Not an Inno Setup installer, or one from an Inno Setup version innoextract does not know", s.log);
	} catch(const std::exception & e) {
		return failure(std::string("Could not read the installer: ") + e.what(), s.log);
	}
}

extern "C" char * mid_inno_extract(const char * setup_exe, const char * out_dir, const char * language,
                                   mid_inno_progress * progress) {

	std::lock_guard<std::mutex> lock(g_lock);
	session s(progress);

	extract_options o;
	o.quiet = true;              // no "Extracting ..." banner on stdout
	o.silent = true;             // nor collision notes: nothing goes to stdout
	o.extract = true;
	o.extract_unknown = true;
	o.extract_temp = false;      // -m
	o.include.push_back("/app"); // -I /app: only what goes into the game folder
	o.gog = false;               // no unrar on iOS (rar_data in inspect reports those installers)
	o.gog_galaxy = true;         // re-assemble GOG Galaxy file parts
	o.preserve_file_times = true;
	o.collisions = OverwriteCollisions;
	o.default_language = language ? language : "";
	o.filenames.set_expand(true);
	o.output_dir = out_dir;

	std::string error;
	try {
		process_file(fs::path(setup_exe), o);
	} catch(const std::ios_base::failure & e) {
		error = std::string("Stream error while extracting files: ") + e.what();
	} catch(const format_error & e) {
		error = e.what();
	} catch(const setup::version_error &) {
		error = "Not an Inno Setup installer, or one from an Inno Setup version innoextract does not know";
	} catch(const std::exception & e) {
		error = e.what();
	}
	if(error.empty() && logger::total_errors) {
		error = "innoextract reported errors (see the log)";
		for(size_t i = s.log.size(); i-- > 0;) {
			if(s.log[i].compare(0, 7, "error: ") == 0) {
				error = s.log[i].substr(7);
				break;
			}
		}
	}

	std::ostringstream json;
	json << "{\"ok\":" << (error.empty() ? "true" : "false")
	     << ",\"error\":" << quote(error)
	     << ",\"cancelled\":" << ((progress && progress->cancel) ? "true" : "false")
	     << ",\"warnings\":" << logger::total_warnings
	     << ",\"errors\":" << logger::total_errors
	     << ",\"done\":" << (progress ? progress->done : 0)
	     << ",\"total\":" << (progress ? progress->total : 0)
	     << ",\"log\":" << quote_list(s.log) << "}";
	return result(json.str());
}

extern "C" void mid_inno_free(char * json) {
	std::free(json);
}
