/**
 * libtransport -- C++ library for easy XMPP Transports development
 *
 * Copyright (C) 2011, Jan Kaluza <hanzz.k@gmail.com>
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

#include "transport/user.h"
#include "transport/transport.h"
#include "transport/storagebackend.h"
#include "transport/rostermanager.h"
#include "transport/usermanager.h"
#include "transport/conversationmanager.h"
#include "transport/presenceoracle.h"
#include "transport/logging.h"
#include "Swiften/Server/ServerStanzaChannel.h"
#include "Swiften/Elements/StreamError.h"
#include "Swiften/Elements/MUCPayload.h"
#include "Swiften/Elements/SpectrumErrorPayload.h"
#include <boost/foreach.hpp>
#include <stdio.h>
#include <stdlib.h>

using namespace boost;

#define foreach         BOOST_FOREACH

namespace Transport {

DEFINE_LOGGER(logger, "User");

User::User(const Swift::JID &jid, UserInfo &userInfo, Component *component, UserManager *userManager) {
	m_jid = jid.toBare();
	m_data = NULL;

	m_cacheMessages = false;
	m_component = component;
	m_presenceOracle = component->m_presenceOracle;
	m_entityCapsManager = component->m_entityCapsManager;
	m_userManager = userManager;
	m_userInfo = userInfo;
	m_connected = false;
	m_readyForConnect = false;
	m_ignoreDisconnect = false;
	m_resources = 0;
	m_reconnectCounter = 0;

	m_reconnectTimer = m_component->getNetworkFactories()->getTimerFactory()->createTimer(5000);
	m_reconnectTimer->onTick.connect(boost::bind(&User::onConnectingTimeout, this)); 

	m_rosterManager = new RosterManager(this, m_component);
	m_conversationManager = new ConversationManager(this, m_component);
	LOG4CXX_INFO(logger, m_jid.toString() << ": Created");
	updateLastActivity();
}

User::~User(){
	LOG4CXX_INFO(logger, m_jid.toString() << ": Destroying");
	if (m_component->inServerMode()) {
		dynamic_cast<Swift::ServerStanzaChannel *>(m_component->getStanzaChannel())->finishSession(m_jid, boost::shared_ptr<Swift::Element>());
	}

	m_reconnectTimer->stop();
	delete m_rosterManager;
	delete m_conversationManager;
}

const Swift::JID &User::getJID() {
	return m_jid;
}

std::vector<Swift::JID> User::getJIDWithFeature(const std::string &feature) {
	std::vector<Swift::JID> jid;
	std::vector<Swift::Presence::ref> presences = m_presenceOracle->getAllPresence(m_jid);

	foreach(Swift::Presence::ref presence, presences) {
		if (presence->getType() == Swift::Presence::Unavailable)
			continue;

		Swift::DiscoInfo::ref discoInfo = m_entityCapsManager->getCaps(presence->getFrom());
		if (!discoInfo) {
#ifdef SUPPORT_LEGACY_CAPS
			if (m_legacyCaps.find(presence->getFrom()) != m_legacyCaps.end()) {
				discoInfo = m_legacyCaps[presence->getFrom()];
			}
			else {
				continue;
			}

			if (!discoInfo) {
				continue;
			}
#else
			continue;
#endif
		}

		if (discoInfo->hasFeature(feature)) {
			LOG4CXX_INFO(logger, m_jid.toString() << ": Found JID with " << feature << " feature: " << presence->getFrom().toString());
			jid.push_back(presence->getFrom());
		}
	}

	if (jid.empty()) {
		LOG4CXX_INFO(logger, m_jid.toString() << ": No JID with " << feature << " feature " << m_legacyCaps.size());
	}
	return jid;
}

Swift::DiscoInfo::ref User::getCaps(const Swift::JID &jid) const {
	Swift::DiscoInfo::ref discoInfo = m_entityCapsManager->getCaps(jid);
#ifdef SUPPORT_LEGACY_CAPS
	if (!discoInfo) {
		std::map<Swift::JID, Swift::DiscoInfo::ref>::const_iterator it = m_legacyCaps.find(jid);
		if (it != m_legacyCaps.end()) {
			discoInfo = it->second;
		}
	}
#endif
	return discoInfo;
}

void User::sendCurrentPresence() {
	if (m_component->inServerMode()) {
		return;
	}

	std::vector<Swift::Presence::ref> presences = m_presenceOracle->getAllPresence(m_jid);
	foreach(Swift::Presence::ref presence, presences) {
		if (presence->getType() == Swift::Presence::Unavailable) {
			continue;
		}

		if (m_connected) {
			Swift::Presence::ref highest = m_presenceOracle->getHighestPriorityPresence(m_jid.toBare());
			if (highest) {
				Swift::Presence::ref response = Swift::Presence::create(highest);
				response->setTo(presence->getFrom());
				response->setFrom(m_component->getJID());
				m_component->getStanzaChannel()->sendPresence(response);
			}
			else {
				Swift::Presence::ref response = Swift::Presence::create();
				response->setTo(presence->getFrom());
				response->setFrom(m_component->getJID());
				response->setType(Swift::Presence::Unavailable);
				m_component->getStanzaChannel()->sendPresence(response);
			}
		}
		else {
			Swift::Presence::ref response = Swift::Presence::create();
			response->setTo(presence->getFrom());
			response->setFrom(m_component->getJID());
			response->setType(Swift::Presence::Unavailable);
			response->setStatus("Connecting");
			m_component->getStanzaChannel()->sendPresence(response);
		}
	}
}

void User::setConnected(bool connected) {
	m_connected = connected;
	m_reconnectCounter = 0;
	setIgnoreDisconnect(false);
	updateLastActivity();

	sendCurrentPresence();

	if (m_connected) {
		BOOST_FOREACH(Swift::Presence::ref &presence, m_joinedRooms) {
			handlePresence(presence, true);
		}
	}
}

void User::setCacheMessages(bool cacheMessages) {
	if (m_component->inServerMode() && !m_cacheMessages && cacheMessages) {
		m_conversationManager->sendCachedChatMessages();
	}
	m_cacheMessages = cacheMessages;
}

void User::handlePresence(Swift::Presence::ref presence, bool forceJoin) {
	LOG4CXX_INFO(logger, "PRESENCE " << presence->getFrom().toString() << " " << presence->getTo().toString());
	if (!m_connected) {
		// we are not connected to legacy network, so we should do it when disco#info arrive :)
		if (m_readyForConnect == false) {
			
			// Forward status message to legacy network, but only if it's sent from active resource
// 					if (m_activeResource == presence->getFrom().getResource().getUTF8String()) {
// 						forwardStatus(presenceShow, stanzaStatus);
// 					}
			boost::shared_ptr<Swift::CapsInfo> capsInfo = presence->getPayload<Swift::CapsInfo>();
			if (capsInfo && capsInfo->getHash() == "sha-1") {
				if (m_entityCapsManager->getCaps(presence->getFrom()) != Swift::DiscoInfo::ref()) {
					LOG4CXX_INFO(logger, m_jid.toString() << ": Ready to be connected to legacy network");
					m_readyForConnect = true;
					onReadyToConnect();
				}
				else {
					m_reconnectTimer->start();
				}
			}
			else if (m_component->inServerMode()) {
				LOG4CXX_INFO(logger, m_jid.toString() << ": Ready to be connected to legacy network");
				m_readyForConnect = true;
				onReadyToConnect();
			}
			else {
				m_reconnectTimer->start();
			}
		}
	}

	
	if (!presence->getTo().getNode().empty()) {
		bool isMUC = presence->getPayload<Swift::MUCPayload>() != NULL || *presence->getTo().getNode().c_str() == '#';
		if (presence->getType() == Swift::Presence::Unavailable) {
			std::string room = Buddy::JIDToLegacyName(presence->getTo());
			Conversation *conv = m_conversationManager->getConversation(room);
			if (conv) {
				conv->removeJID(presence->getFrom());
				if (!conv->getJIDs().empty()) {
					return;
				}
			}
			else {
				return;
			}

			if (getUserSetting("stay_connected") != "1") {
				LOG4CXX_INFO(logger, m_jid.toString() << ": Going to left room " << room);
				onRawPresenceReceived(presence);
				onRoomLeft(room);

				BOOST_FOREACH(Swift::Presence::ref &p, m_joinedRooms) {
					if (p->getTo() == presence->getTo()) {
						m_joinedRooms.remove(p);
						break;
					}
				}

				if (conv) {
					m_conversationManager->removeConversation(conv);
					delete conv;
				}
			}

			return;
		}
		else if (isMUC) {
			// force connection to legacy network to let backend to handle auto-join on connect.
			if (!m_readyForConnect) {
				LOG4CXX_INFO(logger, m_jid.toString() << ": Ready to be connected to legacy network");
				m_readyForConnect = true;
				onReadyToConnect();
			}

			std::string room = Buddy::JIDToLegacyName(presence->getTo());
			std::string password = "";
			if (presence->getPayload<Swift::MUCPayload>() != NULL) {
				password = presence->getPayload<Swift::MUCPayload>()->getPassword() ? *presence->getPayload<Swift::MUCPayload>()->getPassword() : "";
			}

			Conversation *conv = m_conversationManager->getConversation(room);
			if (conv != NULL) {
				if (std::find(conv->getJIDs().begin(), conv->getJIDs().end(), presence->getFrom()) != conv->getJIDs().end()) {
					LOG4CXX_INFO(logger, m_jid.toString() << ": User has already tried to join room " << room << " as " << presence->getTo().getResource());
				}
				else {
					conv->addJID(presence->getFrom());
					conv->sendParticipants(presence->getFrom());
					conv->sendCachedMessages(presence->getFrom());
				}

				if (forceJoin) {
					onRawPresenceReceived(presence);
					onRoomJoined(presence->getFrom(), room, presence->getTo().getResource(), password);
				}
				return;
			}

			bool isInJoined = false;
			BOOST_FOREACH(Swift::Presence::ref &p, m_joinedRooms) {
				if (p->getTo() == presence->getTo()) {
					isInJoined = true;
				}
			}
			if (!isInJoined) {
				m_joinedRooms.push_back(presence);
			}

			if (!m_connected) {
				LOG4CXX_INFO(logger, m_jid.toString() << ": Joining room " << room << " postponed, because use is not connected to legacy network yet.");
				return;
			}

			LOG4CXX_INFO(logger, m_jid.toString() << ": Going to join room " << room << " as " << presence->getTo().getResource());

			conv = m_component->getFactory()->createConversation(m_conversationManager, room, true);
			m_conversationManager->addConversation(conv);
			conv->setNickname(presence->getTo().getResource());
			conv->addJID(presence->getFrom());

			onRawPresenceReceived(presence);
			onRoomJoined(presence->getFrom(), room, presence->getTo().getResource(), password);

			return;
		}

		onRawPresenceReceived(presence);
	}

	int currentResourcesCount = m_presenceOracle->getAllPresence(m_jid).size();

	m_conversationManager->resetResources();


	if (presence->getType() == Swift::Presence::Unavailable) {
		m_conversationManager->removeJID(presence->getFrom());

		std::string presences;
		std::vector<Swift::Presence::ref> ps = m_presenceOracle->getAllPresence(m_jid);
		BOOST_FOREACH(Swift::Presence::ref p, ps) {
			if (p != presence) {
				presences += p->getFrom().toString() + " ";
			}
		};

		if (!presences.empty()) {
			LOG4CXX_INFO(logger, m_jid.toString() << ": User is still connected from following clients: " << presences);
		}
		else {
			LOG4CXX_INFO(logger, m_jid.toString() << ": Last client disconnected");
		}
	}


	// User wants to disconnect this resource
	if (!m_component->inServerMode()) {
		if (presence->getType() == Swift::Presence::Unavailable) {
				// Send unavailable presences for online contacts
				m_rosterManager->sendUnavailablePresences(presence->getFrom());

				// Send unavailable presence for transport contact itself
				Swift::Presence::ref response = Swift::Presence::create();
				response->setTo(presence->getFrom());
				response->setFrom(m_component->getJID());
				response->setType(Swift::Presence::Unavailable);
				m_component->getStanzaChannel()->sendPresence(response);
		}
		else {
			sendCurrentPresence();
		}
	}

	// This resource is new, so we have to send buddies presences
	if (presence->getType() != Swift::Presence::Unavailable && currentResourcesCount != m_resources) {
		m_rosterManager->sendCurrentPresences(presence->getFrom());
	}

	m_resources = currentResourcesCount;

	// Change legacy network presence
	if (m_readyForConnect) {
		Swift::Presence::ref highest = m_presenceOracle->getHighestPriorityPresence(m_jid.toBare());
		if (highest) {
			if (highest->getType() == Swift::Presence::Unavailable && getUserSetting("stay_connected") == "1") {
				if (getUserSetting("xa_when_offline") == "1") {
					highest->setType(Swift::Presence::Available);
					highest->setShow(Swift::StatusShow::XA);
					onRawPresenceReceived(highest);
				}

				m_resources = 0;
				m_conversationManager->clearJIDs();
				setCacheMessages(true);
				return;
			}
			Swift::Presence::ref response = Swift::Presence::create(highest);
			response->setTo(m_jid);
			response->setFrom(m_component->getJID());
			LOG4CXX_INFO(logger, m_jid.toString() << ": Changing legacy network presence to " << response->getType());
			onPresenceChanged(highest);
			setCacheMessages(false);
		}
		else {
			if (getUserSetting("stay_connected") == "1") {
				m_resources = 0;
				m_conversationManager->clearJIDs();
				setCacheMessages(true);
				return;
			}
			Swift::Presence::ref response = Swift::Presence::create();
			response->setTo(m_jid.toBare());
			response->setFrom(m_component->getJID());
			response->setType(Swift::Presence::Unavailable);
			onPresenceChanged(response);
		}
	}
}

void User::handleSubscription(Swift::Presence::ref presence) {
	m_rosterManager->handleSubscription(presence);
}

void User::handleDiscoInfo(const Swift::JID& jid, boost::shared_ptr<Swift::DiscoInfo> info) {
	LOG4CXX_INFO(logger, jid.toString() << ": got disco#info");
#ifdef SUPPORT_LEGACY_CAPS
	Swift::DiscoInfo::ref discoInfo = m_entityCapsManager->getCaps(jid);
	// This is old legacy cap which is not stored in entityCapsManager,
	// we have to store it in our user class.
	if (!discoInfo) {
		LOG4CXX_INFO(logger, jid.toString() << ": LEGACY");
		m_legacyCaps[jid] = info;
	}
#endif

	onConnectingTimeout();
}

void User::onConnectingTimeout() {
	if (m_connected || m_readyForConnect)
		return;
	m_reconnectTimer->stop();
	m_readyForConnect = true;
	onReadyToConnect();

	Swift::Presence::ref highest = m_presenceOracle->getHighestPriorityPresence(m_jid.toBare());
	if (highest) {
		LOG4CXX_INFO(logger, m_jid.toString() << ": Changing legacy network presence to " << highest->getType());
		onPresenceChanged(highest);
	}
}

void User::setIgnoreDisconnect(bool ignoreDisconnect) {
	m_ignoreDisconnect = ignoreDisconnect;
	LOG4CXX_INFO(logger, m_jid.toString() << ": Setting ignoreDisconnect=" << m_ignoreDisconnect);
}

void User::handleDisconnected(const std::string &error, Swift::SpectrumErrorPayload::Error e) {
	if (m_ignoreDisconnect) {
		LOG4CXX_INFO(logger, m_jid.toString() << ": Disconnecting from legacy network ignored (probably moving between backends)");
		return;
	}

	if (e == Swift::SpectrumErrorPayload::CONNECTION_ERROR_OTHER_ERROR || e == Swift::SpectrumErrorPayload::CONNECTION_ERROR_NETWORK_ERROR) {
		if (m_reconnectCounter < 3) {
			m_reconnectCounter++;
			LOG4CXX_INFO(logger, m_jid.toString() << ": Disconnecting from legacy network " << error << ", trying to reconnect automatically.");
			// Simulate destruction/resurrection :)
			// TODO: If this stops working, create onReconnect signal
			m_userManager->onUserDestroyed(this);
			m_userManager->onUserCreated(this);
			onReadyToConnect();
			return;
		}
	}

	if (error.empty()) {
		LOG4CXX_INFO(logger, m_jid.toString() << ": Disconnected from legacy network");
	}
	else {
		LOG4CXX_INFO(logger, m_jid.toString() << ": Disconnected from legacy network with error " << error);
	}
	onDisconnected();

	boost::shared_ptr<Swift::Message> msg(new Swift::Message());
	msg->setBody(error);
	msg->setTo(m_jid.toBare());
	msg->setFrom(m_component->getJID());
	msg->addPayload(boost::make_shared<Swift::SpectrumErrorPayload>(e));
	m_component->getStanzaChannel()->sendMessage(msg);

	// In server mode, server finishes the session and pass unavailable session to userManager if we're connected to legacy network,
	// so we can't removeUser() in server mode, because it would be removed twice.
	// Once in finishSession and once in m_userManager->removeUser.
	if (m_component->inServerMode()) {
		// Remove user later just to be sure there won't be double-free.
		// We can't be sure finishSession sends unavailable presence everytime, so check if user gets removed
		// in finishSession(...) call and if not, remove it here.
		std::string jid = m_jid.toBare().toString();		
		dynamic_cast<Swift::ServerStanzaChannel *>(m_component->getStanzaChannel())->finishSession(m_jid, boost::shared_ptr<Swift::Element>(new Swift::StreamError(Swift::StreamError::UndefinedCondition, error)));
		if (m_userManager->getUser(jid) != NULL) {
			m_userManager->removeUser(this);
		}
	}
	else {
		m_userManager->removeUser(this);
	}
}

}
