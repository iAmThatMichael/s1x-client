#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "command.hpp"
#include "game_console.hpp"

#include "game/game.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>
#include <utils/memory.hpp>

namespace command
{
	namespace
	{
		utils::hook::detour client_command_hook;

		std::unordered_map<std::string, std::function<void(params&)>> handlers;
		std::unordered_map<std::string, std::function<void(int, params_sv&)>> handlers_sv;

		void main_handler()
		{
			params params = {};

			const auto command = utils::string::to_lower(params[0]);
			if (handlers.find(command) != handlers.end())
			{
				handlers[command](params);
			}
		}

		void client_command(const int client_num, void* a2)
		{
			params_sv params = {};

			const auto command = utils::string::to_lower(params[0]);
			if (handlers_sv.find(command) != handlers_sv.end())
			{
				handlers_sv[command](client_num, params);
			}

			client_command_hook.invoke<void>(client_num, a2);
		}

		// Shamelessly stolen from Quake3
		// https://github.com/id-Software/Quake-III-Arena/blob/dbe4ddb10315479fc00086f08e25d968b4b43c49/code/qcommon/common.c#L364
		void parse_command_line()
		{
			static auto parsed = false;
			if (parsed)
			{
				return;
			}

			static std::string comand_line_buffer = GetCommandLineA();
			auto* command_line = comand_line_buffer.data();

			auto& com_num_console_lines = *reinterpret_cast<int*>(0x147B76504);
			auto* com_console_lines = reinterpret_cast<char**>(0x147B76510);

			auto inq = false;
			com_console_lines[0] = command_line;
			com_num_console_lines = 0;

			while (*command_line)
			{
				if (*command_line == '"')
				{
					inq = !inq;
				}
				// look for a + separating character
				// if commandLine came from a file, we might have real line seperators
				if ((*command_line == '+' && !inq) || *command_line == '\n' || *command_line == '\r')
				{
					if (com_num_console_lines == 0x20) // MAX_CONSOLE_LINES
					{
						break;
					}
					com_console_lines[com_num_console_lines] = command_line + 1;
					com_num_console_lines++;
					*command_line = '\0';
				}
				command_line++;
			}
			parsed = true;
		}

		void parse_commandline_stub()
		{
			parse_command_line();
			reinterpret_cast<void(*)()>(0x1403CEE10)();
		}
	}

	void read_startup_variable(const std::string& dvar)
	{
		// parse the commandline if it's not parsed
		parse_command_line();

		auto& com_num_console_lines = *reinterpret_cast<int*>(0x147B76504);
		auto* com_console_lines = reinterpret_cast<char**>(0x147B76510);

		for (int i = 0; i < com_num_console_lines; i++)
		{
			game::Cmd_TokenizeString(com_console_lines[i]);

			// only +set dvar value
			if (game::Cmd_Argc() >= 3 && game::Cmd_Argv(0) == "set"s && game::Cmd_Argv(1) == dvar)
			{
				game::Dvar_SetCommand(game::Cmd_Argv(1), game::Cmd_Argv(2));
			}

			game::Cmd_EndTokenizeString();
		}
	}

	params::params()
		: nesting_(game::cmd_args->nesting)
	{
	}

	int params::size() const
	{
		return game::cmd_args->argc[this->nesting_];
	}

	const char* params::get(const int index) const
	{
		if (index >= this->size())
		{
			return "";
		}

		return game::cmd_args->argv[this->nesting_][index];
	}

	std::string params::join(const int index) const
	{
		std::string result = {};

		for (auto i = index; i < this->size(); i++)
		{
			if (i > index) result.append(" ");
			result.append(this->get(i));
		}
		return result;
	}

	params_sv::params_sv()
		: nesting_(game::sv_cmd_args->nesting)
	{
	}

	int params_sv::size() const
	{
		return game::sv_cmd_args->argc[this->nesting_];
	}

	const char* params_sv::get(const int index) const
	{
		if (index >= this->size())
		{
			return "";
		}

		return game::sv_cmd_args->argv[this->nesting_][index];
	}

	std::string params_sv::join(const int index) const
	{
		std::string result = {};

		for (auto i = index; i < this->size(); i++)
		{
			if (i > index) result.append(" ");
			result.append(this->get(i));
		}
		return result;
	}

	void add_raw(const char* name, void (*callback)())
	{
		game::Cmd_AddCommandInternal(name, callback, utils::memory::get_allocator()->allocate<game::cmd_function_s>());
	}

	void add(const char* name, const std::function<void(const params&)>& callback)
	{
		const auto command = utils::string::to_lower(name);

		if (handlers.find(command) == handlers.end())
			add_raw(name, main_handler);

		handlers[command] = callback;
	}

	void add(const char* name, const std::function<void()>& callback)
	{
		add(name, [callback](const params&)
		{
			callback();
		});
	}

	void add_sv(const char* name, std::function<void(int, const params_sv&)> callback)
	{
		// doing this so the sv command would show up in the console
		add_raw(name, nullptr);

		const auto command = utils::string::to_lower(name);

		if (handlers_sv.find(command) == handlers_sv.end())
			handlers_sv[command] = std::move(callback);
	}

	void execute(std::string command, const bool sync)
	{
		command += "\n";

		if (sync)
		{
			game::Cmd_ExecuteSingleCommand(0, 0, command.data());
		}
		else
		{
			game::Cbuf_AddText(0, command.data());
		}
	}

	void enum_assets(const game::XAssetType type, const std::function<void(game::XAssetHeader)>& callback, const bool includeOverride)
	{
		game::DB_EnumXAssets_Internal(type, static_cast<void(*)(game::XAssetHeader, void*)>([](game::XAssetHeader header, void* data)
		{
			const auto& cb = *static_cast<const std::function<void(game::XAssetHeader)>*>(data);
			cb(header);
		}), &callback, includeOverride);
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			if (game::environment::is_sp())
			{
				add_commands_sp();
			}
			else
			{
				utils::hook::call(0x1403CDF1C, &parse_commandline_stub);

				add_commands_mp();
			}
			add_commands_generic();
		}

	private:
		static void add_commands_generic()
		{
			add("quit", game::Com_Quit_f);
			add("quit_hard", utils::nt::raise_hard_exception);
			add("crash", []()
			{
				*reinterpret_cast<int*>(1) = 0;
			});

			add("consoleList", [](const params& params)
			{
				const std::string input = params.get(1);

				std::vector<std::string> matches;
				game_console::find_matches(input, matches, false);

				for(auto& match : matches)
				{
					auto* dvar = game::Dvar_FindVar(match.c_str());
					if (!dvar)
					{
						game_console::print(game_console::con_type_info, "[CMD]\t %s", match.c_str());
					}
					else
					{
						game_console::print(game_console::con_type_info, "[DVAR]\t%s \"%s\"", match.c_str(), game::Dvar_ValueToString(dvar, dvar->current));
					}
				}
				
				game_console::print(game_console::con_type_info, "Total %i matches", matches.size());
			});

			add("dvarDump", []()
			{
				game_console::print(game_console::con_type_info,
				                    "================================ DVAR DUMP ========================================\n");
				for (auto i = 0; i < *game::dvarCount; i++)
				{
					const auto dvar = game::sortedDvars[i];
					if (dvar)
					{
						game_console::print(game_console::con_type_info, "%s \"%s\"\n", dvar->name,
						                    game::Dvar_ValueToString(dvar, dvar->current));
					}
				}
				game_console::print(game_console::con_type_info, "\n%i dvar indexes\n", *game::dvarCount);
				game_console::print(game_console::con_type_info,
				                    "================================ END DVAR DUMP ====================================\n");
			});

			add("commandDump", []()
			{
				game_console::print(game_console::con_type_info,
				                    "================================ COMMAND DUMP =====================================\n");
				game::cmd_function_s* cmd = (*game::cmd_functions);
				int i = 0;
				while (cmd)
				{
					if (cmd->name)
					{
						game_console::print(game_console::con_type_info, "%s\n", cmd->name);
						i++;
					}
					cmd = cmd->next;
				}
				game_console::print(game_console::con_type_info, "\n%i command indexes\n", i);
				game_console::print(game_console::con_type_info,
				                    "================================ END COMMAND DUMP =================================\n");
			});

			add("listassetpool", [](const params& params)
			{
				if (params.size() < 2)
				{
					game_console::print(game_console::con_type_info,
									"listassetpool <poolnumber>: list all the assets in the specified pool\n");

					for (auto i = 0; i < game::XAssetType::ASSET_TYPE_COUNT; i++)
					{
						game_console::print(game_console::con_type_info, "%d %s\n", i, game::g_assetNames[i]);
					}
				}
				else
				{
					const auto type = static_cast<game::XAssetType>(atoi(params.get(1)));

					if (type < 0 || type >= game::XAssetType::ASSET_TYPE_COUNT)
					{
						game_console::print(game_console::con_type_error,
											"Invalid pool passed must be between [%d, %d]", 0,
											game::XAssetType::ASSET_TYPE_COUNT - 1);
						return;
					}

					game_console::print(game_console::con_type_info, "Listing assets in pool %s",
										game::g_assetNames[type]);

					enum_assets(type, [type](game::XAssetHeader header)
					{
						const game::XAsset asset{ type, header };
						const auto* asset_name = game::DB_GetXAssetName(&asset);
						const auto* const entry = game::DB_FindXAssetEntry(type, asset_name);
						const char* zone_name;

						if(game::environment::is_sp())
						{
							zone_name = game::sp::g_zones_0[entry->zoneIndex].name;
						}
						else
						{
							zone_name = game::mp::g_zones_0[entry->zoneIndex].name;
						}

						game_console::print(game_console::con_type_info, "%s | %s.ff", asset_name, zone_name);
					}, true);
				}
			});
		}

		static void add_commands_sp()
		{
			add("god", []()
			{
				if (!game::SV_Loaded())
				{
					return;
				}

				game::sp::g_entities[0].flags ^= 1;
				game::CG_GameMessage(0, utils::string::va("godmode %s",
				                                          game::sp::g_entities[0].flags & 1
					                                          ? "^2on"
					                                          : "^1off"));
			});

			add("demigod", []()
			{
				if (!game::SV_Loaded())
				{
					return;
				}

				game::sp::g_entities[0].flags ^= 2;
				game::CG_GameMessage(0, utils::string::va("demigod mode %s",
				                                          game::sp::g_entities[0].flags & 2
					                                          ? "^2on"
					                                          : "^1off"));
			});

			add("noclip", []()
			{
				if (!game::SV_Loaded())
				{
					return;
				}

				game::sp::g_entities[0].client->flags ^= 1;
				game::CG_GameMessage(0, utils::string::va("noclip %s",
				                                          game::sp::g_entities[0].client->flags & 1
					                                          ? "^2on"
					                                          : "^1off"));
			});

			add("ufo", []()
			{
				if (!game::SV_Loaded())
				{
					return;
				}

				game::sp::g_entities[0].client->flags ^= 2;
				game::CG_GameMessage(
					0, utils::string::va("ufo %s", game::sp::g_entities[0].client->flags & 2 ? "^2on" : "^1off"));
			});

			add("give", [](const params& params)
			{
				if (!game::SV_Loaded())
				{
					return;
				}

				if (params.size() < 2)
				{
					game::CG_GameMessage(0, "You did not specify a weapon name");
					return;
				}

				auto ps = game::SV_GetPlayerstateForClientNum(0);
				const auto wp = game::G_GetWeaponForName(params.get(1));
				if (wp)
				{
					if (game::G_GivePlayerWeapon(ps, wp, 0, 0, 0, 0, 0, 0))
					{
						game::G_InitializeAmmo(ps, wp, 0);
						game::G_SelectWeapon(0, wp);
					}
				}
			});

			add("take", [](const params& params)
			{
				if (!game::SV_Loaded())
				{
					return;
				}

				if (params.size() < 2)
				{
					game::CG_GameMessage(0, "You did not specify a weapon name");
					return;
				}

				auto ps = game::SV_GetPlayerstateForClientNum(0);
				const auto wp = game::G_GetWeaponForName(params.get(1));
				if (wp)
				{
					game::G_TakePlayerWeapon(ps, wp);
				}
			});
		}

		static void add_commands_mp()
		{
			client_command_hook.create(0x1402E98F0, &client_command);

			add_sv("god", [](const int client_num, const params_sv&)
			{
				if (!game::Dvar_FindVar("sv_cheats")->current.enabled)
				{
					game::SV_GameSendServerCommand(client_num, game::SV_CMD_RELIABLE,
					                               "f \"Cheats are not enabled on this server\"");
					return;
				}

				game::mp::g_entities[client_num].flags ^= 1;
				game::SV_GameSendServerCommand(client_num, game::SV_CMD_RELIABLE,
				                               utils::string::va("f \"godmode %s\"",
				                                                 game::mp::g_entities[client_num].flags & 1
					                                                 ? "^2on"
					                                                 : "^1off"));
			});

			add_sv("demigod", [](const int client_num, const params_sv&)
			{
				if (!game::Dvar_FindVar("sv_cheats")->current.enabled)
				{
					game::SV_GameSendServerCommand(client_num, game::SV_CMD_RELIABLE,
					                               "f \"Cheats are not enabled on this server\"");
					return;
				}

				game::mp::g_entities[client_num].flags ^= 2;
				game::SV_GameSendServerCommand(client_num, game::SV_CMD_RELIABLE,
				                               utils::string::va("f \"demigod mode %s\"",
				                                                 game::mp::g_entities[client_num].flags & 2
					                                                 ? "^2on"
					                                                 : "^1off"));
			});

			add_sv("noclip", [](const int client_num, const params_sv&)
			{
				if (!game::Dvar_FindVar("sv_cheats")->current.enabled)
				{
					game::SV_GameSendServerCommand(client_num, game::SV_CMD_RELIABLE,
					                               "f \"Cheats are not enabled on this server\"");
					return;
				}

				game::mp::g_entities[client_num].client->flags ^= 1;
				game::SV_GameSendServerCommand(client_num, game::SV_CMD_RELIABLE,
				                               utils::string::va("f \"noclip %s\"",
				                                                 game::mp::g_entities[client_num].client->flags & 1
					                                                 ? "^2on"
					                                                 : "^1off"));
			});

			add_sv("ufo", [](const int client_num, const params_sv&)
			{
				if (!game::Dvar_FindVar("sv_cheats")->current.enabled)
				{
					game::SV_GameSendServerCommand(client_num, game::SV_CMD_RELIABLE,
					                               "f \"Cheats are not enabled on this server\"");
					return;
				}

				game::mp::g_entities[client_num].client->flags ^= 2;
				game::SV_GameSendServerCommand(client_num, game::SV_CMD_RELIABLE,
				                               utils::string::va("f \"ufo %s\"",
				                                                 game::mp::g_entities[client_num].client->flags & 2
					                                                 ? "^2on"
					                                                 : "^1off"));
			});

			add_sv("give", [](const int client_num, const params_sv& params)
			{
				if (!game::Dvar_FindVar("sv_cheats")->current.enabled)
				{
					game::SV_GameSendServerCommand(client_num, game::SV_CMD_RELIABLE,
					                               "f \"Cheats are not enabled on this server\"");
					return;
				}

				if (params.size() < 2)
				{
					game::SV_GameSendServerCommand(client_num, game::SV_CMD_RELIABLE,
					                               "f \"You did not specify a weapon name\"");
					return;
				}

				auto ps = game::SV_GetPlayerstateForClientNum(client_num);
				const auto wp = game::G_GetWeaponForName(params.get(1));
				if (wp)
				{
					if (game::G_GivePlayerWeapon(ps, wp, 0, 0, 0, 0, 0, 0))
					{
						game::G_InitializeAmmo(ps, wp, 0);
						game::G_SelectWeapon(client_num, wp);
					}
				}
			});

			add_sv("take", [](const int client_num, const params_sv& params)
			{
				if (!game::Dvar_FindVar("sv_cheats")->current.enabled)
				{
					game::SV_GameSendServerCommand(client_num, game::SV_CMD_RELIABLE,
					                               "f \"Cheats are not enabled on this server\"");
					return;
				}

				if (params.size() < 2)
				{
					game::SV_GameSendServerCommand(client_num, game::SV_CMD_RELIABLE,
					                               "f \"You did not specify a weapon name\"");
					return;
				}

				auto ps = game::SV_GetPlayerstateForClientNum(client_num);
				const auto wp = game::G_GetWeaponForName(params.get(1));
				if (wp)
				{
					game::G_TakePlayerWeapon(ps, wp);
				}
			});
		}
	};
}

REGISTER_COMPONENT(command::component)
