#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "command.hpp"
#include "game/game.hpp"
#include "scheduler.hpp"

#include <utils/hook.hpp>
#include <utils/nt.hpp>

namespace patches
{
	namespace
	{
		utils::hook::detour live_get_local_client_name_hook;

		const char* live_get_local_client_name()
		{
			return game::Dvar_FindVar("name")->current.string;
		}
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			// Increment ref-count on these
			LoadLibraryA("PhysXDevice64.dll");
			LoadLibraryA("PhysXUpdateLoader64.dll");

			if (game::environment::is_sp())
			{
				patch_sp();
			}
			else
			{
				patch_mp();
			}
		}

		static void patch_mp()
		{
			// Use name dvar
			//live_get_local_client_name_hook.create(0x1404D47F0, &live_get_local_client_name);

			// block changing name in-game
			//utils::hook::set<uint8_t>(0x140438850, 0xC3);
		}

		static void patch_sp()
		{
			// SP doesn't initialize WSA
			WSADATA wsa_data;
			WSAStartup(MAKEWORD(2, 2), &wsa_data);
		}
	};
}

REGISTER_COMPONENT(patches::component)
