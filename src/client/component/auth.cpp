#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "auth.hpp"
#include "command.hpp"
#include "network.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>
#include <utils/smbios.hpp>
#include <utils/info_string.hpp>
#include <utils/cryptography.hpp>

#include "game/game.hpp"

namespace auth
{
	namespace
	{
		std::string get_key_entropy()
		{
			auto uuid = utils::smbios::get_uuid();
			if (uuid.empty())
			{
				uuid.resize(16);
				utils::cryptography::random::get_data(uuid.data(), uuid.size());
			}

			return uuid;
		}

		utils::cryptography::ecc::key& get_key()
		{
			static auto key = utils::cryptography::ecc::generate_key(512, get_key_entropy());
			return key;
		}

		int send_connect_data_stub(game::netsrc_t sock, game::netadr_s* adr, const char* format, const int len)
		{
			std::string connect_string(format, len);
			game::SV_Cmd_TokenizeString(connect_string.data());
			const auto _ = gsl::finally([]()
			{
				game::SV_Cmd_EndTokenizedString();
			});

			const command::params_sv params;
			if (params.size() < 3)
			{
				return false;
			}

			const utils::info_string info_string{std::string{params[2]}};
			const auto challenge = info_string.get("challenge");

			connect_string.clear();
			connect_string.append(params[0]);
			connect_string.append(" ");
			connect_string.append(params[1]);
			connect_string.append(" ");
			connect_string.append("\"" + info_string.build() + "\"");

			proto::network::connect_info info;
			info.set_publickey(get_key().get_public_key());
			info.set_signature(sign_message(get_key(), challenge));
			info.set_infostring(connect_string);

			network::send(*adr, "connect", info.SerializeAsString());
			return true;
		}

		void direct_connect(game::netadr_s* from, game::msg_t* msg)
		{
			const auto offset = sizeof("connect") + 4;

			proto::network::connect_info info;
			if (!info.ParseFromArray(msg->data + offset, msg->cursize - offset))
			{
				network::send(*from, "error", "Invalid connect data!", '\n');
				return;
			}

			game::SV_Cmd_EndTokenizedString();
			game::SV_Cmd_TokenizeString(info.infostring().data());

			const command::params_sv params;
			if (params.size() < 3)
			{
				network::send(*from, "error", "Invalid connect string!", '\n');
				return;
			}

			const utils::info_string info_string{std::string{params[2]}};

			const auto steam_id = info_string.get("xuid");
			const auto challenge = info_string.get("challenge");

			if (steam_id.empty() || challenge.empty())
			{
				network::send(*from, "error", "Invalid connect data!", '\n');
				return;
			}

			utils::cryptography::ecc::key key;
			key.set(info.publickey());

			const auto xuid = strtoull(steam_id.data(), nullptr, 16);
			if (xuid != key.get_hash())
			{
				//MessageBoxA(nullptr, steam_id.data(), std::to_string(key.get_hash()).data(), 0);
				network::send(*from, "error",
					utils::string::va("XUID doesn't match the certificate: %llX != %llX", xuid, key.get_hash()), '\n');
				return;
			}

			if (!key.is_valid() || !verify_message(key, challenge, info.signature()))
			{
				network::send(*from, "error", "Challenge signature was invalid!", '\n');
				return;
			}

			game::SV_DirectConnect(from);
		}

		void* get_direct_connect_stub()
		{
			return utils::hook::assemble([](utils::hook::assembler& a)
			{
				a.lea(rcx, qword_ptr(rsp, 0x20));
				a.movaps(xmmword_ptr(rsp, 0x20), xmm0);

				a.pushad64();
				a.mov(rdx, rdi);
				a.call_aligned(direct_connect);
				a.popad64();

				a.jmp(0x140442317);
			});
		}
	}

	uint64_t get_guid()
	{
		if (game::environment::is_dedi())
		{
			return 0x110000100000000 | (::utils::cryptography::random::get_integer() & ~0x80000000);
		}

		return get_key().get_hash();
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			// Patch steam id bit check
			if (game::environment::is_sp())
			{
				utils::hook::jump(0x1404267F0, 0x140426846);
				utils::hook::jump(0x14042760F, 0x140427650);
				utils::hook::jump(0x140427AB4, 0x140427B02);
			}
			else
			{
				utils::hook::jump(0x140538920, 0x140538976);
				utils::hook::jump(0x140009801, 0x140009B48);
				utils::hook::jump(0x140009AEB, 0x140009B48);
				utils::hook::jump(0x14053995F, 0x1405399A0);
				utils::hook::jump(0x140539E70, 0x140539EB6);

				utils::hook::jump(0x1404421F6, get_direct_connect_stub(), true);
				utils::hook::call(0x140208C54, send_connect_data_stub);
			}
		}
	};
}

REGISTER_COMPONENT(auth::component)
