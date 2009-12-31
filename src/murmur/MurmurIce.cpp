/* Copyright (C) 2005-2009, Thorvald Natvig <thorvald@natvig.com>

   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.
   - Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.
   - Neither the name of the Mumble Developers nor the names of its
     contributors may be used to endorse or promote products derived from this
     software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include <Ice/Ice.h>
#include <IceUtil/IceUtil.h>
#include "Meta.h"
#include "Server.h"
#include "ServerDB.h"
#include "User.h"
#include "Channel.h"
#include "Group.h"
#include "MurmurIce.h"
#include "MurmurI.h"

using namespace std;
using namespace Murmur;

static MurmurIce *mi = NULL;
static Ice::ObjectPtr iopServer;
static Ice::PropertiesPtr ippProperties;

void IceParse(int &argc, char *argv[]) {
	ippProperties = Ice::createProperties(argc, argv);
}

void IceStart() {
	mi = new MurmurIce();
}

void IceStop() {
	if (mi)
		delete mi;
	mi = NULL;
}

static void logToLog(const ServerDB::LogRecord &r, Murmur::LogEntry &le) {
	le.timestamp = r.first;
	le.txt = u8(r.second);
}

static void userToUser(const ::User *p, Murmur::User &mp) {
	mp.session = p->uiSession;
	mp.userid = p->iId;
	mp.name = u8(p->qsName);
	mp.mute = p->bMute;
	mp.deaf = p->bDeaf;
	mp.suppress = p->bSuppress;
	mp.selfMute = p->bSelfMute;
	mp.selfDeaf = p->bSelfDeaf;
	mp.channel = p->cChannel->iId;
	mp.comment = u8(p->qsComment);

	const ServerUser *u=static_cast<const ServerUser *>(p);
	mp.onlinesecs = u->bwr.onlineSeconds();
	mp.bytespersec = u->bwr.bandwidth();
	mp.version = u->uiVersion;
	mp.release = u8(u->qsRelease);
	mp.os = u8(u->qsOS);
	mp.osversion = u8(u->qsOSVersion);
	mp.identity = u8(u->qsIdentity);
	mp.context = u->ssContext;
	mp.idlesecs = u->bwr.idleSeconds();

	mp.tcponly = ! u->bUdp;

	::Murmur::NetAddress addr(16, 0);
	const Q_IPV6ADDR &a = u->haAddress.qip6;
	for (int i=0;i<16;++i)
		addr[i] = a[i];

	mp.address = addr;
}

static void channelToChannel(const ::Channel *c, Murmur::Channel &mc) {
	mc.id = c->iId;
	mc.name = u8(c->qsName);
	mc.parent = c->cParent ? c->cParent->iId : -1;
	mc.description = u8(c->qsDesc);
	mc.position = c->iPosition;
	mc.links.clear();
	foreach(::Channel *chn, c->qsPermLinks)
		mc.links.push_back(chn->iId);
	mc.temporary = c->bTemporary;
}

static void ACLtoACL(const ::ChanACL *acl, Murmur::ACL &ma) {
	ma.applyHere = acl->bApplyHere;
	ma.applySubs = acl->bApplySubs;
	ma.inherited = false;
	ma.userid = acl->iUserId;
	ma.group = u8(acl->qsGroup);
	ma.allow = acl->pAllow;
	ma.deny = acl->pDeny;
}

static void groupToGroup(const ::Group *g, Murmur::Group &mg) {
	mg.name = u8(g->qsName);
	mg.inherit = g->bInherit;
	mg.inheritable = g->bInheritable;
	mg.add.clear();
	mg.remove.clear();
	mg.members.clear();
}

static void banToBan(const ::Ban &b, Murmur::Ban &mb) {
	::Murmur::NetAddress addr(16, 0);
	const Q_IPV6ADDR &a = b.haAddress.qip6;
	for (int i=0;i<16;++i)
		addr[i] = a[i];

	mb.address = addr;
	mb.bits = b.iMask;
	mb.name = u8(b.qsUsername);
	mb.hash = u8(b.qsHash);
	mb.reason = u8(b.qsReason);
	mb.start = b.qdtStart.toTime_t();
	mb.duration = b.iDuration;
}

static void banToBan(const ::Murmur::Ban &mb, ::Ban &b) {
	if (mb.address.size() != 16)
		for (int i=0;i<16;++i)
			b.haAddress.qip6[i] = 0;
	else
		for (int i=0;i<16;++i)
			b.haAddress.qip6[i] = mb.address[i];
	b.iMask = mb.bits;
	b.qsUsername = u8(mb.name);
	b.qsHash = u8(mb.hash);
	b.qsReason = u8(mb.reason);
	b.qdtStart = QDateTime::fromTime_t(static_cast<quint32>(mb.start));
	b.iDuration = mb.duration;
}

static void infoToInfo(const QMap<int, QString> &info, Murmur::UserInfoMap &im) {
	QMap<int, QString>::const_iterator i;
	for (i = info.constBegin(); i != info.constEnd(); ++i)
		im[static_cast<Murmur::UserInfo>(i.key())] = u8(i.value());
}

static void infoToInfo(const Murmur::UserInfoMap &im, QMap<int, QString> &info) {
	Murmur::UserInfoMap::const_iterator i;
	for (i = im.begin(); i != im.end(); ++i)
		info.insert((*i).first, u8((*i).second));
}

class ServerLocator : public virtual Ice::ServantLocator {
	public:
		virtual Ice::ObjectPtr locate(const Ice::Current &, Ice::LocalObjectPtr &);
		virtual void finished(const Ice::Current &, const Ice::ObjectPtr &, const Ice::LocalObjectPtr &) {};
		virtual void deactivate(const std::string &) {};
};

MurmurIce::MurmurIce() {
	count = 0;

	if (meta->mp.qsIceEndpoint.isEmpty())
		return;

	Ice::PropertiesPtr ipp = Ice::createProperties();

	::Meta::mp.qsSettings->beginGroup("Ice");
	foreach(const QString &v, ::Meta::mp.qsSettings->childKeys()) {
		ipp->setProperty(u8(v), u8(::Meta::mp.qsSettings->value(v).toString()));
	}
	::Meta::mp.qsSettings->endGroup();

	Ice::PropertyDict props = ippProperties->getPropertiesForPrefix("");
	Ice::PropertyDict::iterator i;
	for (i=props.begin(); i != props.end(); ++i) {
		ipp->setProperty((*i).first, (*i).second);
	}

	Ice::InitializationData idd;
	idd.properties = ipp;

	try {
		communicator = Ice::initialize(idd);
		adapter = communicator->createObjectAdapterWithEndpoints("Murmur", qPrintable(meta->mp.qsIceEndpoint));
		MetaPtr m = new MetaI;
		MetaPrx mprx = MetaPrx::uncheckedCast(adapter->add(m, communicator->stringToIdentity("Meta")));
		adapter->addServantLocator(new ServerLocator(), "s");

		iopServer = new ServerI;

		adapter->activate();
		foreach(const Ice::EndpointPtr ep, mprx->ice_getEndpoints()) {
			qWarning("MurmurIce: Endpoint \"%s\" running", qPrintable(u8(ep->toString())));
		}

		meta->connectListener(this);
	} catch (Ice::Exception &e) {
		qCritical("MurmurIce: Initialization failed: %s", qPrintable(u8(e.ice_name())));
	}
}

MurmurIce::~MurmurIce() {
	if (communicator) {
		communicator->shutdown();
		communicator->waitForShutdown();
		communicator->destroy();
		communicator=NULL;
		qWarning("MurmurIce: Shutdown complete");
	}
	iopServer = NULL;
}

void MurmurIce::customEvent(QEvent *evt) {
	if (evt->type() == EXEC_QEVENT)
		static_cast<ExecEvent *>(evt)->execute();
}

void MurmurIce::badMetaProxy(const ::Murmur::MetaCallbackPrx &prx) {
	qCritical("Registered Ice MetaCallback %s failed", qPrintable(QString::fromStdString(communicator->proxyToString(prx))));
	qmMetaCallbacks.removeAll(prx);
}

void MurmurIce::badServerProxy(const ::Murmur::ServerCallbackPrx &prx, int id) {
	qCritical("Registered Ice ServerCallback %s on server %d failed", qPrintable(QString::fromStdString(communicator->proxyToString(prx))), id);
	qmServerCallbacks[id].removeAll(prx);
}

void MurmurIce::badAuthenticator(::Server *server) {
	server->disconnectAuthenticator(this);
	const ::Murmur::ServerAuthenticatorPrx &prx = qmServerAuthenticator.value(server->iServerNum);
	qCritical("Registered Ice Authenticator %s on server %d failed", qPrintable(QString::fromStdString(communicator->proxyToString(prx))), server->iServerNum);
	qmServerAuthenticator.remove(server->iServerNum);
	qmServerUpdatingAuthenticator.remove(server->iServerNum);
}

static ServerPrx idToProxy(int id, const Ice::ObjectAdapterPtr &adapter) {
	Ice::Identity ident;
	ident.category = "s";
	ident.name = u8(QString::number(id));

	return ServerPrx::uncheckedCast(adapter->createProxy(ident));
}

void MurmurIce::started(::Server *s) {
	s->connectListener(mi);
	connect(s, SIGNAL(contextAction(const User *, const QString &, unsigned int, int)), this, SLOT(contextAction(const User *, const QString &, unsigned int, int)));

	const QList< ::Murmur::MetaCallbackPrx> &qmList = qmMetaCallbacks;

	if (qmList.isEmpty())
		return;

	foreach(const ::Murmur::MetaCallbackPrx &prx, qmList) {
		try {
			prx->started(idToProxy(s->iServerNum, adapter));
		} catch (...) {
			badMetaProxy(prx);
		}
	}
}

void MurmurIce::stopped(::Server *s) {
	qmServerCallbacks.remove(s->iServerNum);
	qmServerAuthenticator.remove(s->iServerNum);
	qmServerUpdatingAuthenticator.remove(s->iServerNum);

	const QList< ::Murmur::MetaCallbackPrx> &qmList = qmMetaCallbacks;

	if (qmList.isEmpty())
		return;

	foreach(const ::Murmur::MetaCallbackPrx &prx, qmList) {
		try {
			prx->stopped(idToProxy(s->iServerNum, adapter));
		} catch (...) {
			badMetaProxy(prx);
		}
	}
}

void MurmurIce::userConnected(const ::User *p) {
	::Server *s = qobject_cast< ::Server *> (sender());

	const QList< ::Murmur::ServerCallbackPrx> &qmList = qmServerCallbacks[s->iServerNum];

	if (qmList.isEmpty())
		return;

	::Murmur::User mp;
	userToUser(p, mp);

	foreach(const ::Murmur::ServerCallbackPrx &prx, qmList) {
		try {
			prx->userConnected(mp);
		} catch (...) {
			badServerProxy(prx, s->iServerNum);
		}
	}
}

void MurmurIce::userDisconnected(const ::User *p) {
	::Server *s = qobject_cast< ::Server *> (sender());

	mi->qmServerContextCallbacks[s->iServerNum].remove(p->uiSession);

	const QList< ::Murmur::ServerCallbackPrx> &qmList = qmServerCallbacks[s->iServerNum];

	if (qmList.isEmpty())
		return;

	::Murmur::User mp;
	userToUser(p, mp);

	foreach(const ::Murmur::ServerCallbackPrx &prx, qmList) {
		try {
			prx->userDisconnected(mp);
		} catch (...) {
			badServerProxy(prx, s->iServerNum);
		}
	}
}

void MurmurIce::userStateChanged(const ::User *p) {
	::Server *s = qobject_cast< ::Server *> (sender());

	const QList< ::Murmur::ServerCallbackPrx> &qmList = qmServerCallbacks[s->iServerNum];

	if (qmList.isEmpty())
		return;

	::Murmur::User mp;
	userToUser(p, mp);

	foreach(const ::Murmur::ServerCallbackPrx &prx, qmList) {
		try {
			prx->userStateChanged(mp);
		} catch (...) {
			badServerProxy(prx, s->iServerNum);
		}
	}
}

void MurmurIce::channelCreated(const ::Channel *c) {
	::Server *s = qobject_cast< ::Server *> (sender());

	const QList< ::Murmur::ServerCallbackPrx> &qmList = qmServerCallbacks[s->iServerNum];

	if (qmList.isEmpty())
		return;

	::Murmur::Channel mc;
	channelToChannel(c, mc);

	foreach(const ::Murmur::ServerCallbackPrx &prx, qmList) {
		try {
			prx->channelCreated(mc);
		} catch (...) {
			badServerProxy(prx, s->iServerNum);
		}
	}
}

void MurmurIce::channelRemoved(const ::Channel *c) {
	::Server *s = qobject_cast< ::Server *> (sender());

	const QList< ::Murmur::ServerCallbackPrx> &qmList = qmServerCallbacks[s->iServerNum];

	if (qmList.isEmpty())
		return;

	::Murmur::Channel mc;
	channelToChannel(c, mc);

	foreach(const ::Murmur::ServerCallbackPrx &prx, qmList) {
		try {
			prx->channelRemoved(mc);
		} catch (...) {
			badServerProxy(prx, s->iServerNum);
		}
	}
}

void MurmurIce::channelStateChanged(const ::Channel *c) {
	::Server *s = qobject_cast< ::Server *> (sender());

	const QList< ::Murmur::ServerCallbackPrx> &qmList = qmServerCallbacks[s->iServerNum];

	if (qmList.isEmpty())
		return;

	::Murmur::Channel mc;
	channelToChannel(c, mc);

	foreach(const ::Murmur::ServerCallbackPrx &prx, qmList) {
		try {
			prx->channelStateChanged(mc);
		} catch (...) {
			badServerProxy(prx, s->iServerNum);
		}
	}
}

void MurmurIce::contextAction(const ::User *pSrc, const QString &action, unsigned int session, int iChannel) {
	::Server *s = qobject_cast< ::Server *> (sender());

	QMap<int, QMap<int, QMap<QString, ::Murmur::ServerContextCallbackPrx> > > &qmAll = mi->qmServerContextCallbacks;
	if (! qmAll.contains(s->iServerNum))
		return;

	QMap<int, QMap<QString, ::Murmur::ServerContextCallbackPrx> > &qmServer = qmAll[s->iServerNum];
	if (! qmServer.contains(pSrc->uiSession))
		return;

	QMap<QString, ::Murmur::ServerContextCallbackPrx> &qmUser = qmServer[pSrc->uiSession];
	if (! qmUser.contains(action))
		return;

	const ::Murmur::ServerContextCallbackPrx &prx = qmUser[action];

	::Murmur::User mp;
	userToUser(pSrc, mp);

	try {
		prx->contextAction(u8(action), mp, session, iChannel);
	} catch (...) {
		qCritical("Registered Ice ServerContextCallback %s on server %d, session %d, action %s failed", qPrintable(QString::fromStdString(communicator->proxyToString(prx))), s->iServerNum, pSrc->uiSession, qPrintable(action));
		qmUser.remove(action);
	}
}

void MurmurIce::idToNameSlot(QString &name, int id) {
	::Server *server = qobject_cast< ::Server *> (sender());

	ServerAuthenticatorPrx prx = mi->qmServerAuthenticator.value(server->iServerNum);
	try {
		name = u8(prx->idToName(id));
	} catch (...) {
		badAuthenticator(server);
	}
}
void MurmurIce::idToTextureSlot(QByteArray &qba, int id) {
	::Server *server = qobject_cast< ::Server *> (sender());

	ServerAuthenticatorPrx prx = mi->qmServerAuthenticator.value(server->iServerNum);
	try {
		const ::Murmur::Texture &tex = prx->idToTexture(id);

		qba.resize(static_cast<int>(tex.size()));
		char *ptr = qba.data();
		for (unsigned int i=0;i<tex.size();++i)
			ptr[i] = tex[i];
	} catch (...) {
		badAuthenticator(server);
	}
}

void MurmurIce::nameToIdSlot(int &id, const QString &name) {
	::Server *server = qobject_cast< ::Server *> (sender());

	ServerAuthenticatorPrx prx = mi->qmServerAuthenticator.value(server->iServerNum);
	try {
		id = prx->nameToId(u8(name));
	} catch (...) {
		badAuthenticator(server);
	}
}

void MurmurIce::authenticateSlot(int &res, QString &uname, const QList<QSslCertificate> &certlist, const QString &certhash, bool certstrong, const QString &pw) {
	::Server *server = qobject_cast< ::Server *> (sender());

	ServerAuthenticatorPrx prx = mi->qmServerAuthenticator.value(server->iServerNum);
	::std::string newname;
	::Murmur::GroupNameList groups;
	::Murmur::CertificateList certs;

	certs.resize(certlist.size());
	for (int i=0;i<certlist.size();++i) {
		::Murmur::CertificateDer der;
		QByteArray qba = certlist.at(i).toDer();
		der.resize(qba.size());
		const char *ptr = qba.constData();
		for (int j=0;j<qba.size();++j)
			der[j] = ptr[j];
		certs[i] = der;
	}

	try {
		res = prx->authenticate(u8(uname), u8(pw), certs, u8(certhash), certstrong, newname, groups);
	} catch (...) {
		badAuthenticator(server);
	}
	if (res >= 0) {
		if (newname.length() > 0)
			uname = u8(newname);
		QStringList qsl;
		foreach(const ::std::string &str, groups) {
			qsl << u8(str);
		}
		if (! qsl.isEmpty())
			server->setTempGroups(res, NULL, qsl);
	}
}

void MurmurIce::registerUserSlot(int &res, const QMap<int, QString> &info) {
	::Server *server = qobject_cast< ::Server *> (sender());

	ServerUpdatingAuthenticatorPrx prx = mi->qmServerUpdatingAuthenticator.value(server->iServerNum);
	if (! prx)
		return;

	::Murmur::UserInfoMap im;

	infoToInfo(info, im);
	try {
		res = prx->registerUser(im);
	} catch (...) {
		badAuthenticator(server);
	}
}

void MurmurIce::unregisterUserSlot(int &res, int id) {
	::Server *server = qobject_cast< ::Server *> (sender());

	ServerUpdatingAuthenticatorPrx prx = mi->qmServerUpdatingAuthenticator.value(server->iServerNum);
	if (! prx)
		return;
	try {
		res = prx->unregisterUser(id);
	} catch (...) {
		badAuthenticator(server);
	}
}

void MurmurIce::getRegistrationSlot(int &res, int id, QMap<int, QString> &info) {
	::Server *server = qobject_cast< ::Server *> (sender());

	ServerUpdatingAuthenticatorPrx prx = mi->qmServerUpdatingAuthenticator.value(server->iServerNum);
	if (! prx)
		return;

	Murmur::UserInfoMap im;
	try {
		if (prx->getInfo(id, im)) {
			res = 1;
			infoToInfo(im, info);
		}
	} catch (...) {
		badAuthenticator(server);
		return;
	}
}

void  MurmurIce::getRegisteredUsersSlot(const QString &filter, QMap<int, QString> &m) {
	::Server *server = qobject_cast< ::Server *> (sender());

	ServerUpdatingAuthenticatorPrx prx = mi->qmServerUpdatingAuthenticator.value(server->iServerNum);
	if (! prx)
		return;

	::Murmur::NameMap lst;

	try {
		lst = prx->getRegisteredUsers(u8(filter));
	} catch (...) {
		badAuthenticator(server);
		return;
	}
	::Murmur::NameMap::const_iterator i;
	for (i=lst.begin(); i != lst.end(); ++i)
		m.insert((*i).first, u8((*i).second));
}

void MurmurIce::setInfoSlot(int &res, int id, const QMap<int, QString> &info) {
	::Server *server = qobject_cast< ::Server *> (sender());

	ServerUpdatingAuthenticatorPrx prx = mi->qmServerUpdatingAuthenticator.value(server->iServerNum);
	if (! prx)
		return;

	Murmur::UserInfoMap im;
	infoToInfo(info, im);

	try {
		res = prx->setInfo(id, im);
	} catch (...) {
		badAuthenticator(server);
	}
}

void MurmurIce::setTextureSlot(int &res, int id, const QByteArray &texture) {
	::Server *server = qobject_cast< ::Server *> (sender());

	ServerUpdatingAuthenticatorPrx prx = mi->qmServerUpdatingAuthenticator.value(server->iServerNum);
	if (! prx)
		return;

	::Murmur::Texture tex;
	tex.resize(texture.size());
	const char *ptr = texture.constData();
	for (int i=0;i<texture.size();++i)
		tex[i] = ptr[i];

	try {
		res = prx->setTexture(id, tex);
	} catch (...) {
		badAuthenticator(server);
	}
}

Ice::ObjectPtr ServerLocator::locate(const Ice::Current &, Ice::LocalObjectPtr &) {
	return iopServer;
}

#define FIND_SERVER \
	::Server *server = meta->qhServers.value(server_id);

#define NEED_SERVER_EXISTS \
	FIND_SERVER \
	if (!server && ! ServerDB::serverExists(server_id)) { \
		cb->ice_exception(::Ice::ObjectNotExistException(__FILE__,__LINE__)); \
		return; \
	}

#define NEED_SERVER \
	NEED_SERVER_EXISTS \
	if (! server) { \
		cb->ice_exception(ServerBootedException()); \
		return; \
	}

#define NEED_PLAYER \
	ServerUser *user = server->qhUsers.value(session); \
	if (!user) { \
		cb->ice_exception(::Murmur::InvalidSessionException()); \
		return; \
	}

#define NEED_CHANNEL_VAR(x,y) \
	x = server->qhChannels.value(y); \
	if (!x) { \
		cb->ice_exception(::Murmur::InvalidChannelException()); \
		return; \
	}

#define NEED_CHANNEL \
	::Channel *channel; \
	NEED_CHANNEL_VAR(channel, channelid);

void ServerI::ice_ping(const Ice::Current &current) const {
	// This is executed in the ice thread.
	int server_id = u8(current.id.name).toInt();
	if (! ServerDB::serverExists(server_id))
		throw ::Ice::ObjectNotExistException(__FILE__, __LINE__);
}

static void impl_Server_isRunning(const ::Murmur::AMD_Server_isRunningPtr cb, int server_id) {
	NEED_SERVER_EXISTS;
	cb->ice_response(server != NULL);
}

static void impl_Server_start(const ::Murmur::AMD_Server_startPtr cb, int server_id) {
	NEED_SERVER_EXISTS;
	if (server)
		cb->ice_exception(ServerBootedException());
	else if (! meta->boot(server_id))
		cb->ice_exception(ServerFailureException());
	else
		cb->ice_response();
}

static void impl_Server_stop(const ::Murmur::AMD_Server_stopPtr cb, int server_id) {
	NEED_SERVER;
	meta->kill(server_id);
	cb->ice_response();
}

static void impl_Server_delete(const ::Murmur::AMD_Server_deletePtr cb, int server_id) {
	NEED_SERVER_EXISTS;
	if (server) {
		cb->ice_exception(ServerBootedException());
		return;
	}
	ServerDB::deleteServer(server_id);
	cb->ice_response();
}

static void impl_Server_addCallback(const Murmur::AMD_Server_addCallbackPtr cb, int server_id, const Murmur::ServerCallbackPrx& cbptr) {
	NEED_SERVER;
	QList< ::Murmur::ServerCallbackPrx> &qmList = mi->qmServerCallbacks[server_id];

	try {
		const Murmur::ServerCallbackPrx &oneway = Murmur::ServerCallbackPrx::checkedCast(cbptr->ice_oneway()->ice_connectionCached(false));
		if (! qmList.contains(oneway))
			qmList.append(oneway);
		cb->ice_response();
	} catch (...) {
		cb->ice_exception(InvalidCallbackException());
	}
}

static void impl_Server_removeCallback(const Murmur::AMD_Server_removeCallbackPtr cb, int server_id, const Murmur::ServerCallbackPrx& cbptr) {
	NEED_SERVER;
	QList< ::Murmur::ServerCallbackPrx> &qmList = mi->qmServerCallbacks[server_id];

	try {
		const Murmur::ServerCallbackPrx &oneway = Murmur::ServerCallbackPrx::uncheckedCast(cbptr->ice_oneway()->ice_connectionCached(false));
		qmList.removeAll(oneway);
		cb->ice_response();
	} catch (...) {
		cb->ice_exception(InvalidCallbackException());
	}
}

static void impl_Server_setAuthenticator(const ::Murmur::AMD_Server_setAuthenticatorPtr& cb, int server_id, const ::Murmur::ServerAuthenticatorPrx &aptr) {
	NEED_SERVER;

	if (mi->qmServerAuthenticator[server_id])
		server->disconnectAuthenticator(mi);

	::Murmur::ServerAuthenticatorPrx prx;

	try {
		prx = ::Murmur::ServerAuthenticatorPrx::checkedCast(aptr->ice_connectionCached(false));
		const ::Murmur::ServerUpdatingAuthenticatorPrx uprx = ::Murmur::ServerUpdatingAuthenticatorPrx::checkedCast(prx);

		mi->qmServerAuthenticator[server_id] = prx;
		if (uprx)
			mi->qmServerUpdatingAuthenticator[server_id] = uprx;
	} catch (...) {
		cb->ice_exception(InvalidCallbackException());
		return;
	}

	if (prx)
		server->connectAuthenticator(mi);

	cb->ice_response();

	server->log(QString("Registered Ice Authenticator %1").arg(QString::fromStdString(mi->communicator->proxyToString(aptr))));
}

static void impl_Server_id(const ::Murmur::AMD_Server_idPtr cb, int server_id) {
	NEED_SERVER_EXISTS;
	cb->ice_response(server_id);
}

static void impl_Server_getConf(const ::Murmur::AMD_Server_getConfPtr cb, int server_id,  const ::std::string& key) {
	NEED_SERVER_EXISTS;
	cb->ice_response(u8(ServerDB::getConf(server_id, u8(key)).toString()));
}

static void impl_Server_getAllConf(const ::Murmur::AMD_Server_getAllConfPtr cb, int server_id) {
	NEED_SERVER_EXISTS;

	::Murmur::ConfigMap cm;

	QMap<QString, QString> values = ServerDB::getAllConf(server_id);
	QMap<QString, QString>::const_iterator i;
	for (i=values.constBegin();i != values.constEnd(); ++i) {
		cm[u8(i.key())] = u8(i.value());
	}
	cb->ice_response(cm);
}

static void impl_Server_setConf(const ::Murmur::AMD_Server_setConfPtr cb, int server_id,  const ::std::string& key,  const ::std::string& value) {
	NEED_SERVER_EXISTS;
	QString k=u8(key);
	QString v=u8(value);
	ServerDB::setConf(server_id, k, v);
	if (server)
		server->setLiveConf(k, v);
	cb->ice_response();
}

static void impl_Server_setSuperuserPassword(const ::Murmur::AMD_Server_setSuperuserPasswordPtr cb, int server_id,  const ::std::string& pw) {
	NEED_SERVER_EXISTS;
	ServerDB::setSUPW(server_id, u8(pw));
	cb->ice_response();
}

static void impl_Server_getLog(const ::Murmur::AMD_Server_getLogPtr cb, int server_id,  ::Ice::Int min,  ::Ice::Int max) {
	NEED_SERVER_EXISTS;

	::Murmur::LogList ll;

	QList<ServerDB::LogRecord> dblog = ServerDB::getLog(server_id, min, max);
	foreach(const ServerDB::LogRecord &e, dblog) {
		::Murmur::LogEntry le;
		logToLog(e, le);
		ll.push_back(le);
	}
	cb->ice_response(ll);
}

static void impl_Server_getUsers(const ::Murmur::AMD_Server_getUsersPtr cb, int server_id) {
	NEED_SERVER;
	::Murmur::UserMap pm;
	foreach(const ::User *p, server->qhUsers) {
		::Murmur::User mp;
		if (static_cast<const ServerUser *>(p)->sState == ::ServerUser::Authenticated) {
			userToUser(p, mp);
			pm[p->uiSession] = mp;
		}
	}
	cb->ice_response(pm);
}

static void impl_Server_getChannels(const ::Murmur::AMD_Server_getChannelsPtr cb, int server_id) {
	NEED_SERVER;
	::Murmur::ChannelMap cm;
	foreach(const ::Channel *c, server->qhChannels) {
		::Murmur::Channel mc;
		channelToChannel(c, mc);
		cm[c->iId] = mc;
	}
	cb->ice_response(cm);
}

static bool userSort(const ::User *a, const ::User *b) {
	return a->qsName < b->qsName;
}

static bool channelSort(const ::Channel *a, const ::Channel *b) {
	return ::Channel::lessThan(a, b);
}

TreePtr recurseTree(const ::Channel *c) {
	TreePtr t = new Tree();
	channelToChannel(c, t->c);
	QList< ::User *> users = c->qlUsers;
	qSort(users.begin(), users.end(), userSort);

	foreach(const ::User *p, users) {
		::Murmur::User mp;
		userToUser(p, mp);
		t->users.push_back(mp);
	}

	QList< ::Channel *> channels = c->qlChannels;
	qSort(channels.begin(), channels.end(), channelSort);

	foreach(const ::Channel *chn, channels) {
		t->children.push_back(recurseTree(chn));
	}

	return t;
}

static void impl_Server_getTree(const ::Murmur::AMD_Server_getTreePtr cb, int server_id) {
	NEED_SERVER;
	cb->ice_response(recurseTree(server->qhChannels.value(0)));
}

static void impl_Server_getBans(const ::Murmur::AMD_Server_getBansPtr cb, int server_id) {
	NEED_SERVER;
	::Murmur::BanList bl;
	foreach(const ::Ban &ban, server->qlBans) {
		::Murmur::Ban mb;
		banToBan(ban, mb);
		bl.push_back(mb);
	}
	cb->ice_response(bl);
}

static void impl_Server_setBans(const ::Murmur::AMD_Server_setBansPtr cb, int server_id,  const ::Murmur::BanList& bans) {
	NEED_SERVER;
	server->qlBans.clear();
	foreach(const ::Murmur::Ban &mb, bans) {
		::Ban ban;
		banToBan(mb, ban);
		server->qlBans << ban;
	}
	server->saveBans();
	cb->ice_response();
}

static void impl_Server_kickUser(const ::Murmur::AMD_Server_kickUserPtr cb, int server_id,  ::Ice::Int session,  const ::std::string& reason) {
	NEED_SERVER;
	NEED_PLAYER;

	MumbleProto::UserRemove mpur;
	mpur.set_session(session);
	mpur.set_reason(reason);
	server->sendAll(mpur);
	user->disconnectSocket();
	cb->ice_response();
}

static void impl_Server_sendMessage(const ::Murmur::AMD_Server_sendMessagePtr cb, int server_id, ::Ice::Int session, const ::std::string &text) {
	NEED_SERVER;
	NEED_PLAYER;

	server->sendTextMessage(NULL, user, false, u8(text));
	cb->ice_response();
}

static void impl_Server_hasPermission(const ::Murmur::AMD_Server_hasPermissionPtr cb, int server_id, ::Ice::Int session, ::Ice::Int channelid, ::Ice::Int perm) {
	NEED_SERVER;
	NEED_PLAYER;
	NEED_CHANNEL;
	cb->ice_response(server->hasPermission(user, channel, static_cast<ChanACL::Perm>(perm)));
}

static void impl_Server_addContextCallback(const Murmur::AMD_Server_addContextCallbackPtr cb, int server_id, ::Ice::Int session, const ::std::string& action, const ::std::string& text, const ::Murmur::ServerContextCallbackPrx& cbptr, int ctx) {
	NEED_SERVER;
	NEED_PLAYER;

	QMap<QString, ::Murmur::ServerContextCallbackPrx> & qmPrx = mi->qmServerContextCallbacks[server_id][session];

	if (!(ctx & (MumbleProto::ContextActionAdd_Context_Server | MumbleProto::ContextActionAdd_Context_Channel | MumbleProto::ContextActionAdd_Context_User))) {
		cb->ice_exception(InvalidCallbackException());
		return;
	}

	try {
		const Murmur::ServerContextCallbackPrx &oneway = Murmur::ServerContextCallbackPrx::checkedCast(cbptr->ice_oneway()->ice_connectionCached(false));
		qmPrx.insert(u8(action), oneway);
		cb->ice_response();
	} catch (...) {
		cb->ice_exception(InvalidCallbackException());
		return;
	}

	MumbleProto::ContextActionAdd mpcaa;
	mpcaa.set_action(action);
	mpcaa.set_text(text);
	mpcaa.set_context(ctx);
	server->sendMessage(user, mpcaa);
}

static void impl_Server_removeContextCallback(const Murmur::AMD_Server_removeContextCallbackPtr cb, int server_id, const Murmur::ServerContextCallbackPrx& cbptr) {
	NEED_SERVER;

	QMap<int, QMap<QString, ::Murmur::ServerContextCallbackPrx> > & qmPrx = mi->qmServerContextCallbacks[server_id];

	try {
		const Murmur::ServerContextCallbackPrx &oneway = Murmur::ServerContextCallbackPrx::uncheckedCast(cbptr->ice_oneway()->ice_connectionCached(false));

		foreach(int session, qmPrx.keys()) {
			QMap<QString, ::Murmur::ServerContextCallbackPrx> qm = qmPrx[session];
			foreach(const QString &act, qm.keys(oneway))
				qm.remove(act);
		}

		cb->ice_response();
	} catch (...) {
		cb->ice_exception(InvalidCallbackException());
	}
}

static void impl_Server_getState(const ::Murmur::AMD_Server_getStatePtr cb, int server_id,  ::Ice::Int session) {
	NEED_SERVER;
	NEED_PLAYER;

	::Murmur::User mp;
	userToUser(user, mp);
	cb->ice_response(mp);
}

static void impl_Server_setState(const ::Murmur::AMD_Server_setStatePtr cb, int server_id,  const ::Murmur::User& state) {
	int session = state.session;
	::Channel *channel;
	NEED_SERVER;
	NEED_PLAYER;
	NEED_CHANNEL_VAR(channel, state.channel);

	server->setUserState(user, channel, state.mute, state.deaf, state.suppress, u8(state.comment));
	cb->ice_response();
}

static void impl_Server_sendMessageChannel(const ::Murmur::AMD_Server_sendMessageChannelPtr cb, int server_id, ::Ice::Int channelid, bool tree, const ::std::string &text) {
	NEED_SERVER;
	NEED_CHANNEL;

	server->sendTextMessage(channel, NULL, tree, u8(text));
	cb->ice_response();
}

static void impl_Server_getChannelState(const ::Murmur::AMD_Server_getChannelStatePtr cb, int server_id,  ::Ice::Int channelid) {
	NEED_SERVER;
	NEED_CHANNEL;

	::Murmur::Channel mc;
	channelToChannel(channel, mc);
	cb->ice_response(mc);
}

static void impl_Server_setChannelState(const ::Murmur::AMD_Server_setChannelStatePtr cb, int server_id,  const ::Murmur::Channel& state) {
	int channelid = state.id;
	NEED_SERVER;
	NEED_CHANNEL;
	::Channel *np = NULL;
	if (channel->iId != 0) {
		NEED_CHANNEL_VAR(np, state.parent);
	}

	QString qsName = u8(state.name);

	QSet< ::Channel *> newset;
	foreach(int linkid, state.links) {
		::Channel *cLink;
		NEED_CHANNEL_VAR(cLink, linkid);
		newset << cLink;
	}

	if (! server->setChannelState(channel, np, qsName, newset, u8(state.description), state.position))
		cb->ice_exception(::Murmur::InvalidChannelException());
	else
		cb->ice_response();
}

static void impl_Server_removeChannel(const ::Murmur::AMD_Server_removeChannelPtr cb, int server_id,  ::Ice::Int channelid) {
	NEED_SERVER;
	NEED_CHANNEL;

	if (!channel->cParent) {
		cb->ice_exception(::Murmur::InvalidChannelException());
	} else {
		server->removeChannel(channel);
		cb->ice_response();
	}
}

static void impl_Server_addChannel(const ::Murmur::AMD_Server_addChannelPtr cb, int server_id,  const ::std::string& name,  ::Ice::Int parent) {
	NEED_SERVER;
	::Channel *p, *nc;
	NEED_CHANNEL_VAR(p, parent);

	QString qsName = u8(name);

	nc = server->addChannel(p, qsName);
	server->updateChannel(nc);
	int newid = nc->iId;

	MumbleProto::ChannelState mpcs;
	mpcs.set_channel_id(newid);
	mpcs.set_parent(parent);
	mpcs.set_name(name);
	server->sendAll(mpcs);

	cb->ice_response(newid);
}

static void impl_Server_getACL(const ::Murmur::AMD_Server_getACLPtr cb, int server_id, ::Ice::Int channelid) {
	NEED_SERVER;
	NEED_CHANNEL;

	::Murmur::ACLList acls;
	::Murmur::GroupList groups;

	QStack< ::Channel *> chans;
	::Channel *p;
	ChanACL *acl;
	p = channel;
	while (p) {
		chans.push(p);
		if ((p == channel) || (p->bInheritACL))
			p = p->cParent;
		else
			p = NULL;
	}

	bool inherit = channel->bInheritACL;

	while (! chans.isEmpty()) {
		p = chans.pop();
		foreach(acl, p->qlACL) {
			if ((p==channel) || (acl->bApplySubs)) {
				::Murmur::ACL ma;
				ACLtoACL(acl, ma);
				if (p != channel)
					ma.inherited = true;
				acls.push_back(ma);
			}
		}
	}

	p = channel->cParent;
	const QSet<QString> allnames = ::Group::groupNames(channel);
	foreach(const QString &name, allnames) {
		::Group *g = channel->qhGroups.value(name);
		::Group *pg = p ? ::Group::getGroup(p, name) : NULL;
		if (!g && ! pg)
			continue;
		::Murmur::Group mg;
		groupToGroup(g ? g : pg, mg);
		QSet<int> members;
		if (pg)
			members = pg->members();
		if (g) {
			mg.add = g->qsAdd.toList().toVector().toStdVector();
			mg.remove = g->qsRemove.toList().toVector().toStdVector();
			mg.inherited = false;
			members += g->qsAdd;
			members -= g->qsRemove;
		} else {
			mg.inherited = true;
		}
		mg.members = members.toList().toVector().toStdVector();
		groups.push_back(mg);
	}
	cb->ice_response(acls, groups, inherit);
}

static void impl_Server_setACL(const ::Murmur::AMD_Server_setACLPtr cb, int server_id,  ::Ice::Int channelid,  const ::Murmur::ACLList& acls,  const ::Murmur::GroupList& groups,  bool inherit) {
	NEED_SERVER;
	NEED_CHANNEL;

	::Group *g;
	ChanACL *acl;

	QHash<QString, QSet<int> > hOldTemp;
	foreach(g, channel->qhGroups) {
		hOldTemp.insert(g->qsName, g->qsTemporary);
		delete g;
	}
	foreach(acl, channel->qlACL)
		delete acl;

	channel->qhGroups.clear();
	channel->qlACL.clear();

	channel->bInheritACL = inherit;
	foreach(const ::Murmur::Group &gi, groups) {
		QString name = u8(gi.name);
		g = new ::Group(channel, name);
		g->bInherit = gi.inherit;
		g->bInheritable = gi.inheritable;
		g->qsAdd = QVector<int>::fromStdVector(gi.add).toList().toSet();
		g->qsRemove = QVector<int>::fromStdVector(gi.remove).toList().toSet();
		g->qsTemporary = hOldTemp.value(name);
	}
	foreach(const ::Murmur::ACL &ai, acls) {
		acl = new ChanACL(channel);
		acl->bApplyHere = ai.applyHere;
		acl->bApplySubs = ai.applySubs;
		acl->iUserId = ai.userid;
		acl->qsGroup = u8(ai.group);
		acl->pDeny = static_cast<ChanACL::Permissions>(ai.deny) & ChanACL::All;
		acl->pAllow = static_cast<ChanACL::Permissions>(ai.allow) & ChanACL::All;
	}

	server->clearACLCache();
	server->updateChannel(channel);
	cb->ice_response();
}

static void impl_Server_getUserNames(const ::Murmur::AMD_Server_getUserNamesPtr cb, int server_id,  const ::Murmur::IdList& ids) {
	NEED_SERVER;
	::Murmur::NameMap nm;
	foreach(int userid, ids) {
		nm[userid] = u8(server->getUserName(userid));
	}
	cb->ice_response(nm);
}

static void impl_Server_getUserIds(const ::Murmur::AMD_Server_getUserIdsPtr cb, int server_id,  const ::Murmur::NameList& names) {
	NEED_SERVER;
	::Murmur::IdMap im;
	foreach(const string &n, names) {
		QString name = u8(n);
		im[n] = server->getUserID(name);
	}
	cb->ice_response(im);
}

static void impl_Server_registerUser(const ::Murmur::AMD_Server_registerUserPtr cb, int server_id, const ::Murmur::UserInfoMap &im) {
	NEED_SERVER;

	QMap<int, QString> info;
	infoToInfo(im, info);

	int userid = server->registerUser(info);
	if (userid < 0)
		cb->ice_exception(InvalidUserException());
	else
		cb->ice_response(userid);
}

static void impl_Server_unregisterUser(const ::Murmur::AMD_Server_unregisterUserPtr cb, int server_id,  ::Ice::Int userid) {
	NEED_SERVER;
	if (! server->unregisterUser(userid))
		cb->ice_exception(InvalidUserException());
	else
		cb->ice_response();
}

static void impl_Server_updateRegistration(const ::Murmur::AMD_Server_updateRegistrationPtr cb, int server_id,  int id, const ::Murmur::UserInfoMap &im) {
	NEED_SERVER;

	if (! server->isUserId(id)) {
		cb->ice_exception(InvalidUserException());
		return;
	}

	QMap<int, QString> info;
	infoToInfo(im, info);

	if (! server->setInfo(id, info)) {
		cb->ice_exception(InvalidUserException());
		return;
	}
	cb->ice_response();
}

static void impl_Server_getRegistration(const ::Murmur::AMD_Server_getRegistrationPtr cb, int server_id,  ::Ice::Int userid) {
	NEED_SERVER;

	QMap<int, QString> info = server->getRegistration(userid);

	if (info.isEmpty()) {
		cb->ice_exception(InvalidUserException());
		return;
	}

	Murmur::UserInfoMap im;
	infoToInfo(info, im);
	cb->ice_response(im);
}

static void impl_Server_getRegisteredUsers(const ::Murmur::AMD_Server_getRegisteredUsersPtr cb, int server_id,  const ::std::string& filter) {
	NEED_SERVER;
	Murmur::NameMap rpl;

	const QMap<int, QString> l = server->getRegisteredUsers(u8(filter));
	QMap<int, QString>::const_iterator i;
	for (i = l.constBegin(); i != l.constEnd(); ++i) {
		rpl[i.key()] = u8(i.value());
	}

	cb->ice_response(rpl);
}

static void impl_Server_verifyPassword(const ::Murmur::AMD_Server_verifyPasswordPtr cb, int server_id,  const ::std::string& name,  const ::std::string& pw) {
	NEED_SERVER;
	QString uname = u8(name);
	cb->ice_response(server->authenticate(uname, u8(pw)));
}

static void impl_Server_getTexture(const ::Murmur::AMD_Server_getTexturePtr cb, int server_id,  ::Ice::Int userid) {
	NEED_SERVER;

	if (! server->isUserId(userid)) {
		cb->ice_exception(InvalidUserException());
		return;
	}

	const QByteArray &qba = server->getUserTexture(userid);

	::Murmur::Texture tex;
	tex.resize(qba.size());
	const char *ptr = qba.constData();
	for (int i=0;i<qba.size()-4;++i)
		tex[i] = ptr[i+4];

	cb->ice_response(tex);
}

static void impl_Server_setTexture(const ::Murmur::AMD_Server_setTexturePtr cb, int server_id,  ::Ice::Int userid,  const ::Murmur::Texture& tex) {
	NEED_SERVER;

	if (! server->isUserId(userid)) {
		cb->ice_exception(InvalidUserException());
		return;
	}

	QByteArray qba(static_cast<int>(tex.size()), 0);
	char *ptr = qba.data();
	for (unsigned int i=0;i<tex.size();++i)
		ptr[i] = tex[i];
	if (! server->setTexture(userid, qba))
		cb->ice_exception(InvalidTextureException());
	else
		cb->ice_response();
}

static void impl_Server_addUserToGroup(const ::Murmur::AMD_Server_addUserToGroupPtr cb, int server_id, ::Ice::Int channelid,  ::Ice::Int session,  const ::std::string& group) {
	NEED_SERVER;
	NEED_PLAYER;
	NEED_CHANNEL;

	QString qsgroup = u8(group);
	if (qsgroup.isEmpty()) {
		cb->ice_exception(InvalidChannelException());
		return;
	}

	::Group *g = channel->qhGroups.value(qsgroup);
	if (! g)
		g = new ::Group(channel, qsgroup);

	g->qsTemporary.insert(- session);
	server->clearACLCache(user);

	cb->ice_response();
}

static void impl_Server_removeUserFromGroup(const ::Murmur::AMD_Server_removeUserFromGroupPtr cb, int server_id,  ::Ice::Int channelid,  ::Ice::Int session,  const ::std::string& group) {
	NEED_SERVER;
	NEED_PLAYER;
	NEED_CHANNEL;

	QString qsgroup = u8(group);
	if (qsgroup.isEmpty()) {
		cb->ice_exception(InvalidChannelException());
		return;
	}

	::Group *g = channel->qhGroups.value(qsgroup);
	if (! g)
		g = new ::Group(channel, qsgroup);

	g->qsTemporary.remove(- session);
	server->clearACLCache(user);

	cb->ice_response();
}

static void impl_Server_redirectWhisperGroup(const ::Murmur::AMD_Server_redirectWhisperGroupPtr cb, int server_id,  ::Ice::Int session,  const ::std::string& source,  const ::std::string& target) {
	NEED_SERVER;
	NEED_PLAYER;

	QString qssource = u8(source);
	QString qstarget = u8(target);
	if (qstarget.isEmpty())
		user->qmWhisperRedirect.remove(qssource);
	else
		user->qmWhisperRedirect.insert(qssource, qstarget);

	server->clearACLCache(user);

	cb->ice_response();
}

static void impl_Meta_getServer(const ::Murmur::AMD_Meta_getServerPtr cb, const Ice::ObjectAdapterPtr adapter, ::Ice::Int id) {
	QList<int> server_list = ServerDB::getAllServers();
	if (! server_list.contains(id))
		cb->ice_response(NULL);
	else
		cb->ice_response(idToProxy(id, adapter));
}

static void impl_Meta_newServer(const ::Murmur::AMD_Meta_newServerPtr cb, const Ice::ObjectAdapterPtr adapter) {
	cb->ice_response(idToProxy(ServerDB::addServer(), adapter));
}

static void impl_Meta_getAllServers(const ::Murmur::AMD_Meta_getAllServersPtr cb, const Ice::ObjectAdapterPtr adapter) {
	::Murmur::ServerList sl;

	foreach(int id, ServerDB::getAllServers())
		sl.push_back(idToProxy(id, adapter));
	cb->ice_response(sl);
}

static void impl_Meta_getDefaultConf(const ::Murmur::AMD_Meta_getDefaultConfPtr cb, const Ice::ObjectAdapterPtr) {
	::Murmur::ConfigMap cm;
	QMap<QString, QString>::const_iterator i;
	for (i=meta->mp.qmConfig.constBegin();i != meta->mp.qmConfig.constEnd(); ++i) {
		cm[u8(i.key())] = u8(i.value());
	}
	cb->ice_response(cm);
}

static void impl_Meta_getBootedServers(const ::Murmur::AMD_Meta_getBootedServersPtr cb, const Ice::ObjectAdapterPtr adapter) {
	::Murmur::ServerList sl;

	foreach(int id, meta->qhServers.keys())
		sl.push_back(idToProxy(id, adapter));
	cb->ice_response(sl);
}

static void impl_Meta_getVersion(const ::Murmur::AMD_Meta_getVersionPtr cb, const Ice::ObjectAdapterPtr) {
	int major, minor, patch;
	QString txt;
	::Meta::getVersion(major, minor, patch, txt);
	cb->ice_response(major, minor, patch, u8(txt));
}

static void impl_Meta_addCallback(const Murmur::AMD_Meta_addCallbackPtr cb, const Ice::ObjectAdapterPtr, const Murmur::MetaCallbackPrx& cbptr) {
	try {
		const Murmur::MetaCallbackPrx &oneway = Murmur::MetaCallbackPrx::checkedCast(cbptr->ice_oneway()->ice_connectionCached(false));
		if (! mi->qmMetaCallbacks.contains(oneway))
			mi->qmMetaCallbacks.append(oneway);
		cb->ice_response();
	} catch (...) {
		cb->ice_exception(InvalidCallbackException());
	}
}

static void impl_Meta_removeCallback(const Murmur::AMD_Meta_removeCallbackPtr cb, const Ice::ObjectAdapterPtr, const Murmur::MetaCallbackPrx& cbptr) {
	try {
		const Murmur::MetaCallbackPrx &oneway = Murmur::MetaCallbackPrx::uncheckedCast(cbptr->ice_oneway()->ice_connectionCached(false));
		mi->qmMetaCallbacks.removeAll(oneway);
		cb->ice_response();
	} catch (...) {
		cb->ice_exception(InvalidCallbackException());
	}
}

#include "MurmurIceWrapper.cpp"
