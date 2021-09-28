#include <dpp/dpp.h>
#include <dpp/nlohmann/json.hpp>
#include <dpp/fmt/format.h>
#include <iomanip>
#include <sstream>

#include <vector>
#include <fstream>
#include <iostream>
#include <mpg123.h>
#include <out123.h>

using json = nlohmann::json;


static bool match(const char* str, const char* mask)
{
	char* cp = NULL;
	char* mp = NULL;
	char* string = (char*)str;
	char* wild = (char*)mask;

	while ((*string) && (*wild != '*')) {
		if ((tolower(*wild) != tolower(*string)) && (*wild != '?')) {
			return 0;
		}
		wild++;
		string++;
	}

	while (*string) {
		if (*wild == '*') {
			if (!*++wild) {
				return 1;
			}
			mp = wild;
			cp = string+1;
		}
		else {
			if ((tolower(*wild) == tolower(*string)) || (*wild == '?')) {
				wild++;
				string++;
			} else {
				wild = mp;
				string = cp++;
			}
		}
	}

	while (*wild == '*') {
		wild++;
	}

	return !*wild;
}

/**
 *  trim from end of string (right)
 */
inline std::string rtrim(std::string s)
{
	s.erase(s.find_last_not_of(" \t\n\r\f\v") + 1);
	return s;
}

/**
 * trim from beginning of string (left)
 */
inline std::string ltrim(std::string s)
{
	s.erase(0, s.find_first_not_of(" \t\n\r\f\v"));
	return s;
}

/**
 * trim from both ends of string (right then left)
 */
inline std::string trim(std::string s)
{
	return ltrim(rtrim(s));
}


std::string song_to_load = "";
dpp::snowflake last_ch_id = 0;
bool encode_thread_active = false;

std::vector<uint8_t> get_song(std::string file)
{
	std::vector<uint8_t> pcmdata;

	mpg123_init();

	int err;
	mpg123_handle *mh = mpg123_new(NULL, &err);
	unsigned char* buffer;
	size_t buffer_size;
	size_t done;
	int channels, encoding;
	long rate;

	mpg123_param(mh, MPG123_FORCE_RATE, 48000, 48000.0);

	buffer_size = mpg123_outblock(mh);
	buffer = new unsigned char[buffer_size];

	mpg123_open(mh, file.c_str());
	mpg123_getformat(mh, &rate, &channels, &encoding);

	unsigned int counter = 0;
	for (int totalBtyes = 0; mpg123_read(mh, buffer, buffer_size, &done) == MPG123_OK; ) {
		for (auto i = 0; i < buffer_size; i++) {
			pcmdata.push_back(buffer[i]);
		}
		counter += buffer_size;
		totalBtyes += done;
	}
	delete buffer;
	mpg123_close(mh);
	mpg123_delete(mh);
	mpg123_exit();
	return pcmdata;
}

std::string current_song;

void start_play(dpp::discord_voice_client* v, dpp::cluster &bot)
{
	std::ifstream file("songindex.txt");
	std::string search = song_to_load;
	std::string load_this;
	std::replace(search.begin(), search.end(), ' ', '*');
	search = "*" + search + "*";
	if (file.is_open()) {
		std::string line;
		while (std::getline(file, line)) {
			if (match(line.c_str(), search.c_str())) {
				load_this = line;
				break;
			}
		}
		file.close();
	}
	if (load_this.empty()) {
		bot.message_create(dpp::message(last_ch_id, "No songs that match: " + song_to_load));
	} else {
		auto t = std::thread([&bot, v, load_this]() {
			char filename_part[1024];
			strncpy(filename_part, load_this.c_str(), 1023);
			for (char* v = filename_part; *v; ++v) {
				if (*v == '_') {
					*v = ' ';
				}
			}
			filename_part[load_this.length() - 4] = '\0';
			bot.message_create(dpp::message(last_ch_id, "Enqueued: " + std::string(basename(filename_part))));
			bot.log(dpp::ll_info, fmt::format("Begin mp3 decode of file: {}", std::string(basename(filename_part))));
			double mp3_start = dpp::utility::time_f();
			std::vector<uint8_t> pcmdata = get_song(load_this);
			double mp3_end = dpp::utility::time_f();
			bot.log(dpp::ll_info, fmt::format("End mp3 decode of file: {} [{:.02f} seconds]", std::string(basename(filename_part)), mp3_end - mp3_start));
			if (encode_thread_active) {
				bot.log(dpp::ll_info, fmt::format("Waiting for opus to finish..."));
				while(encode_thread_active) {
					std::this_thread::sleep_for(std::chrono::seconds(1));
				}
			}
			encode_thread_active = true;
			bot.log(dpp::ll_info, fmt::format("Begin opus encode of file: {}", std::string(basename(filename_part))));
			double opus_start = dpp::utility::time_f();
			v->insert_marker(std::string(basename(filename_part)));
			v->send_audio_raw((uint16_t*)pcmdata.data(), pcmdata.size());
			double opus_end = dpp::utility::time_f();
			bot.log(dpp::ll_info, fmt::format("End opus encode of file: {} [{:.02f} seconds]", std::string(basename(filename_part)), opus_end - opus_start));
			encode_thread_active = false;

		});
		t.detach();
}
}

int main(int argc, char const *argv[])
{

	/* Setup the bot */
        json configdocument;
        std::ifstream configfile("../config.json");
        configfile >> configdocument;
        dpp::cluster bot(configdocument["token"]);

	/* Use the on_message_create event to look for commands */
	bot.on_message_create([&bot](const dpp::message_create_t & event) {
		std::stringstream ss(event.msg->content);
		std::string command;
		ss >> command;

		if (command == ".queue") {
			std::string resp = "__**Current Queue:**__\n\n";
			bool any = false;
			dpp::voiceconn* v = event.from->get_voice(event.msg->guild_id);
			if (v && v->voiceclient && v->voiceclient->is_ready()) {
				std::vector<std::string> songqueue = v->voiceclient->get_marker_metadata();
				for (auto & s : songqueue) {
					any = true;
					if (resp.length() < 2048) {
						resp += "🎵 " + s + "\n";
					} else {
						break;
					}
				}
			}
			if (!any) {
				bot.message_create(dpp::message(event.msg->channel_id, "The queue is empty, fool. Play something!"));
			} else {
				bot.message_create(dpp::message(event.msg->channel_id, resp), [&](const dpp::confirmation_callback_t &callback) {
					if (callback.is_error()) {
						bot.log(dpp::ll_error, fmt::format("Failed to send message: {}", callback.http_info.body));
					}
				});
			}
		}
		
		if (command == ".search") {
			std::string search;
			std::getline(ss, search);
			search = trim(search);
			std::string matches = "__**Search Results:**__\n\n";
			
			std::ifstream file("songindex.txt");
			std::replace(search.begin(), search.end(), ' ', '*');
			search = "*" + search + "*";
			bool found_any = false;
			if (file.is_open()) {
				std::string line;
				bot.log(dpp::ll_debug, fmt::format("Searching for: '{}'", search));
				while (std::getline(file, line)) {
					if (match(line.c_str(), search.c_str())) {
						bot.log(dpp::ll_debug, fmt::format("Search match: {}", line));
						char filename_part[1024];
						strncpy(filename_part, line.c_str(), 1023);
						for (char* v = filename_part; *v; ++v) {
							if (*v == '_') {
								*v = ' ';
							}
						}
						filename_part[line.length() - 4] = '\0';
						if (matches.length() < 2048) {
							matches += "🎵 " + std::string(basename(filename_part)) + "\n";
						} else {
							break;
						}
						found_any = true;
					}
				}
				file.close();
			}
			if (!found_any) {
				bot.message_create(dpp::message(event.msg->channel_id, "I don't have any songs at all that look like that."));
			} else {
				bot.message_create(dpp::message(event.msg->channel_id, matches), [&](const dpp::confirmation_callback_t &callback) {
					if (callback.is_error()) {
						bot.log(dpp::ll_error, fmt::format("Failed to send message: {}", callback.http_info.body));
					}
				});
			}
		}

		if (command == ".np") {
			dpp::voiceconn* v = event.from->get_voice(event.msg->guild_id);
			if (v && v->voiceclient && v->voiceclient->is_ready() && v->voiceclient->get_tracks_remaining() > 0) {
				bot.message_create(dpp::message(event.msg->channel_id, "Currently playing: " + current_song));
			} else {
				bot.message_create(dpp::message(event.msg->channel_id, "...No sounds here except crickets... 🦗\nPerhaps play something with **.play**?"));
			}
		}

		if (command == ".skip") {
			dpp::voiceconn* v = event.from->get_voice(event.msg->guild_id);
			if (v && v->voiceclient && v->voiceclient->is_ready() && v->voiceclient->get_tracks_remaining() > 1) {
				bot.message_create(dpp::message(event.msg->channel_id, "Skipping..."));
				v->voiceclient->skip_to_next_marker();
			} else {
				bot.message_create(dpp::message(event.msg->channel_id, "There's nothing to skip to!"));
			}
		}

		if (command == ".play") {
			std::getline(ss, song_to_load);
			song_to_load = trim(song_to_load);
			last_ch_id = event.msg->channel_id;
			dpp::voiceconn* v = event.from->get_voice(event.msg->guild_id);
			if (v && v->voiceclient && v->voiceclient->is_ready()) {
				start_play(v->voiceclient, bot);
			} else {
				dpp::guild * g = dpp::find_guild(event.msg->guild_id);
				if (!g->connect_member_voice(event.msg->author->id)) {
					bot.message_create(dpp::message(event.msg->channel_id, "You don't seem to be on a voice channel! :("));
				}
			}
		}
	});

	bot.on_voice_track_marker([&](const dpp::voice_track_marker_t &ev) {
		std::string song = ev.track_meta;
		bot.message_create(dpp::message(last_ch_id, "Now Playing: " + song));
		current_song = song;
	});

	bot.on_voice_ready([&](const dpp::voice_ready_t& ev) {
		dpp::discord_voice_client* v = ev.voice_client;
		if (v) {
			start_play(v, bot);
		}
	});

	bot.on_log([](const dpp::log_t & event) {
		if (event.severity > dpp::ll_trace) {
			std::cout << event.message << "\n";
		}
	});

	/* Start bot */
	bot.start(false);

	return 0;
}
