#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "fastfiles.hpp"

#include "command.hpp"
#include "game_console.hpp"

#include <utils/hook.hpp>
#include <utils/memory.hpp>

namespace fastfiles
{
	static std::string current_fastfile;

	namespace
	{
		utils::hook::detour db_try_load_x_file_internal_hook;

		void db_try_load_x_file_internal(const char* zone_name, const int flags)
		{
			game_console::print(game_console::con_type_info, "Loading fastfile %s\n", zone_name);
			current_fastfile = zone_name;
			return db_try_load_x_file_internal_hook.invoke<void>(zone_name, flags);
		}
	}

	const char* get_current_fastfile()
	{
		return current_fastfile.data();
	}

	void reallocate_asset_pool(const game::XAssetType type, const unsigned int new_size)
	{
		const size_t element_size = game::DB_GetXAssetTypeSize(type);

		auto* new_pool = utils::memory::get_allocator()->allocate(new_size * element_size);
		std::memmove(new_pool, game::DB_XAssetPool[type], game::g_poolSize[type] * element_size);

		game::DB_XAssetPool[type] = new_pool;
		game::g_poolSize[type] = new_size;
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			db_try_load_x_file_internal_hook.create(
				SELECT_VALUE(0x1401816F0, 0x1402741C0), &db_try_load_x_file_internal);

			command::add("loadzone", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					game_console::print(game_console::con_type_info, "usage: loadzone <zone>\n");
					return;
				}

				game::XZoneInfo info;
				info.name = params.get(1);
				info.allocFlags = 1;
				info.freeFlags = 0;
				game::DB_LoadXAssets(&info, 1u, game::DBSyncMode::DB_LOAD_SYNC);
			});

			reallocate_asset_pool(game::ASSET_TYPE_FONT, 48);
		}
	};
}

REGISTER_COMPONENT(fastfiles::component)
