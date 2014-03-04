/**
 * XMPP - libpurple transport
 *
 * Copyright (C) 2009, Jan Kaluza <hanzz@soc.pidgin.im>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 */

#include "transport/usersreconnecter.h"

#include <iostream>
#include <boost/bind.hpp>
#include "Swiften/Queries/IQRouter.h"
#include "transport/storagebackend.h"
#include "transport/transport.h"
#include "transport/logging.h"

#include "Swiften/Network/NetworkFactories.h"

using namespace Swift;
using namespace boost;

namespace Transport {

DEFINE_LOGGER(logger, "UserReconnecter");

UsersReconnecter::UsersReconnecter(Component *component, StorageBackend *storageBackend) {
	m_component = component;
	m_storageBackend = storageBackend;
	m_started = false;

	m_nextUserTimer = m_component->getNetworkFactories()->getTimerFactory()->createTimer(1000);
	m_nextUserTimer->onTick.connect(boost::bind(&UsersReconnecter::reconnectNextUser, this));

	m_component->onConnected.connect(bind(&UsersReconnecter::handleConnected, this));

	m_config = m_component->getConfig();
}

UsersReconnecter::~UsersReconnecter() {
	m_component->onConnected.disconnect(bind(&UsersReconnecter::handleConnected, this));
	m_nextUserTimer->stop();
	m_nextUserTimer->onTick.disconnect(boost::bind(&UsersReconnecter::reconnectNextUser, this));
}

void UsersReconnecter::reconnectNextUser() {
	if (m_users.empty()) {
		LOG4CXX_INFO(logger, "All users reconnected, stopping UserReconnecter.");
		return;
	}

	std::string jid = m_users.back();
	m_users.pop_back();

	if (CONFIG_BOOL(m_config, "service.reconnect_on_start")) {
	    UserInfo userInfo;
	    bool registered = m_storageBackend ? m_storageBackend->getUser(jid, userInfo) : false;

	    if (registered) {                
		std::string value = "1";
		int type = (int) TYPE_BOOLEAN;
		m_storageBackend->getUserSetting(userInfo.id, "stay_connected", type, value);

		if (value == "1") {
		    LOG4CXX_INFO(logger, "Reconnecting user " << jid);
		    Swift::Presence::ref presence = Swift::Presence::create();
		    presence->setTo(m_component->getJID());
		    presence->setFrom(jid);
		    presence->setType(Swift::Presence::Available);
		    m_component->onUserPresenceReceived(presence);
		} else {
			LOG4CXX_INFO(logger, "Skipping user " << jid << " (stay_connected != 1)");
		}
	    } else {
		LOG4CXX_INFO(logger, "Unknown user " << jid);
	    }
	} else {
	    LOG4CXX_INFO(logger, "Sending probe presence to " << jid);
	    Swift::Presence::ref response = Swift::Presence::create();
	    try {
		response->setTo(jid);
	    }
	    catch (...) { return; }

	    response->setFrom(m_component->getJID());
	    response->setType(Swift::Presence::Probe);

	    m_component->getStanzaChannel()->sendPresence(response);
	}
        
	m_nextUserTimer->start();
}

void UsersReconnecter::handleConnected() {
	if (m_started)
		return;

	LOG4CXX_INFO(logger, "Starting UserReconnecter.");
	m_started = true;

	if (CONFIG_BOOL(m_config, "service.reconnect_on_start")) {
		m_storageBackend->getAllUsers(m_users);
	} else {
		m_storageBackend->getOnlineUsers(m_users);
	}
	
	reconnectNextUser();
}


}
