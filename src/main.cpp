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

#include <musicbot/musicbot.h>

using json = nlohmann::json;

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

void start_play(dpp::discord_voice_client* v, dpp::cluster &bot, dpp::commandhandler* ch = nullptr, dpp::command_source* src = nullptr)
{
	dpp::command_source s;
	bool command = false;
	if (ch && src) {
		command = true;
		s = *src;
	}
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
		if (command)
			bad_embed(*ch, s, "⚠️ No songs that match: " + song_to_load);
		else
			bad_embed(bot, last_ch_id, "⚠️ No songs that match: " + song_to_load);
	} else {
		auto t = std::thread([&bot, v, load_this, ch, command, s]() {
			char filename_part[1024];
			strncpy(filename_part, load_this.c_str(), 1023);
			for (char* v = filename_part; *v; ++v) {
				if (*v == '_') {
					*v = ' ';
				}
			}
			filename_part[load_this.length() - 4] = '\0';
			if (command)
				good_embed(*ch, s, "⌛ Enqueued: " + std::string(basename(filename_part)));
			else
				good_embed(bot, last_ch_id, "⌛ Enqueued: " + std::string(basename(filename_part)));
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

	dpp::snowflake home_server = std::stoull(configdocument["homeserver"].get<std::string>());

	chdir(configdocument["homedir"].get<std::string>().c_str());

	dpp::commandhandler command_handler(&bot);
	command_handler.add_prefix(".").add_prefix("/");

	bot.on_ready([&](const dpp::ready_t &event) {
		command_handler.add_command(
			"queue",
			{ },
			[&](const std::string& command, const dpp::parameter_list_t& parameters, dpp::command_source src) {
				std::string resp = "__**Current Queue:**__\n\n";
				bool any = false;
				dpp::voiceconn* v = bot.get_shard(0)->get_voice(src.guild_id);
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
					bad_embed(command_handler, src, "⚠️ The queue is empty, fool. Play something!");
				} else {
					good_embed(command_handler, src, resp);
				}
			},
			"Show music queue",
			home_server
		);

		command_handler.add_command(
			"np",
			{ },
			[&](const std::string& command, const dpp::parameter_list_t& parameters, dpp::command_source src) {
				dpp::voiceconn* v = bot.get_shard(0)->get_voice(src.guild_id);
				if (v && v->voiceclient && v->voiceclient->is_ready() && v->voiceclient->get_tracks_remaining() > 0) {
					good_embed(command_handler, src, "⏯️ Currently playing: " + current_song);
				} else {
					bad_embed(command_handler, src, "...No sounds here except crickets... 🦗\nPerhaps play something with **.play**?");
				}
			},
			"Show currently playing song",
			home_server
		);

		command_handler.add_command(
			"skip",
			{ },
			[&](const std::string& command, const dpp::parameter_list_t& parameters, dpp::command_source src) {
				dpp::voiceconn* v = bot.get_shard(0)->get_voice(src.guild_id);
				if (v && v->voiceclient && v->voiceclient->is_ready() && v->voiceclient->get_tracks_remaining() > 1) {
					good_embed(command_handler, src, "⏯️ Skipping...");
					v->voiceclient->skip_to_next_marker();
				} else {
					bad_embed(command_handler, src, "⚠️ There's nothing to skip to!");
				}
			},
			"Skip to the next song in the queue",
			home_server
		);

		command_handler.add_command(
			"stop",
			{ },
			[&](const std::string& command, const dpp::parameter_list_t& parameters, dpp::command_source src) {
				dpp::voiceconn* v = bot.get_shard(0)->get_voice(src.guild_id);
				if (v && v->voiceclient && v->voiceclient->is_ready()) {
					good_embed(command_handler, src, "⏯️ Leaving voice...");
					bot.get_shard(0)->disconnect_voice(src.guild_id);
				} else {
					bad_embed(command_handler, src, "⚠️ I'm on no voice channel...");
				}
			},
			"Stop playing, clear queue and leave voice channel",
			home_server
		);

		command_handler.add_command(
			"play",
			{ {"song", dpp::param_info(dpp::pt_string, false, "Song Name") } },
			[&](const std::string& command, const dpp::parameter_list_t& parameters, dpp::command_source src) {
				song_to_load = trim(std::get<std::string>(parameters[0].second));
				last_ch_id = src.channel_id;
				dpp::voiceconn* v = bot.get_shard(0)->get_voice(src.guild_id);
				if (v && v->voiceclient && v->voiceclient->is_ready()) {
					start_play(v->voiceclient, bot, &command_handler, &src);
				} else {
					dpp::guild * g = dpp::find_guild(src.guild_id);
					if (!g->connect_member_voice(src.issuer->id)) {
						bad_embed(command_handler, src, "🔇 You don't seem to be on a voice channel! :(");
					} else {
						good_embed(command_handler, src, "🔈 Connecting to voice...");
					}
				}
			},
			"Play a song",
			home_server
		);

		command_handler.add_command(
			"search",
			{ {"query", dpp::param_info(dpp::pt_string, false, "Search query (wildcard)") } },
			[&](const std::string& command, const dpp::parameter_list_t& parameters, dpp::command_source src) {
				std::string search = trim(std::get<std::string>(parameters[0].second));
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
					bad_embed(command_handler, src, "⚠️ I don't have any songs at all that look like that.");
				} else {
					good_embed(command_handler, src, matches);
				}
			},
			"Search for a song",
			home_server
		);

		command_handler.register_commands();

	});

	bot.on_voice_track_marker([&](const dpp::voice_track_marker_t &ev) {
		std::string song = ev.track_meta;
		good_embed(bot, last_ch_id, "⏯️ Now Playing: " + song);
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
			std::cout << "[" << dpp::utility::loglevel(event.severity) << "] " << event.message << "\n";
		}
	});

	/* Start bot */
	bot.start(true);

	while (true) {
		std::this_thread::sleep_for(std::chrono::seconds(60));

		dpp::discord_client* shard = bot.get_shard(0);
		dpp::channel* c = dpp::find_channel(last_ch_id);

		if (shard && c) {
			dpp::voiceconn* vc = shard->get_voice(c->guild_id);
			if (vc && vc->is_active() && vc->is_ready() && vc->voiceclient) {
				if (!encode_thread_active && vc->voiceclient->get_tracks_remaining() == 0) {
					good_embed(bot, last_ch_id, "⏯️ Leaving voice...");
					shard->disconnect_voice(c->guild_id);
				}
			}
		}
	}

	return 0;
}
