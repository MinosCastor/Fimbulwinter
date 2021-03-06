/*==================================================================*
*     ___ _           _           _          _       _				*
*    / __(_)_ __ ___ | |__  _   _| |_      _(_)_ __ | |_ ___ _ __	*
*   / _\ | | '_ ` _ \| '_ \| | | | \ \ /\ / / | '_ \| __/ _ \ '__|	*
*  / /   | | | | | | | |_) | |_| | |\ V  V /| | | | | ||  __/ |		*
*  \/    |_|_| |_| |_|_.__/ \__,_|_| \_/\_/ |_|_| |_|\__\___|_|		*
*																	*
* ------------------------------------------------------------------*
*							 Emulator   			                *
* ------------------------------------------------------------------*
*                     Licenced under GNU GPL v3                     *
* ----------------------------------------------------------------- *
*                     Charserver from ZoneServer          	        *
* ==================================================================*/
#pragma once

#include "CharServer.hpp"

#include  "../Common/show_message.hpp"
#include  "../Common/database_helper.h"
#include  "../Common/ragnarok.hpp"
#include  "../Common/packets.hpp"
#include  "../Common/timers.hpp"
#include  "../Common/strfuncs.hpp"
#include  "../Common/core.hpp"


#include <boost/thread.hpp>
#include <boost/foreach.hpp>
#include <iostream>

/*! 
 *  \brief     Parse from Zone
 *  
 *  \author    Fimbulwinter Development Team
 *  \author    GreenBox
 *  \date      ??/12/11
 *
 **/
int CharServer::parse_from_zone(tcp_connection::pointer cl)
{
	if (cl->flags.eof)
	{
		typedef map<int, int> tmpdef;

		// Remove all maps from map_to_zone associated with this server
		int rindex = -1;
		BOOST_FOREACH(tmpdef::value_type &kvp, map_to_zone)
		{
			if (rindex != -1)
				map_to_zone.erase(rindex);

			if (kvp.second == cl->tag())
				rindex = kvp.first;
			else
				rindex = -1;
		}

		rindex = -1;
		BOOST_FOREACH(online_char_db::value_type &pair, online_chars)
		{
			if (rindex != -1)
				set_char_offline(rindex, -1);

			if (pair.second.server == cl->tag())
			{
				if (pair.second.disconnect_timer)
					TimerManager::FreeTimer(pair.second.disconnect_timer);

				rindex = pair.first;
			}
			else
				rindex = -1;
		}

		return 0;
	}
	
	while(RFIFOREST(cl) >= 2)
	{
		unsigned short cmd = RFIFOW(cl, 0);

		switch (cmd)
		{
		case INTER_ZC_REQ_REGS:
			if (RFIFOREST(cl) < 13)
			{
				if (RFIFOB(cl,12))
					inter_reply_regs(cl, RFIFOL(cl,2), RFIFOL(cl,6), 3);

				if (RFIFOB(cl,11))
					inter_reply_regs(cl, RFIFOL(cl,2), RFIFOL(cl,6), 2);

				if (RFIFOB(cl,10))
					request_accreg2(RFIFOL(cl,2), RFIFOL(cl,6));

				return 1;
			}
			break;

		case INTER_ZC_MAPS:
			if (RFIFOREST(cl) < 4 || RFIFOREST(cl) < RFIFOW(cl, 2))
				return 0;
			{
				int num_maps = (RFIFOW(cl, 2) - 4) / sizeof(int);
				int accepted = 0;

				for (int i = 0; i < num_maps; i++)
				{
					int m = RFIFOL(cl, 4 + (i * sizeof(int)));

					if (!map_to_zone.count(m))
					{
						map_to_zone[m] = cl->tag();

						accepted++;
					}
				}

				ShowStatus("Received %d maps from ZoneServer %d.\n", accepted, cl->tag());

				cl->skip(RFIFOW(cl, 2));
			}
			break;

		case INTER_ZC_AUTH:
			if (RFIFOREST(cl) < 15)
				return 0;
			{
				int account_id;
				int char_id;
				int login_id1;
				char sex;
				struct CharData* cd = 0;
				struct CharData char_dat;

				account_id = RFIFOL(cl,2);
				char_id    = RFIFOL(cl,6);
				login_id1  = RFIFOL(cl,10);
				sex        = RFIFOB(cl,14);
				cl->skip(15);

				// TODO: Cache char data?
				chars->load_char(char_id, char_dat, true);
				/*cd = (struct mmo_charstatus*)uidb_get(char_db_,char_id);
				if(!)
				{	//Really shouldn't happen.
					mmo_char_fromsql(char_id, &char_dat, true);
					cd = (struct mmo_charstatus*)uidb_get(char_db_,char_id);
				}*/
				if(auth_nodes.count(account_id) && /*cd != NULL &&*/
					auth_nodes[account_id].char_id == char_id &&
					auth_nodes[account_id].login_id1 == login_id1 &&
					auth_nodes[account_id].sex == sex)
				{
					WFIFOHEAD(cl, 24 + sizeof(struct CharData));
					WFIFOW(cl,0) = INTER_CZ_AUTH_OK;
					WFIFOW(cl,2) = 24 + sizeof(struct CharData);
					WFIFOL(cl,4) = account_id;
					WFIFOL(cl,8) = auth_nodes[account_id].login_id1;
					WFIFOL(cl,12) = auth_nodes[account_id].login_id2;
					WFIFOL(cl,16) = (unsigned int)auth_nodes[account_id].expiration_time; // FIXME: will wrap to negative after "19-Jan-2038, 03:14:07 AM GMT"
					WFIFOL(cl,20) = auth_nodes[account_id].gmlevel;
					memcpy(WFIFOP(cl,24), &char_dat, sizeof(struct CharData));
					cl->send_buffer(WFIFOW(cl,2));

					auth_nodes.erase(account_id);
					set_char_online(cl->tag(), char_id, account_id);
				}
				else
				{
					// auth failed
					WFIFOHEAD(cl,19);
					WFIFOW(cl,0) = INTER_CZ_AUTH_FAIL;
					WFIFOL(cl,2) = account_id;
					cl->send_buffer(6);
				}
			}
			break;

		default:
			ShowWarning("Unknown packet 0x%x sent from ZoneServer, closing connection.\n", cmd, cl->socket().remote_endpoint().address().to_string().c_str());
			cl->set_eof();
			return 0;
		}
	}

	return 0;
}

void CharServer::inter_reply_regs( tcp_connection::pointer cl, int account_id, int char_id, int type )
{
	static struct AccountReg reg;
	WFIFOHEAD(cl, 13 + 5000);
	chars->load_acc_reg(account_id, char_id, reg, type);

	WFIFOW(cl,0) = INTER_ZC_REQ_REGS_REPLY;
	WFIFOL(cl,4) = account_id;
	WFIFOL(cl,8) = char_id;
	WFIFOB(cl,12) = type;

	if(reg.reg_num==0)
	{
		WFIFOW(cl,2)=13;
	}
	else
	{
		int i, p;

		for (p = 13,i = 0; i < reg.reg_num && p < 5000; i++) 
		{
			p += sprintf((char*)WFIFOP(cl,p), "%s", reg.reg[i].str) + 1;
			p += sprintf((char*)WFIFOP(cl,p), "%s", reg.reg[i].value) + 1;
		}

		WFIFOW(cl,2)=p;

		if (p >= 5000)
			ShowWarning("Too many account regs for %d:%d, not all values were loaded.\n", account_id, char_id);
	}

	cl->send_buffer(WFIFOW(cl,2));
}
