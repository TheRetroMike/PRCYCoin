// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2018 The PIVX developers
// Copyright (c) 2018-2020 The DAPS Project developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/prcycoin-config.h"
#endif

#include "net.h"

#include "addrman.h"
#include "chainparams.h"
#include "clientversion.h"
#include "crypto/common.h"
#include "guiinterface.h"
#include "main.h"
#include "primitives/transaction.h"
#include "netbase.h"
#include "scheduler.h"

#ifdef WIN32
#include <string.h>
#else

#include <fcntl.h>

#endif

#ifdef USE_UPNP
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#endif

#include <boost/thread.hpp>

#include <math.h>

// Dump addresses to peers.dat and banlist.dat every 15 minutes (900s)
#define DUMP_ADDRESSES_INTERVAL 900

// We add a random period time (0 to 1 seconds) to feeler connections to prevent synchronization.
#define FEELER_SLEEP_WINDOW 1

#if !defined(HAVE_MSG_NOSIGNAL) && !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

// Fix for ancient MinGW versions, that don't have defined these in ws2tcpip.h.
// Todo: Can be removed when our pull-tester is upgraded to a modern MinGW version.
#ifdef WIN32
#ifndef PROTECTION_LEVEL_UNRESTRICTED
#define PROTECTION_LEVEL_UNRESTRICTED 10
#endif
#ifndef IPV6_PROTECTION_LEVEL
#define IPV6_PROTECTION_LEVEL 23
#endif
#endif


namespace {
    const int MAX_OUTBOUND_CONNECTIONS = 16;
    const int MAX_FEELER_CONNECTIONS = 1;

    struct ListenSocket {
        SOCKET socket;
        bool whitelisted;

        ListenSocket(SOCKET socket, bool whitelisted) : socket(socket), whitelisted(whitelisted) {}
    };
}

/** Services this node implementation cares about */
static const ServiceFlags nRelevantServices = NODE_NETWORK;

//
// Global state variables
//
bool fDiscover = true;
bool fListen = true;
ServiceFlags nLocalServices = NODE_NETWORK;
RecursiveMutex cs_mapLocalHost;
std::map<CNetAddr, LocalServiceInfo> mapLocalHost;
static bool vfLimited[NET_MAX] = {};
static CNode *pnodeLocalHost = NULL;
uint64_t nLocalHostNonce = 0;
static std::vector <ListenSocket> vhListenSocket;
CAddrMan addrman;
int nMaxConnections = DEFAULT_MAX_PEER_CONNECTIONS;
bool fAddressesInitialized = false;
std::string strSubVersion;

std::vector<CNode*> vNodes;
RecursiveMutex cs_vNodes;
std::map<CInv, CDataStream> mapRelay;
std::deque<std::pair<int64_t, CInv> > vRelayExpiration;
RecursiveMutex cs_mapRelay;
limitedmap<CInv, int64_t> mapAlreadyAskedFor(MAX_INV_SZ);

static std::deque<std::string> vOneShots;
RecursiveMutex cs_vOneShots;

std::vector<std::string> vAddedNodes;
RecursiveMutex cs_vAddedNodes;

NodeId nLastNodeId = 0;
RecursiveMutex cs_nLastNodeId;

static CSemaphore *semOutbound = NULL;
boost::condition_variable messageHandlerCondition;

// Signals for message handling
static CNodeSignals g_signals;

CNodeSignals &GetNodeSignals() { return g_signals; }

void AddOneShot(std::string strDest) {
    LOCK(cs_vOneShots);
    vOneShots.push_back(strDest);
}

unsigned short GetListenPort() {
    return (unsigned short) (GetArg("-port", Params().GetDefaultPort()));
}

// find 'best' local address for a particular peer
bool GetLocal(CService &addr, const CNetAddr *paddrPeer) {
    if (!fListen)
        return false;

    int nBestScore = -1;
    int nBestReachability = -1;
    {
        LOCK(cs_mapLocalHost);
        for (std::map<CNetAddr, LocalServiceInfo>::iterator it = mapLocalHost.begin(); it != mapLocalHost.end(); it++) {
            int nScore = (*it).second.nScore;
            int nReachability = (*it).first.GetReachabilityFrom(paddrPeer);
            if (nReachability > nBestReachability || (nReachability == nBestReachability && nScore > nBestScore)) {
                addr = CService((*it).first, (*it).second.nPort);
                nBestReachability = nReachability;
                nBestScore = nScore;
            }
        }
    }
    return nBestScore >= 0;
}

//! Convert the pnSeeds6 array into usable address objects.
static std::vector<CAddress> convertSeed6(const std::vector<SeedSpec6>& vSeedsIn)
{
    // It'll only connect to one or two seed nodes because once it connects,
    // it'll get a pile of addresses with newer timestamps.
    // Seed nodes are given a random 'last seen time' of between one and two
    // weeks ago.
    const int64_t nOneWeek = 7 * 24 * 60 * 60;
    std::vector<CAddress> vSeedsOut;
    vSeedsOut.reserve(vSeedsIn.size());
    for (std::vector<SeedSpec6>::const_iterator i(vSeedsIn.begin()); i != vSeedsIn.end(); ++i) {
        struct in6_addr ip;
        memcpy(&ip, i->addr, sizeof(ip));
        CAddress addr(CService(ip, i->port), NODE_NETWORK);
        addr.nTime = GetTime() - GetRand(nOneWeek) - nOneWeek;
        vSeedsOut.push_back(addr);
    }
    return vSeedsOut;
}

// get best local address for a particular peer as a CAddress
// Otherwise, return the unroutable 0.0.0.0 but filled in with
// the normal parameters, since the IP may be changed to a useful
// one by discovery.
CAddress GetLocalAddress(const CNetAddr *paddrPeer) {
    CAddress ret(CService(CNetAddr(), GetListenPort()), NODE_NONE);
    CService addr;
    if (GetLocal(addr, paddrPeer)) {
        ret = CAddress(addr, nLocalServices);
    }
    ret.nTime = GetAdjustedTime();
    return ret;
}

int GetnScore(const CService &addr) {
    LOCK(cs_mapLocalHost);
    if (mapLocalHost.count(addr) == LOCAL_NONE)
        return 0;
    return mapLocalHost[addr].nScore;
}

// Is our peer's addrLocal potentially useful as an external IP source?
bool IsPeerAddrLocalGood(CNode *pnode) {
    return fDiscover && pnode->addr.IsRoutable() && pnode->addrLocal.IsRoutable() &&
           !IsLimited(pnode->addrLocal.GetNetwork());
}

// pushes our own address to a peer
void AdvertiseLocal(CNode* pnode)
{
    if (fListen && pnode->fSuccessfullyConnected) {
        CAddress addrLocal = GetLocalAddress(&pnode->addr);
        // If discovery is enabled, sometimes give our peer the address it
        // tells us that it sees us as in case it has a better idea of our
        // address than we do.
        if (IsPeerAddrLocalGood(pnode) && (!addrLocal.IsRoutable() ||
                                           GetRand((GetnScore(addrLocal) > LOCAL_MANUAL) ? 8 : 2) == 0)) {
            addrLocal.SetIP(pnode->addrLocal);
        }
        if (addrLocal.IsRoutable()) {
            LogPrintf("%s: advertising address %s\n", __func__, addrLocal.ToString());
            FastRandomContext insecure_rand;
            pnode->PushAddress(addrLocal, insecure_rand);
        }
    }
}

// learn a new local address
bool AddLocal(const CService &addr, int nScore) {
    if (!addr.IsRoutable())
        return false;

    if (!fDiscover && nScore < LOCAL_MANUAL)
        return false;

    if (IsLimited(addr))
        return false;

    LogPrintf("AddLocal(%s,%i)\n", addr.ToString(), nScore);

    {
        LOCK(cs_mapLocalHost);
        bool fAlready = mapLocalHost.count(addr) > 0;
        LocalServiceInfo &info = mapLocalHost[addr];
        if (!fAlready || nScore >= info.nScore) {
            info.nScore = nScore + (fAlready ? 1 : 0);
            info.nPort = addr.GetPort();
        }
    }

    return true;
}

bool AddLocal(const CNetAddr &addr, int nScore) {
    return AddLocal(CService(addr, GetListenPort()), nScore);
}

bool RemoveLocal(const CService &addr) {
    LOCK(cs_mapLocalHost);
    LogPrintf("RemoveLocal(%s)\n", addr.ToString());
    mapLocalHost.erase(addr);
    return true;
}

/** Make a particular network entirely off-limits (no automatic connects to it) */
void SetLimited(enum Network net, bool fLimited) {
    if (net == NET_UNROUTABLE)
        return;
    LOCK(cs_mapLocalHost);
    vfLimited[net] = fLimited;
}

bool IsLimited(enum Network net) {
    LOCK(cs_mapLocalHost);
    return vfLimited[net];
}

bool IsLimited(const CNetAddr &addr) {
    return IsLimited(addr.GetNetwork());
}

/** vote for a local address */
bool SeenLocal(const CService &addr) {
    {
        LOCK(cs_mapLocalHost);
        if (mapLocalHost.count(addr) == 0)
            return false;
        mapLocalHost[addr].nScore++;
    }
    return true;
}


/** check whether a given address is potentially local */
bool IsLocal(const CService &addr) {
    LOCK(cs_mapLocalHost);
    return mapLocalHost.count(addr) > 0;
}

/** check whether a given network is one we can probably connect to */
bool IsReachable(enum Network net) {
    LOCK(cs_mapLocalHost);
    return !vfLimited[net];
}

/** check whether a given address is in a network we can probably connect to */
bool IsReachable(const CNetAddr &addr) {
    enum Network net = addr.GetNetwork();
    return IsReachable(net);
}

void AddressCurrentlyConnected(const CService &addr) {
    addrman.Connected(addr);
}


uint64_t CNode::nTotalBytesRecv = 0;
uint64_t CNode::nTotalBytesSent = 0;
RecursiveMutex CNode::cs_totalBytesRecv;
RecursiveMutex CNode::cs_totalBytesSent;

CNode *FindNode(const CNetAddr &ip) {
    LOCK(cs_vNodes);
    for (CNode * pnode : vNodes)
    if ((CNetAddr) pnode->addr == ip)
        return (pnode);
    return NULL;
}

CNode *FindNode(const CSubNet &subNet) {
    LOCK(cs_vNodes);
    for (CNode * pnode : vNodes)
    if (subNet.Match((CNetAddr) pnode->addr))
        return (pnode);
    return NULL;
}

CNode *FindNode(const std::string &addrName) {
    LOCK(cs_vNodes);
    for (CNode * pnode : vNodes)
    if (pnode->addrName == addrName)
        return (pnode);
    return NULL;
}

CNode *FindNode(const CService &addr) {
    LOCK(cs_vNodes);
    const bool isRegTestNet = Params().IsRegTestNet();
    for (CNode * pnode : vNodes)
    {
        if (isRegTestNet) {
            //if using regtest, just check the IP
            if ((CNetAddr) pnode->addr == (CNetAddr) addr)
                return (pnode);
        } else {
            if (pnode->addr == addr)
                return (pnode);
        }
    }
    return NULL;
}

CNode* ConnectNode(CAddress addrConnect, const char* pszDest, bool fCountFailure)
{
    if (pszDest == NULL) {
        if (IsLocal(addrConnect))
            return NULL;

        // Look for an existing connection
        CNode *pnode = FindNode((CService) addrConnect);
        if (pnode) {
            pnode->AddRef();
            return pnode;
        }
    }

    /// debug print
    LogPrint(BCLog::NET, "trying connection %s lastseen=%.1fhrs\n",
             pszDest ? pszDest : addrConnect.ToString(),
             pszDest ? 0.0 : (double) (GetAdjustedTime() - addrConnect.nTime) / 3600.0);

    // Connect
    SOCKET hSocket = INVALID_SOCKET;
    bool proxyConnectionFailed = false;
    if (pszDest ? ConnectSocketByName(addrConnect, hSocket, pszDest, Params().GetDefaultPort(), nConnectTimeout,
                                      &proxyConnectionFailed) :
        ConnectSocket(addrConnect, hSocket, nConnectTimeout, &proxyConnectionFailed)) {
        if (!IsSelectableSocket(hSocket)) {
            LogPrintf("Cannot create connection: non-selectable socket created (fd >= FD_SETSIZE ?)\n");
            CloseSocket(hSocket);
            return NULL;
        }

        addrman.Attempt(addrConnect, fCountFailure);

        // Add node
        CNode *pnode = new CNode(hSocket, addrConnect, pszDest ? pszDest : "", false);
        pnode->AddRef();

        {
            LOCK(cs_vNodes);
            vNodes.push_back(pnode);
        }

        pnode->nServicesExpected = ServiceFlags(addrConnect.nServices & nRelevantServices);
        pnode->nTimeConnected = GetTime();

        return pnode;
    } else if (!proxyConnectionFailed) {
        // If connecting to the node failed, and failure is not caused by a problem connecting to
        // the proxy, mark this as an attempt.
        addrman.Attempt(addrConnect, fCountFailure);
    }

    return NULL;
}

static void DumpBanlist()
{
    CNode::SweepBanned(); // clean unused entries (if bantime has expired)

    if (!CNode::BannedSetIsDirty())
        return;

    int64_t nStart = GetTimeMillis();

    CBanDB bandb;
    banmap_t banmap;
    CNode::SetBannedSetDirty(false);
    CNode::GetBanned(banmap);
    if (!bandb.Write(banmap))
        CNode::SetBannedSetDirty(true);

    LogPrint(BCLog::NET, "Flushed %d banned node ips/subnets to banlist.dat  %dms\n",
        banmap.size(), GetTimeMillis() - nStart);
}

void CNode::CloseSocketDisconnect() {
    fDisconnect = true;
    if (hSocket != INVALID_SOCKET) {
        LogPrint(BCLog::NET, "disconnecting peer=%d\n", id);
        CloseSocket(hSocket);
    }

    // in case this fails, we'll empty the recv buffer when the CNode is deleted
    TRY_LOCK(cs_vRecvMsg, lockRecv);
    if (lockRecv)
        vRecvMsg.clear();
}

bool CNode::DisconnectOldProtocol(int nVersionRequired, std::string strLastCommand) {
    fDisconnect = false;
    if (nVersion < nVersionRequired) {
        LogPrintf("%s : peer=%d using obsolete version %i; disconnecting\n", __func__, id, nVersion);
        PushMessage(NetMsgType::REJECT, strLastCommand, REJECT_OBSOLETE,
                    strprintf("Version must be %d or greater", ActiveProtocol()));
        fDisconnect = true;
    }

    return fDisconnect;
}

bool CNode::DisconnectOldVersion(std::string strSubVer, int nHeight, std::string strLastCommand) {
    fDisconnect = false;
    //if (nHeight >= Params().FixChecks()) {
        if (strSubVer == "/PRCY:1.0.0.2/" || strSubVer == "/PRCY:1.0.0.3/" ||
                strSubVer == "/PRCY:1.0.0.4/" || strSubVer == "/PRCY:1.0.0.5/" ||
                strSubVer == "/PRCY:1.0.0.6/" || strSubVer == "/PRCY:1.0.0.7/" ||
                strSubVer == "/PRCY:1.0.0.8/" || strSubVer == "/PRCY:1.0.0.9/" ||
                strSubVer == "/PRCY:2.0.0/" || strSubVer == "/PRCY:2.0.0.1/" ||
                strSubVer == "/PRCY:2.0.0.2/" || strSubVer == "/PRCY:2.0.0.3/" ||
                strSubVer == "/PRCY:2.0.0.4/") {
            LogPrintf("%s : peer=%d using unsupported version %i; disconnecting\n",  __func__, id, strSubVer);
            PushMessage(NetMsgType::REJECT, strLastCommand, REJECT_OBSOLETE,
                        strprintf("Using unsupported version %i; disconnecting\n", strSubVer));
            fDisconnect = true;
        }
    //}
    return fDisconnect;
}

void CNode::PushVersion() {
    int nBestHeight = GetNodeSignals().GetHeight().get_value_or(0);

    /// when NTP implemented, change to just nTime = GetAdjustedTime()
    int64_t nTime = (fInbound ? GetAdjustedTime() : GetTime());
    CAddress addrYou = (addr.IsRoutable() && !IsProxy(addr) ? addr : CAddress(CService(), addr.nServices));
    CAddress addrMe = GetLocalAddress(&addr);
    GetRandBytes((unsigned char *) &nLocalHostNonce, sizeof(nLocalHostNonce));
    if (fLogIPs)
        LogPrint(BCLog::NET, "send version message: version %d, blocks=%d, us=%s, them=%s, peer=%d\n", PROTOCOL_VERSION,
                 nBestHeight, addrMe.ToString(), addrYou.ToString(), id);
    else
        LogPrint(BCLog::NET, "send version message: version %d, blocks=%d, us=%s, peer=%d\n", PROTOCOL_VERSION, nBestHeight,
                 addrMe.ToString(), id);
    PushMessage(NetMsgType::VERSION, PROTOCOL_VERSION, (uint64_t)nLocalServices, nTime, addrYou, addrMe,
        nLocalHostNonce, strSubVersion, nBestHeight, true);
}


banmap_t CNode::setBanned;
RecursiveMutex CNode::cs_setBanned;
bool CNode::setBannedIsDirty;

void CNode::ClearBanned() {
    {
        LOCK(cs_setBanned);
        setBanned.clear();
        setBannedIsDirty = true;
    }
    DumpBanlist(); // store banlist to Disk
    uiInterface.BannedListChanged();
}

bool CNode::IsBanned(CNetAddr ip) {
    bool fResult = false;
    {
        LOCK(cs_setBanned);
        for (banmap_t::iterator it = setBanned.begin(); it != setBanned.end(); it++) {
            CSubNet subNet = (*it).first;
            CBanEntry banEntry = (*it).second;
            if (subNet.Match(ip) && GetTime() < banEntry.nBanUntil)
                fResult = true;
        }
    }
    return fResult;
}

bool CNode::IsBanned(CSubNet subnet) {
    bool fResult = false;
    {
        LOCK(cs_setBanned);
        banmap_t::iterator i = setBanned.find(subnet);
        if (i != setBanned.end()) {
            CBanEntry banEntry = (*i).second;
            if (GetTime() < banEntry.nBanUntil)
                fResult = true;
        }
    }
    return fResult;
}

void CNode::Ban(const CNetAddr &addr, const BanReason &banReason, int64_t bantimeoffset, bool sinceUnixEpoch) {
    CSubNet subNet(addr);
    Ban(subNet, banReason, bantimeoffset, sinceUnixEpoch);
}

void CNode::Ban(const CSubNet &subNet, const BanReason &banReason, int64_t bantimeoffset, bool sinceUnixEpoch) {
    CBanEntry banEntry(GetTime());
    banEntry.banReason = banReason;
    if (bantimeoffset <= 0) {
        bantimeoffset = GetArg("-bantime", 60 * 60 * 24); // Default 24-hour ban
        sinceUnixEpoch = false;
    }
    banEntry.nBanUntil = (sinceUnixEpoch ? 0 : GetTime()) + bantimeoffset;
    {
        LOCK(cs_setBanned);
        if (setBanned[subNet].nBanUntil < banEntry.nBanUntil) {
            setBanned[subNet] = banEntry;
            setBannedIsDirty = true;
        }
        else return;
    }
    uiInterface.BannedListChanged();
    {
        LOCK(cs_vNodes);
        for (CNode* pnode : vNodes) {
            if (subNet.Match((CNetAddr)pnode->addr))
                pnode->fDisconnect = true;
        }
    }
    if(banReason == BanReasonManuallyAdded)
        DumpBanlist(); //store banlist to disk immediately if user requested ban

}

bool CNode::Unban(const CNetAddr &addr) {
    CSubNet subNet(addr);
    return Unban(subNet);
}

bool CNode::Unban(const CSubNet &subNet) {
    {
        LOCK(cs_setBanned);
        if (!setBanned.erase(subNet))
            return false;
        setBannedIsDirty = true;
    }
    uiInterface.BannedListChanged();
    DumpBanlist(); //store banlist to disk immediately
    return true;
}

void CNode::GetBanned(banmap_t &banMap) {
    LOCK(cs_setBanned);
    banMap = setBanned; //create a thread safe copy
}

void CNode::SetBanned(const banmap_t &banMap) {
    LOCK(cs_setBanned);
    setBanned = banMap;
    setBannedIsDirty = true;
}

void CNode::SweepBanned() {
    int64_t now = GetTime();
    bool notifyUI = false;
    {
        LOCK(cs_setBanned);
        banmap_t::iterator it = setBanned.begin();
        while(it != setBanned.end())
        {
            CSubNet subNet = (*it).first;
            CBanEntry banEntry = (*it).second;
            if(now > banEntry.nBanUntil)
            {
                setBanned.erase(it++);
                setBannedIsDirty = true;
                notifyUI = true;
                LogPrint(BCLog::NET, "%s: Removed banned node ip/subnet from banlist.dat: %s\n", __func__, subNet.ToString());
            }
            else
                ++it;
        }
    }
    // update UI
    if(notifyUI) {
        uiInterface.BannedListChanged();
    }
}

bool CNode::BannedSetIsDirty() {
    LOCK(cs_setBanned);
    return setBannedIsDirty;
}

void CNode::SetBannedSetDirty(bool dirty) {
    LOCK(cs_setBanned); //reuse setBanned lock for the isDirty flag
    setBannedIsDirty = dirty;
}

std::vector <CSubNet> CNode::vWhitelistedRange;
RecursiveMutex CNode::cs_vWhitelistedRange;

bool CNode::IsWhitelistedRange(const CNetAddr &addr) {
    LOCK(cs_vWhitelistedRange);
    for (const CSubNet &subnet : vWhitelistedRange) {
        if (subnet.Match(addr))
            return true;
    }
    return false;
}

void CNode::AddWhitelistedRange(const CSubNet &subnet) {
    LOCK(cs_vWhitelistedRange);
    vWhitelistedRange.push_back(subnet);
}

#undef X
#define X(name) stats.name = name

void CNode::copyStats(CNodeStats &stats) {
    stats.nodeid = this->GetId();
    X(nServices);
    X(addr);
    X(nLastSend);
    X(nLastRecv);
    X(nTimeConnected);
    X(nTimeOffset);
    X(addrName);
    X(nVersion);
    X(cleanSubVer);
    X(fInbound);
    X(nStartingHeight);
    X(nSendBytes);
    X(nRecvBytes);
    X(fWhitelisted);

    // It is common for nodes with good ping times to suddenly become lagged,
    // due to a new block arriving or other large transfer.
    // Merely reporting pingtime might fool the caller into thinking the node was still responsive,
    // since pingtime does not update until the ping is complete, which might take a while.
    // So, if a ping is taking an unusually long time in flight,
    // the caller can immediately detect that this is happening.
    int64_t nPingUsecWait = 0;
    if ((0 != nPingNonceSent) && (0 != nPingUsecStart)) {
        nPingUsecWait = GetTimeMicros() - nPingUsecStart;
    }

    // Raw ping time is in microseconds, but show it to user as whole seconds (PRCY users should be well used to small numbers with many decimal places by now :)
    stats.dPingTime = (((double) nPingUsecTime) / 1e6);
    stats.dPingWait = (((double) nPingUsecWait) / 1e6);

    // Leave string empty if addrLocal invalid (not filled in yet)
    stats.addrLocal = addrLocal.IsValid() ? addrLocal.ToString() : "";
}

#undef X

// requires LOCK(cs_vRecvMsg)
bool CNode::ReceiveMsgBytes(const char *pch, unsigned int nBytes) {
    while (nBytes > 0) {
        // get current incomplete message, or create a new one
        if (vRecvMsg.empty() ||
            vRecvMsg.back().complete())
            vRecvMsg.push_back(CNetMessage(SER_NETWORK, nRecvVersion));

        CNetMessage &msg = vRecvMsg.back();

        // absorb network data
        int handled;
        if (!msg.in_data)
            handled = msg.readHeader(pch, nBytes);
        else
            handled = msg.readData(pch, nBytes);

        if (handled < 0)
            return false;

        if (msg.in_data && msg.hdr.nMessageSize > MAX_PROTOCOL_MESSAGE_LENGTH) {
            LogPrint(BCLog::NET, "Oversized message from peer=%i, disconnecting", GetId());
            return false;
        }

        pch += handled;
        nBytes -= handled;

        if (msg.complete()) {
            msg.nTime = GetTimeMicros();
            messageHandlerCondition.notify_one();
        }
    }

    return true;
}

int CNetMessage::readHeader(const char *pch, unsigned int nBytes) {
    // copy data to temporary parsing buffer
    unsigned int nRemaining = 24 - nHdrPos;
    unsigned int nCopy = std::min(nRemaining, nBytes);

    memcpy(&hdrbuf[nHdrPos], pch, nCopy);
    nHdrPos += nCopy;

    // if header incomplete, exit
    if (nHdrPos < 24)
        return nCopy;

    // deserialize to CMessageHeader
    try {
        hdrbuf >> hdr;
    } catch (const std::exception &) {
        return -1;
    }

    // reject messages larger than MAX_SIZE
    if (hdr.nMessageSize > MAX_SIZE)
        return -1;

    // switch state to reading message data
    in_data = true;

    return nCopy;
}

int CNetMessage::readData(const char *pch, unsigned int nBytes) {
    unsigned int nRemaining = hdr.nMessageSize - nDataPos;
    unsigned int nCopy = std::min(nRemaining, nBytes);

    if (vRecv.size() < nDataPos + nCopy) {
        // Allocate up to 256 KiB ahead, but never more than the total message size.
        vRecv.resize(std::min(hdr.nMessageSize, nDataPos + nCopy + 256 * 1024));
    }

    memcpy(&vRecv[nDataPos], pch, nCopy);
    nDataPos += nCopy;

    return nCopy;
}


// requires LOCK(cs_vSend)
void SocketSendData(CNode *pnode) {
    std::deque<CSerializeData>::iterator it = pnode->vSendMsg.begin();

    while (it != pnode->vSendMsg.end()) {
        const CSerializeData &data = *it;
        assert(data.size() > pnode->nSendOffset);
        int nBytes = send(pnode->hSocket, &data[pnode->nSendOffset], data.size() - pnode->nSendOffset,
                          MSG_NOSIGNAL | MSG_DONTWAIT);
        if (nBytes > 0) {
            pnode->nLastSend = GetTime();
            pnode->nSendBytes += nBytes;
            pnode->nSendOffset += nBytes;
            pnode->RecordBytesSent(nBytes);
            if (pnode->nSendOffset == data.size()) {
                pnode->nSendOffset = 0;
                pnode->nSendSize -= data.size();
                it++;
            } else {
                // could not send full message; stop sending more
                break;
            }
        } else {
            if (nBytes < 0) {
                // error
                int nErr = WSAGetLastError();
                if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS) {
                    LogPrintf("socket send error %s\n", NetworkErrorString(nErr));
                    pnode->CloseSocketDisconnect();
                }
            }
            // couldn't send anything at all
            break;
        }
    }

    if (it == pnode->vSendMsg.end()) {
        assert(pnode->nSendOffset == 0);
        assert(pnode->nSendSize == 0);
    }
    pnode->vSendMsg.erase(pnode->vSendMsg.begin(), it);
}

static std::list<CNode*> vNodesDisconnected;

struct NodeEvictionCandidate
{
    NodeId id;
    int64_t nTimeConnected;
    int64_t nMinPingUsecTime;
    CAddress addr;
};

static bool ReverseCompareNodeMinPingTime(const NodeEvictionCandidate &a, const NodeEvictionCandidate &b)
{
    return a.nMinPingUsecTime > b.nMinPingUsecTime;
}

static bool ReverseCompareNodeTimeConnected(const NodeEvictionCandidate &a, const NodeEvictionCandidate &b)
{
    return a.nTimeConnected > b.nTimeConnected;
}

class CompareNetGroupKeyed
{
    std::vector<unsigned char> vchSecretKey;
public:
    CompareNetGroupKeyed()
    {
        vchSecretKey.resize(32, 0);
        GetRandBytes(vchSecretKey.data(), vchSecretKey.size());
    }

    bool operator()(const NodeEvictionCandidate &a, const NodeEvictionCandidate &b)
    {
        std::vector<unsigned char> vchGroupA, vchGroupB;
        CSHA256 hashA, hashB;
        std::vector<unsigned char> vchA(32), vchB(32);

        vchGroupA = a.addr.GetGroup();
        vchGroupB = b.addr.GetGroup();

        hashA.Write(begin_ptr(vchGroupA), vchGroupA.size());
        hashB.Write(begin_ptr(vchGroupB), vchGroupB.size());

        hashA.Write(begin_ptr(vchSecretKey), vchSecretKey.size());
        hashB.Write(begin_ptr(vchSecretKey), vchSecretKey.size());

        hashA.Finalize(begin_ptr(vchA));
        hashB.Finalize(begin_ptr(vchB));

        return vchA < vchB;
    }
};

/** Try to find a connection to evict when the node is full.
 *  Extreme care must be taken to avoid opening the node to attacker
 *   triggered network partitioning.
 *  The strategy used here is to protect a small number of peers
 *   for each of several distinct characteristics which are difficult
 *   to forge.  In order to partition a node the attacker must be
 *   simultaneously better at all of them than honest peers.
 */
static bool AttemptToEvictConnection(bool fPreferNewConnection) {
    std::vector<NodeEvictionCandidate> vEvictionCandidates;
    {
        LOCK(cs_vNodes);

        for (CNode *node : vNodes) {
            if (node->fWhitelisted)
                continue;
            if (!node->fInbound)
                continue;
            if (node->fDisconnect)
                continue;
            NodeEvictionCandidate candidate = {node->id, node->nTimeConnected, node->nMinPingUsecTime, node->addr};
            vEvictionCandidates.push_back(candidate);
        }
    }

    if (vEvictionCandidates.empty()) return false;

    // Protect connections with certain characteristics

    // Deterministically select 4 peers to protect by netgroup.
    // An attacker cannot predict which netgroups will be protected.
    static CompareNetGroupKeyed comparerNetGroupKeyed;
    std::sort(vEvictionCandidates.begin(), vEvictionCandidates.end(), comparerNetGroupKeyed);
    vEvictionCandidates.erase(vEvictionCandidates.end() - std::min(4, static_cast<int>(vEvictionCandidates.size())), vEvictionCandidates.end());

    if (vEvictionCandidates.empty()) return false;

    // Protect the 8 nodes with the lowest minimum ping time.
    // An attacker cannot manipulate this metric without physically moving nodes closer to the target.
    std::sort(vEvictionCandidates.begin(), vEvictionCandidates.end(), ReverseCompareNodeMinPingTime);
    vEvictionCandidates.erase(vEvictionCandidates.end() - std::min(8, static_cast<int>(vEvictionCandidates.size())), vEvictionCandidates.end());

    if (vEvictionCandidates.empty()) return false;

    // Protect the half of the remaining nodes which have been connected the longest.
    // This replicates the non-eviction implicit behavior, and precludes attacks that start later.
    std::sort(vEvictionCandidates.begin(), vEvictionCandidates.end(), ReverseCompareNodeTimeConnected);
    vEvictionCandidates.erase(vEvictionCandidates.end() - static_cast<int>(vEvictionCandidates.size() / 2), vEvictionCandidates.end());

    if (vEvictionCandidates.empty()) return false;

    // Identify the network group with the most connections and youngest member.
    // (vEvictionCandidates is already sorted by reverse connect time)
    std::vector<unsigned char> naMostConnections;
    unsigned int nMostConnections = 0;
    int64_t nMostConnectionsTime = 0;
    std::map<std::vector<unsigned char>, std::vector<NodeEvictionCandidate> > mapAddrCounts;
    for (const NodeEvictionCandidate &node : vEvictionCandidates) {
        mapAddrCounts[node.addr.GetGroup()].push_back(node);
        int64_t grouptime = mapAddrCounts[node.addr.GetGroup()][0].nTimeConnected;
        size_t groupsize = mapAddrCounts[node.addr.GetGroup()].size();

        if (groupsize > nMostConnections || (groupsize == nMostConnections && grouptime > nMostConnectionsTime)) {
            nMostConnections = groupsize;
            nMostConnectionsTime = grouptime;
            naMostConnections = node.addr.GetGroup();
        }
    }

    // Reduce to the network group with the most connections
    vEvictionCandidates = mapAddrCounts[naMostConnections];

    // Do not disconnect peers if there is only 1 connection from their network group
    if (vEvictionCandidates.size() <= 1)
        // unless we prefer the new connection (for whitelisted peers)
        if (!fPreferNewConnection)
            return false;

    // Disconnect from the network group with the most connections
    NodeId evicted = vEvictionCandidates.front().id;
    LOCK(cs_vNodes);
    for(std::vector<CNode*>::const_iterator it(vNodes.begin()); it != vNodes.end(); ++it) {
        if ((*it)->GetId() == evicted) {
            (*it)->fDisconnect = true;
            return true;
        }
    }
    return false;
}

static void AcceptConnection(const ListenSocket& hListenSocket) {
    struct sockaddr_storage sockaddr;
    socklen_t len = sizeof(sockaddr);
    SOCKET hSocket = accept(hListenSocket.socket, (struct sockaddr*)&sockaddr, &len);
    CAddress addr;
    int nInbound = 0;
    int nMaxInbound = nMaxConnections - (MAX_OUTBOUND_CONNECTIONS + MAX_FEELER_CONNECTIONS);

    if (hSocket != INVALID_SOCKET)
        if (!addr.SetSockAddr((const struct sockaddr*)&sockaddr))
            LogPrintf("Warning: Unknown socket family\n");

    bool whitelisted = hListenSocket.whitelisted || CNode::IsWhitelistedRange(addr);
    {
        LOCK(cs_vNodes);
        for (CNode* pnode : vNodes)
            if (pnode->fInbound)
                nInbound++;
    }

    if (hSocket == INVALID_SOCKET) {
        int nErr = WSAGetLastError();
        if (nErr != WSAEWOULDBLOCK)
            LogPrintf("socket error accept failed: %s\n", NetworkErrorString(nErr));
        return;
    }

    if (!IsSelectableSocket(hSocket)) {
        LogPrintf("connection from %s dropped: non-selectable socket\n", addr.ToString());
        CloseSocket(hSocket);
        return;
    }

    if (CNode::IsBanned(addr) && !whitelisted) {
        LogPrintf("connection from %s dropped (banned)\n", addr.ToString());
        CloseSocket(hSocket);
        return;
    }

    if (nInbound >= nMaxConnections - MAX_OUTBOUND_CONNECTIONS) {
        if (!AttemptToEvictConnection(whitelisted)) {
            // No connection to evict, disconnect the new connection
            LogPrint(BCLog::NET, "failed to find an eviction candidate - connection dropped (full)\n");
            CloseSocket(hSocket);
            return;
        }
    }

    CNode* pnode = new CNode(hSocket, addr, "", true);
    pnode->AddRef();
    pnode->fWhitelisted = whitelisted;

    LogPrint(BCLog::NET, "connection from %s accepted\n", addr.ToString());

    {
        LOCK(cs_vNodes);
        vNodes.push_back(pnode);
    }
}

void ThreadSocketHandler() {
    unsigned int nPrevNodeCount = 0;
    while (true) {
        //
        // Disconnect nodes
        //
        {
            LOCK(cs_vNodes);
            // Disconnect unused nodes
            std::vector<CNode*> vNodesCopy = vNodes;
            for (CNode * pnode : vNodesCopy)
            {
                if (pnode->fDisconnect ||
                    (pnode->GetRefCount() <= 0 && pnode->vRecvMsg.empty() && pnode->nSendSize == 0 &&
                     pnode->ssSend.empty())) {
                    // remove from vNodes
                    vNodes.erase(remove(vNodes.begin(), vNodes.end(), pnode), vNodes.end());

                    // release outbound grant (if any)
                    pnode->grantOutbound.Release();

                    // close socket and cleanup
                    pnode->CloseSocketDisconnect();

                    // hold in disconnected pool until all refs are released
                    if (pnode->fNetworkNode || pnode->fInbound)
                        pnode->Release();
                    vNodesDisconnected.push_back(pnode);
                }
            }
        }
        {
            // Delete disconnected nodes
            std::list<CNode*> vNodesDisconnectedCopy = vNodesDisconnected;
            for (CNode * pnode : vNodesDisconnectedCopy)
            {
                // wait until threads are done using it
                if (pnode->GetRefCount() <= 0) {
                    bool fDelete = false;
                    {
                        TRY_LOCK(pnode->cs_vSend, lockSend);
                        if (lockSend) {
                            TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
                            if (lockRecv) {
                                TRY_LOCK(pnode->cs_inventory, lockInv);
                                if (lockInv)
                                    fDelete = true;
                            }
                        }
                    }
                    if (fDelete) {
                        vNodesDisconnected.remove(pnode);
                        delete pnode;
                    }
                }
            }
        }
        size_t vNodesSize;
        {
            LOCK(cs_vNodes);
            vNodesSize = vNodes.size();
        }
        if (vNodesSize != nPrevNodeCount) {
            nPrevNodeCount = vNodesSize;
            uiInterface.NotifyNumConnectionsChanged(nPrevNodeCount);
        }

        //
        // Find which sockets have data to receive
        //
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 50000; // frequency to poll pnode->vSend

        fd_set fdsetRecv;
        fd_set fdsetSend;
        fd_set fdsetError;
        FD_ZERO(&fdsetRecv);
        FD_ZERO(&fdsetSend);
        FD_ZERO(&fdsetError);
        SOCKET hSocketMax = 0;
        bool have_fds = false;

        for (const ListenSocket &hListenSocket : vhListenSocket) {
            FD_SET(hListenSocket.socket, &fdsetRecv);
            hSocketMax = std::max(hSocketMax, hListenSocket.socket);
            have_fds = true;
        }

        {
            LOCK(cs_vNodes);
            for (CNode * pnode : vNodes)
            {
                if (pnode->hSocket == INVALID_SOCKET)
                    continue;
                FD_SET(pnode->hSocket, &fdsetError);
                hSocketMax = std::max(hSocketMax, pnode->hSocket);
                have_fds = true;

                // Implement the following logic:
                // * If there is data to send, select() for sending data. As this only
                //   happens when optimistic write failed, we choose to first drain the
                //   write buffer in this case before receiving more. This avoids
                //   needlessly queueing received data, if the remote peer is not themselves
                //   receiving data. This means properly utilizing TCP flow control signalling.
                // * Otherwise, if there is no (complete) message in the receive buffer,
                //   or there is space left in the buffer, select() for receiving data.
                // * (if neither of the above applies, there is certainly one message
                //   in the receiver buffer ready to be processed).
                // Together, that means that at least one of the following is always possible,
                // so we don't deadlock:
                // * We send some data.
                // * We wait for data to be received (and disconnect after timeout).
                // * We process a message in the buffer (message handler thread).
                {
                    LOCK(pnode->cs_vSend);
                    if (!pnode->vSendMsg.empty()) {
                        FD_SET(pnode->hSocket, &fdsetSend);
                        continue;
                    }
                }
                {
                    TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
                    if (lockRecv && (pnode->vRecvMsg.empty() || !pnode->vRecvMsg.front().complete() ||
                                     pnode->GetTotalRecvSize() <= ReceiveFloodSize()))
                        FD_SET(pnode->hSocket, &fdsetRecv);
                }
            }
        }

        int nSelect = select(have_fds ? hSocketMax + 1 : 0,
                             &fdsetRecv, &fdsetSend, &fdsetError, &timeout);
        boost::this_thread::interruption_point();

        if (nSelect == SOCKET_ERROR) {
            if (have_fds) {
                int nErr = WSAGetLastError();
                LogPrintf("socket select error %s\n", NetworkErrorString(nErr));
                for (unsigned int i = 0; i <= hSocketMax; i++)
                    FD_SET(i, &fdsetRecv);
            }
            FD_ZERO(&fdsetSend);
            FD_ZERO(&fdsetError);
            MilliSleep(timeout.tv_usec / 1000);
        }

        //
        // Accept new connections
        //
        for (const ListenSocket &hListenSocket : vhListenSocket) {
            if (hListenSocket.socket != INVALID_SOCKET && FD_ISSET(hListenSocket.socket, &fdsetRecv)) {
                AcceptConnection(hListenSocket);
            }
        }

        //
        // Service each socket
        //
        std::vector<CNode*> vNodesCopy;
        {
            LOCK(cs_vNodes);
            vNodesCopy = vNodes;
            for (CNode * pnode : vNodesCopy)
            pnode->AddRef();
        }
        for (CNode * pnode : vNodesCopy)
        {
            boost::this_thread::interruption_point();

            //
            // Receive
            //
            if (pnode->hSocket == INVALID_SOCKET)
                continue;
            if (FD_ISSET(pnode->hSocket, &fdsetRecv) || FD_ISSET(pnode->hSocket, &fdsetError)) {
                TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
                if (lockRecv) {
                    {
                        // typical socket buffer is 8K-64K
                        char pchBuf[0x10000];
                        int nBytes = recv(pnode->hSocket, pchBuf, sizeof(pchBuf), MSG_DONTWAIT);
                        if (nBytes > 0) {
                            if (!pnode->ReceiveMsgBytes(pchBuf, nBytes))
                                pnode->CloseSocketDisconnect();
                            pnode->nLastRecv = GetTime();
                            pnode->nRecvBytes += nBytes;
                            pnode->RecordBytesRecv(nBytes);
                        } else if (nBytes == 0) {
                            // socket closed gracefully
                            if (!pnode->fDisconnect)
                                LogPrint(BCLog::NET, "socket closed\n");
                            pnode->CloseSocketDisconnect();
                        } else if (nBytes < 0) {
                            // error
                            int nErr = WSAGetLastError();
                            if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR &&
                                nErr != WSAEINPROGRESS) {
                                if (!pnode->fDisconnect)
                                    LogPrintf("socket recv error %s\n", NetworkErrorString(nErr));
                                pnode->CloseSocketDisconnect();
                            }
                        }
                    }
                }
            }

            //
            // Send
            //
            if (pnode->hSocket == INVALID_SOCKET)
                continue;
            if (FD_ISSET(pnode->hSocket, &fdsetSend)) {
                LOCK(pnode->cs_vSend);
                SocketSendData(pnode);
            }

            //
            // Inactivity checking
            //
            int64_t nTime = GetTime();
            if (nTime - pnode->nTimeConnected > 60) {
                if (pnode->nLastRecv == 0 || pnode->nLastSend == 0) {
                    LogPrint(BCLog::NET, "socket no message in first 60 seconds, %d %d from %d\n", pnode->nLastRecv != 0,
                             pnode->nLastSend != 0, pnode->id);
                    pnode->fDisconnect = true;
                } else if (nTime - pnode->nLastSend > TIMEOUT_INTERVAL) {
                    LogPrintf("socket sending timeout: %is\n", nTime - pnode->nLastSend);
                    pnode->fDisconnect = true;
                } else if (nTime - pnode->nLastRecv >
                           (pnode->nVersion > BIP0031_VERSION ? TIMEOUT_INTERVAL : 90 * 60)) {
                    LogPrintf("socket receive timeout: %is\n", nTime - pnode->nLastRecv);
                    pnode->fDisconnect = true;
                } else if (pnode->nPingNonceSent &&
                           pnode->nPingUsecStart + TIMEOUT_INTERVAL * 1000000 < GetTimeMicros()) {
                    LogPrintf("ping timeout: %fs\n", 0.000001 * (GetTimeMicros() - pnode->nPingUsecStart));
                    pnode->fDisconnect = true;
                }
            }
        }
        {
            LOCK(cs_vNodes);
            for (CNode * pnode : vNodesCopy)
            pnode->Release();
        }
    }
}


#ifdef USE_UPNP
void ThreadMapPort()
{
    std::string port = strprintf("%u", GetListenPort());
    const char* multicastif = 0;
    const char* minissdpdpath = 0;
    struct UPNPDev* devlist = 0;
    char lanaddr[64];

#ifndef UPNPDISCOVER_SUCCESS
    /* miniupnpc 1.5 */
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0);
#elif MINIUPNPC_API_VERSION < 14
    /* miniupnpc 1.6 */
    int error = 0;
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0, 0, &error);
#else
    /* miniupnpc 1.9.20150730 */
    int error = 0;
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0, 0, 2, &error);
#endif

    struct UPNPUrls urls;
    struct IGDdatas data;
    int r;

    r = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr));
    if (r == 1) {
        if (fDiscover) {
            char externalIPAddress[40];
            r = UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, externalIPAddress);
            if (r != UPNPCOMMAND_SUCCESS)
                LogPrintf("UPnP: GetExternalIPAddress() returned %d\n", r);
            else {
                if (externalIPAddress[0]) {
                    CNetAddr resolved;
                    if (LookupHost(externalIPAddress, resolved, false)) {
                        LogPrintf("UPnP: ExternalIPAddress = %s\n", resolved.ToString().c_str());
                        AddLocal(resolved, LOCAL_UPNP);
                    }
                } else
                    LogPrintf("UPnP: GetExternalIPAddress failed.\n");
            }
        }

        std::string strDesc = "PRCY " + FormatFullVersion();

        try {
            while (true) {
#ifndef UPNPDISCOVER_SUCCESS
                /* miniupnpc 1.5 */
                r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                    port.c_str(), port.c_str(), lanaddr, strDesc.c_str(), "TCP", 0);
#else
                /* miniupnpc 1.6 */
                r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                    port.c_str(), port.c_str(), lanaddr, strDesc.c_str(), "TCP", 0, "0");
#endif

                if (r != UPNPCOMMAND_SUCCESS)
                    LogPrintf("AddPortMapping(%s, %s, %s) failed with code %d (%s)\n",
                        port, port, lanaddr, r, strupnperror(r));
                else
                    LogPrintf("UPnP Port Mapping successful.\n");
                ;

                MilliSleep(20 * 60 * 1000); // Refresh every 20 minutes
            }
        } catch (const boost::thread_interrupted&) {
            r = UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, port.c_str(), "TCP", 0);
            LogPrintf("UPNP_DeletePortMapping() returned : %d\n", r);
            freeUPNPDevlist(devlist);
            devlist = 0;
            FreeUPNPUrls(&urls);
            throw;
        }
    } else {
        LogPrintf("No valid UPnP IGDs found\n");
        freeUPNPDevlist(devlist);
        devlist = 0;
        if (r != 0)
            FreeUPNPUrls(&urls);
    }
}

void MapPort(bool fUseUPnP)
{
    static boost::thread* upnp_thread = NULL;

    if (fUseUPnP) {
        if (upnp_thread) {
            upnp_thread->interrupt();
            upnp_thread->join();
            delete upnp_thread;
        }
        upnp_thread = new boost::thread(boost::bind(&TraceThread<void (*)()>, "upnp", &ThreadMapPort));
    } else if (upnp_thread) {
        upnp_thread->interrupt();
        upnp_thread->join();
        delete upnp_thread;
        upnp_thread = NULL;
    }
}

#else

void MapPort(bool) {
    // Intentionally left blank.
}

#endif

static std::string GetDNSHost(const CDNSSeedData& data, ServiceFlags* requiredServiceBits)
{
    //use default host for non-filter-capable seeds or if we use the default service bits (NODE_NETWORK)
    if (!data.supportsServiceBitsFiltering || *requiredServiceBits == NODE_NETWORK) {
        *requiredServiceBits = NODE_NETWORK;
        return data.host;
    }

    return strprintf("x%x.%s", *requiredServiceBits, data.host);
}

void ThreadDNSAddressSeed() {
    // goal: only query DNS seeds if address need is acute
    if ((addrman.size() > 0) &&
        (!GetBoolArg("-forcednsseed", false))) {
        MilliSleep(11 * 1000);

        LOCK(cs_vNodes);
        if (vNodes.size() >= 3) {
            LogPrintf("P2P peers available. Skipped DNS seeding.\n");
            return;
        }
    }

    const std::vector<CDNSSeedData>& vSeeds = Params().DNSSeeds();
    int found = 0;

    LogPrintf("Loading addresses from DNS seeds (could take a while)\n");

    for (const CDNSSeedData &seed : vSeeds) {
        if (HaveNameProxy()) {
            AddOneShot(seed.host);
        } else {
            std::vector<CNetAddr> vIPs;
            std::vector<CAddress> vAdd;
            ServiceFlags requiredServiceBits = nRelevantServices;
            if (LookupHost(GetDNSHost(seed, &requiredServiceBits).c_str(), vIPs, 0, true)) {
                for (CNetAddr & ip : vIPs)
                {
                    int nOneDay = 24 * 3600;
                    CAddress addr = CAddress(CService(ip, Params().GetDefaultPort()), requiredServiceBits);
                    addr.nTime =
                            GetTime() - 3 * nOneDay - GetRand(4 * nOneDay); // use a random age between 3 and 7 days old
                    vAdd.push_back(addr);
                    found++;
                }
            }
            // TODO: The seed name resolve may fail, yielding an IP of [::], which results in
            // addrman assigning the same source to results from different seeds.
            // This should switch to a hard-coded stable dummy IP for each seed name, so that the
            // resolve is not required at all.
            if (!vIPs.empty()) {
                CService seedSource;
                Lookup(seed.name.c_str(), seedSource, 0, true);
                addrman.Add(vAdd, seedSource);
            }
        }
    }

    LogPrintf("%d addresses found from DNS seeds\n", found);
}


void DumpAddresses() {
    int64_t nStart = GetTimeMillis();

    CAddrDB adb;
    adb.Write(addrman);

    LogPrint(BCLog::NET, "Flushed %d addresses to peers.dat  %dms\n",
             addrman.size(), GetTimeMillis() - nStart);
}

void DumpData() {
    DumpAddresses();
    DumpBanlist();
}

void static ProcessOneShot() {
    std::string strDest;
    {
        LOCK(cs_vOneShots);
        if (vOneShots.empty())
            return;
        strDest = vOneShots.front();
        vOneShots.pop_front();
    }
    CAddress addr;
    CSemaphoreGrant grant(*semOutbound, true);
    if (grant) {
        if (!OpenNetworkConnection(addr, false, &grant, strDest.c_str(), true))
            AddOneShot(strDest);
    }
}

void ThreadOpenConnections() {
    // Connect to specific addresses
    if (mapArgs.count("-connect") && mapMultiArgs["-connect"].size() > 0) {
        for (int64_t nLoop = 0;; nLoop++) {
            ProcessOneShot();
            for (std::string strAddr : mapMultiArgs["-connect"]) {
                CAddress addr(CService(), NODE_NONE);
                OpenNetworkConnection(addr, false, NULL, strAddr.c_str());
                for (int i = 0; i < 10 && i < nLoop; i++) {
                    MilliSleep(500);
                }
            }
            MilliSleep(500);
        }
    }

    // Initiate network connections
    int64_t nStart = GetTime();

    // Minimum time before next feeler connection (in microseconds).
    int64_t nNextFeeler = PoissonNextSend(nStart * 1000 * 1000, FEELER_INTERVAL);
    while (true) {
        ProcessOneShot();

        MilliSleep(500);

        CSemaphoreGrant grant(*semOutbound);
        boost::this_thread::interruption_point();

        // Add seed nodes if DNS seeds are all down (an infrastructure attack?).
        if (addrman.size() == 0 && (GetTime() - nStart > 60)) {
            static bool done = false;
            if (!done) {
                LogPrintf("Adding fixed seed nodes as DNS doesn't seem to be available.\n");
                CNetAddr local;
                LookupHost("127.0.0.1", local, false);
                addrman.Add(convertSeed6(Params().FixedSeeds()), local);
                done = true;
            }
        }

        //
        // Choose an address to connect to based on most recently seen
        //
        CAddress addrConnect;

        // Only connect out to one peer per network group (/16 for IPv4).
        // Do this here so we don't have to critsect vNodes inside mapAddresses critsect.
        int nOutbound = 0;
        std::set<std::vector<unsigned char> > setConnected;
        {
            LOCK(cs_vNodes);
            for (CNode * pnode : vNodes)
            {
                if (!pnode->fInbound) {
                    setConnected.insert(pnode->addr.GetGroup());
                    nOutbound++;
                }
            }
        }

        // Feeler Connections
        //
        // Design goals:
        //  * Increase the number of connectable addresses in the tried table.
        //
        // Method:
        //  * Choose a random address from new and attempt to connect to it if we can connect
        //    successfully it is added to tried.
        //  * Start attempting feeler connections only after node finishes making outbound
        //    connections.
        //  * Only make a feeler connection once every few minutes.
        //
        bool fFeeler = false;
        if (nOutbound >= MAX_OUTBOUND_CONNECTIONS) {
            int64_t nTime = GetTimeMicros(); // The current time right now (in microseconds).
            if (nTime > nNextFeeler) {
                nNextFeeler = PoissonNextSend(nTime, FEELER_INTERVAL);
                fFeeler = true;
            } else {
                continue;
            }
        }

        int64_t nANow = GetAdjustedTime();
        int nTries = 0;
        while (true) {
            CAddrInfo addr = addrman.Select(fFeeler);

            // if we selected an invalid address, restart
            if (!addr.IsValid() || setConnected.count(addr.GetGroup()) || IsLocal(addr))
                break;

            // If we didn't find an appropriate destination after trying 100 addresses fetched from addrman,
            // stop this loop, and let the outer loop run again (which sleeps, adds seed nodes, recalculates
            // already-connected network ranges, ...) before trying new addrman addresses.
            nTries++;
            if (nTries > 100)
                break;

            if (IsLimited(addr))
                continue;

            // only connect to full nodes
            if ((addr.nServices & REQUIRED_SERVICES) != REQUIRED_SERVICES)
                continue;

            // only consider very recently tried nodes after 30 failed attempts
            if (nANow - addr.nLastTry < 600 && nTries < 30)
                continue;

            // do not allow non-default ports, unless after 50 invalid addresses selected already
            if (addr.GetPort() != Params().GetDefaultPort() && nTries < 50)
                continue;

            addrConnect = addr;
            break;
        }

        if (addrConnect.IsValid()) {
            if (fFeeler) {
                // Add small amount of random noise before connection to avoid synchronization.
                int randsleep = GetRandInt(FEELER_SLEEP_WINDOW * 1000);
                MilliSleep(randsleep);
                LogPrint(BCLog::NET, "Making feeler connection to %s\n", addrConnect.ToString());
            }

            OpenNetworkConnection(addrConnect, (int)setConnected.size() >= std::min(nMaxConnections - 1, 2), &grant, nullptr, false, fFeeler);
        }
    }
}

std::vector<AddedNodeInfo> GetAddedNodeInfo()
{
    std::vector<AddedNodeInfo> ret;

    std::list<std::string> lAddresses(0);
    {
        LOCK(cs_vAddedNodes);
        ret.reserve(vAddedNodes.size());
        for (std::string& strAddNode : vAddedNodes)
            lAddresses.push_back(strAddNode);
    }


    // Build a map of all already connected addresses (by IP:port and by name) to inbound/outbound and resolved CService
    std::map<CService, bool> mapConnected;
    std::map<std::string, std::pair<bool, CService> > mapConnectedByName;
    {
        LOCK(cs_vNodes);
        for (const CNode* pnode : vNodes) {
            if (pnode->addr.IsValid()) {
                mapConnected[pnode->addr] = pnode->fInbound;
            }
            if (!pnode->addrName.empty()) {
                mapConnectedByName[pnode->addrName] = std::make_pair(pnode->fInbound, static_cast<const CService&>(pnode->addr));
            }
        }
    }

    for (std::string& strAddNode : lAddresses) {
        CService service(LookupNumeric(strAddNode.c_str(), Params().GetDefaultPort()));
        if (service.IsValid()) {
            // strAddNode is an IP:port
            auto it = mapConnected.find(service);
            if (it != mapConnected.end()) {
                ret.push_back(AddedNodeInfo{strAddNode, service, true, it->second});
            } else {
                ret.push_back(AddedNodeInfo{strAddNode, CService(), false, false});
            }
        } else {
            // strAddNode is a name
            auto it = mapConnectedByName.find(strAddNode);
            if (it != mapConnectedByName.end()) {
                ret.push_back(AddedNodeInfo{strAddNode, it->second.second, true, it->second.first});
            } else {
                ret.push_back(AddedNodeInfo{strAddNode, CService(), false, false});
            }
        }
    }

    return ret;
}

void ThreadOpenAddedConnections()
{
    {
        LOCK(cs_vAddedNodes);
        vAddedNodes = mapMultiArgs["-addnode"];
    }

    for (unsigned int i = 0; true; i++) {
        std::vector<AddedNodeInfo> vInfo = GetAddedNodeInfo();
        for (const AddedNodeInfo& info : vInfo) {
            if (!info.fConnected) {
                CSemaphoreGrant grant(*semOutbound);
                // If strAddedNode is an IP/port, decode it immediately, so
                // OpenNetworkConnection can detect existing connections to that IP/port.
                CService service(LookupNumeric(info.strAddedNode.c_str(), Params().GetDefaultPort()));
                OpenNetworkConnection(CAddress(service, NODE_NONE), false, &grant, info.strAddedNode.c_str(), false);
                MilliSleep(500);
            }
        }

        MilliSleep(120000); // Retry every 2 minutes
    }
}

// if successful, this moves the passed grant to the constructed node
bool OpenNetworkConnection(const CAddress& addrConnect, bool fCountFailure, CSemaphoreGrant* grantOutbound, const char* pszDest, bool fOneShot, bool fFeeler)
{
    //
    // Initiate outbound network connection
    //
    boost::this_thread::interruption_point();
    if (!pszDest) {
        if (IsLocal(addrConnect) ||
            FindNode((CNetAddr) addrConnect) || CNode::IsBanned(addrConnect) ||
            FindNode(addrConnect.ToStringIPPort()))
            return false;
    } else if (FindNode(pszDest))
        return false;

    CNode* pnode = ConnectNode(addrConnect, pszDest, fCountFailure);
    boost::this_thread::interruption_point();

    if (!pnode)
        return false;
    if (grantOutbound)
        grantOutbound->MoveTo(pnode->grantOutbound);
    pnode->fNetworkNode = true;
    if (fOneShot)
        pnode->fOneShot = true;
    if (fFeeler)
        pnode->fFeeler = true;

    return true;
}

void ThreadMessageHandler() {
    boost::mutex condition_mutex;
    boost::unique_lock<boost::mutex> lock(condition_mutex);

    SetThreadPriority(THREAD_PRIORITY_BELOW_NORMAL);
    while (true) {
        std::vector<CNode*> vNodesCopy;
        {
            LOCK(cs_vNodes);
            vNodesCopy = vNodes;
            for (CNode * pnode : vNodesCopy)
            {
                pnode->AddRef();
            }
        }

        bool fSleep = true;
        for (CNode * pnode : vNodesCopy)
        {
            if (pnode->fDisconnect)
                continue;
            // Receive messages
            {
                TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
                if (lockRecv) {
                    if (!GetNodeSignals().ProcessMessages(pnode)) {
                        pnode->CloseSocketDisconnect();
                    }
                    if (pnode->nSendSize < SendBufferSize()) {
                        if (!pnode->vRecvGetData.empty() ||
                            (!pnode->vRecvMsg.empty() && pnode->vRecvMsg[0].complete())) {
                            fSleep = false;
                        }
                    }
                }
            }
            boost::this_thread::interruption_point();
            // Send messages
            {
                LOCK(pnode->cs_sendProcessing);
                GetNodeSignals().SendMessages(pnode);
            }
            boost::this_thread::interruption_point();
        }

        {
            LOCK(cs_vNodes);
            for (CNode * pnode : vNodesCopy)
            pnode->Release();
        }

        if (fSleep)
            messageHandlerCondition.timed_wait(lock, boost::posix_time::microsec_clock::universal_time() +
                                                     boost::posix_time::milliseconds(100));
    }
}

bool BindListenPort(const CService& addrBind, std::string& strError, bool fWhitelisted) {
    strError = "";
    int nOne = 1;

    // Create socket for listening for incoming connections
    struct sockaddr_storage sockaddr;
    socklen_t len = sizeof(sockaddr);
    if (!addrBind.GetSockAddr((struct sockaddr *) &sockaddr, &len)) {
        strError = strprintf("Error: Bind address family for %s not supported", addrBind.ToString());
        LogPrintf("%s\n", strError);
        return false;
    }

    SOCKET hListenSocket = socket(((struct sockaddr *) &sockaddr)->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (hListenSocket == INVALID_SOCKET) {
        strError = strprintf("Error: Couldn't open socket for incoming connections (socket returned error %s)",
                             NetworkErrorString(WSAGetLastError()));
        LogPrintf("%s\n", strError);
        return false;
    }
    if (!IsSelectableSocket(hListenSocket)) {
        strError = "Error: Couldn't create a listenable socket for incoming connections";
        LogPrintf("%s\n", strError);
        return false;
    }


#ifndef WIN32
#ifdef SO_NOSIGPIPE
    // Different way of disabling SIGPIPE on BSD
    setsockopt(hListenSocket, SOL_SOCKET, SO_NOSIGPIPE, (void*)&nOne, sizeof(int));
#endif
    // Allow binding if the port is still in TIME_WAIT state after
    // the program was closed and restarted. Not an issue on windows!
    setsockopt(hListenSocket, SOL_SOCKET, SO_REUSEADDR, (void *) &nOne, sizeof(int));
#endif

    // Set to non-blocking, incoming connections will also inherit this
    if (!SetSocketNonBlocking(hListenSocket, true)) {
        strError = strprintf("BindListenPort: Setting listening socket to non-blocking failed, error %s\n",
                             NetworkErrorString(WSAGetLastError()));
        LogPrintf("%s\n", strError);
        return false;
    }

    // some systems don't have IPV6_V6ONLY but are always v6only; others do have the option
    // and enable it by default or not. Try to enable it, if possible.
    if (addrBind.IsIPv6()) {
#ifdef IPV6_V6ONLY
#ifdef WIN32
        setsockopt(hListenSocket, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&nOne, sizeof(int));
#else
        setsockopt(hListenSocket, IPPROTO_IPV6, IPV6_V6ONLY, (void*)&nOne, sizeof(int));
#endif
#endif
#ifdef WIN32
        int nProtLevel = PROTECTION_LEVEL_UNRESTRICTED;
        setsockopt(hListenSocket, IPPROTO_IPV6, IPV6_PROTECTION_LEVEL, (const char*)&nProtLevel, sizeof(int));
#endif
    }

    if (::bind(hListenSocket, (struct sockaddr *) &sockaddr, len) == SOCKET_ERROR) {
        int nErr = WSAGetLastError();
        if (nErr == WSAEADDRINUSE)
            strError = strprintf(_("Unable to bind to %s on this computer. PRCY is probably already running."),
                                 addrBind.ToString());
        else
            strError = strprintf(_("Unable to bind to %s on this computer (bind returned error %s)"),
                                 addrBind.ToString(), NetworkErrorString(nErr));
        LogPrintf("%s\n", strError);
        CloseSocket(hListenSocket);
        return false;
    }
    LogPrintf("Bound to %s\n", addrBind.ToString());

    // Listen for incoming connections
    if (listen(hListenSocket, SOMAXCONN) == SOCKET_ERROR) {
        strError = strprintf(_("Error: Listening for incoming connections failed (listen returned error %s)"),
                             NetworkErrorString(WSAGetLastError()));
        LogPrintf("%s\n", strError);
        CloseSocket(hListenSocket);
        return false;
    }

    vhListenSocket.push_back(ListenSocket(hListenSocket, fWhitelisted));

    if (addrBind.IsRoutable() && fDiscover && !fWhitelisted)
        AddLocal(addrBind, LOCAL_BIND);

    return true;
}

void static Discover(boost::thread_group &threadGroup) {
    if (!fDiscover)
        return;

#ifdef WIN32
    // Get local host IP
    char pszHostName[256] = "";
    if (gethostname(pszHostName, sizeof(pszHostName)) != SOCKET_ERROR) {
        std::vector<CNetAddr> vaddr;
        if (LookupHost(pszHostName, vaddr, 0, true)) {
            for (const CNetAddr& addr : vaddr) {
                if (AddLocal(addr, LOCAL_IF))
                    LogPrintf("%s: %s - %s\n", __func__, pszHostName, addr.ToString());
            }
        }
    }
#else
    // Get local host ip
    struct ifaddrs *myaddrs;
    if (getifaddrs(&myaddrs) == 0) {
        for (struct ifaddrs *ifa = myaddrs; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == NULL) continue;
            if ((ifa->ifa_flags & IFF_UP) == 0) continue;
            if (strcmp(ifa->ifa_name, "lo") == 0) continue;
            if (strcmp(ifa->ifa_name, "lo0") == 0) continue;
            if (ifa->ifa_addr->sa_family == AF_INET) {
                struct sockaddr_in *s4 = (struct sockaddr_in *) (ifa->ifa_addr);
                CNetAddr addr(s4->sin_addr);
                if (AddLocal(addr, LOCAL_IF))
                    LogPrintf("%s: IPv4 %s: %s\n", __func__, ifa->ifa_name, addr.ToString());
            } else if (ifa->ifa_addr->sa_family == AF_INET6) {
                struct sockaddr_in6 *s6 = (struct sockaddr_in6 *) (ifa->ifa_addr);
                CNetAddr addr(s6->sin6_addr);
                if (AddLocal(addr, LOCAL_IF))
                    LogPrintf("%s: IPv6 %s: %s\n", __func__, ifa->ifa_name, addr.ToString());
            }
        }
        freeifaddrs(myaddrs);
    }
#endif
}

void StartNode(boost::thread_group &threadGroup, CScheduler &scheduler) {
    uiInterface.InitMessage(_("Loading addresses..."));
    // Load addresses from peers.dat
    int64_t nStart = GetTimeMillis();
    {
        CAddrDB adb;
        if (adb.Read(addrman))
            LogPrintf("Loaded %i addresses from peers.dat  %dms\n", addrman.size(), GetTimeMillis() - nStart);
        else {
            addrman.Clear(); // Addrman can be in an inconsistent state after failure, reset it
            LogPrintf("Invalid or missing peers.dat; recreating\n");
            DumpAddresses();
        }
    }

    uiInterface.InitMessage(_("Loading banlist..."));
    // Load addresses from banlist.dat
    nStart = GetTimeMillis();
    CBanDB bandb;
    banmap_t banmap;
    if (bandb.Read(banmap)) {
        CNode::SetBanned(banmap); // thread save setter
        CNode::SetBannedSetDirty(false); // no need to write down, just read data
        CNode::SweepBanned(); // sweep out unused entries

        LogPrint(BCLog::NET, "Loaded %d banned node ips/subnets from banlist.dat  %dms\n",
            banmap.size(), GetTimeMillis() - nStart);
    } else {
        LogPrintf("Invalid or missing banlist.dat; recreating\n");
        CNode::SetBannedSetDirty(true); // force write
        DumpBanlist();
    }
    // Initialize random numbers. Even when rand() is only usable for trivial use-cases most nodes should have a different
    // seed after all the file-IO done at this point. Should be good enough even when nodes are started via scripts.
    srand(time(NULL));

    fAddressesInitialized = true;

    if (semOutbound == NULL) {
        // initialize semaphore
        int nMaxOutbound = std::min((MAX_OUTBOUND_CONNECTIONS + MAX_FEELER_CONNECTIONS), nMaxConnections);
        semOutbound = new CSemaphore(nMaxOutbound);
    }

    if (pnodeLocalHost == NULL) {
        CNetAddr local;
        LookupHost("127.0.0.1", local, false);
        pnodeLocalHost = new CNode(INVALID_SOCKET, CAddress(CService(local, 0), nLocalServices));
    }

    Discover(threadGroup);

    //
    // Start threads
    //

    if (!GetBoolArg("-dnsseed", true))
        LogPrintf("DNS seeding disabled\n");
    else
        threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "dnsseed", &ThreadDNSAddressSeed));

    // Map ports with UPnP
    MapPort(GetBoolArg("-upnp", DEFAULT_UPNP));

    // Send and receive from sockets, accept connections
    threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "net", &ThreadSocketHandler));

    // Initiate outbound connections from -addnode
    threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "addcon", &ThreadOpenAddedConnections));

    // Initiate outbound connections unless connect=0
    if (!mapArgs.count("-connect") || mapMultiArgs["-connect"].size() != 1 || mapMultiArgs["-connect"][0] != "0")
        threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "opencon", &ThreadOpenConnections));

    // Process messages
    threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "msghand", &ThreadMessageHandler));

    // Dump network addresses
    scheduler.scheduleEvery(&DumpData, DUMP_ADDRESSES_INTERVAL);
}

bool StopNode() {
    LogPrintf("StopNode()\n");
    MapPort(false);
    if (semOutbound)
        for (int i = 0; i < (MAX_OUTBOUND_CONNECTIONS + MAX_FEELER_CONNECTIONS); i++)
            semOutbound->post();

    if (fAddressesInitialized) {
        DumpData();
        fAddressesInitialized = false;
    }

    return true;
}

class CNetCleanup {
public:
    CNetCleanup() {}

    ~CNetCleanup() {
        // Close sockets
        for (CNode * pnode : vNodes)
        if (pnode->hSocket != INVALID_SOCKET)
            CloseSocket(pnode->hSocket);
        for (ListenSocket & hListenSocket : vhListenSocket)
        if (hListenSocket.socket != INVALID_SOCKET)
            if (!CloseSocket(hListenSocket.socket))
                LogPrintf("CloseSocket(hListenSocket) failed with error %s\n", NetworkErrorString(WSAGetLastError()));

        // clean up some globals (to help leak detection)
        for (CNode * pnode : vNodes)
        delete pnode;
        for (CNode * pnode : vNodesDisconnected)
        delete pnode;
        vNodes.clear();
        vNodesDisconnected.clear();
        vhListenSocket.clear();
        delete semOutbound;
        semOutbound = NULL;
        delete pnodeLocalHost;
        pnodeLocalHost = NULL;

#ifdef WIN32
        // Shutdown Windows Sockets
        WSACleanup();
#endif
    }
} instance_of_cnetcleanup;

void CExplicitNetCleanup::callCleanup() {
    // Explicit call to destructor of CNetCleanup because it's not implicitly called
    // when the wallet is restarted from within the wallet itself.
    CNetCleanup *tmp = new CNetCleanup();
    delete tmp; // Stroustrup's gonna kill me for that
}

void RelayTransaction(const CTransaction &tx) {
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss.reserve(50000);
    ss << tx;
    RelayTransaction(tx, ss);
}

void RelayTransaction(const CTransaction &tx, const CDataStream &ss) {
    CInv inv(MSG_TX, tx.GetHash());
    {
        LOCK(cs_mapRelay);
        // Expire old relay messages
        while (!vRelayExpiration.empty() && vRelayExpiration.front().first < GetTime()) {
            mapRelay.erase(vRelayExpiration.front().second);
            vRelayExpiration.pop_front();
        }

        // Save original serialized message so newer versions are preserved
        mapRelay.insert(std::make_pair(inv, ss));
        vRelayExpiration.push_back(std::make_pair(GetTime() + 15 * 60, inv));
    }
    LOCK(cs_vNodes);
    for (CNode * pnode : vNodes)
    {
        if (!pnode->fRelayTxes)
            continue;
        LOCK(pnode->cs_filter);
        if (pnode->pfilter) {
            if (pnode->pfilter->IsRelevantAndUpdate(tx))
                pnode->PushInventory(inv);
        } else
            pnode->PushInventory(inv);
    }
}

void RelayTransactionLockReq(const CTransaction &tx, bool relayToAll) {
    CInv inv(MSG_TXLOCK_REQUEST, tx.GetHash());

    //broadcast the new lock
    LOCK(cs_vNodes);
    for (CNode * pnode : vNodes)
    {
        if (!relayToAll && !pnode->fRelayTxes)
            continue;

        pnode->PushMessage(NetMsgType::IX, tx);
    }
}

void RelayInv(CInv &inv) {
    LOCK(cs_vNodes);
    for (CNode * pnode : vNodes)
    {
        if ((pnode->nServices == NODE_BLOOM_WITHOUT_MN) && inv.IsMasterNodeType())continue;
        if (pnode->nVersion >= ActiveProtocol())
            pnode->PushInventory(inv);
    }
}

void CNode::RecordBytesRecv(uint64_t bytes) {
    LOCK(cs_totalBytesRecv);
    nTotalBytesRecv += bytes;
}

void CNode::RecordBytesSent(uint64_t bytes) {
    LOCK(cs_totalBytesSent);
    nTotalBytesSent += bytes;
}

uint64_t CNode::GetTotalBytesRecv() {
    LOCK(cs_totalBytesRecv);
    return nTotalBytesRecv;
}

uint64_t CNode::GetTotalBytesSent() {
    LOCK(cs_totalBytesSent);
    return nTotalBytesSent;
}

void CNode::Fuzz(int nChance) {
    if (!fSuccessfullyConnected) return; // Don't fuzz initial handshake
    if (GetRand(nChance) != 0) return;   // Fuzz 1 of every nChance messages

    switch (GetRand(3)) {
        case 0:
            // xor a random byte with a random value:
            if (!ssSend.empty()) {
                CDataStream::size_type pos = GetRand(ssSend.size());
                ssSend[pos] ^= (unsigned char) (GetRand(256));
            }
            break;
        case 1:
            // delete a random byte:
            if (!ssSend.empty()) {
                CDataStream::size_type pos = GetRand(ssSend.size());
                ssSend.erase(ssSend.begin() + pos);
            }
            break;
        case 2:
            // insert a random byte at a random position
        {
            CDataStream::size_type pos = GetRand(ssSend.size());
            char ch = (char) GetRand(256);
            ssSend.insert(ssSend.begin() + pos, ch);
        }
            break;
    }
    // Chance of more than one change half the time:
    // (more changes exponentially less likely):
    Fuzz(2);
}

//
// CAddrDB
//

CAddrDB::CAddrDB() {
    pathAddr = GetDataDir() / "peers.dat";
}

bool CAddrDB::Write(const CAddrMan &addr) {
    // Generate random temporary filename
    unsigned short randv = 0;
    GetRandBytes((unsigned char *) &randv, sizeof(randv));
    std::string tmpfn = strprintf("peers.dat.%04x", randv);

    // serialize addresses, checksum data up to that point, then append csum
    CDataStream ssPeers(SER_DISK, CLIENT_VERSION);
    ssPeers << FLATDATA(Params().MessageStart());
    ssPeers << addr;
    uint256 hash = Hash(ssPeers.begin(), ssPeers.end());
    ssPeers << hash;

    // open output file, and associate with CAutoFile
    fs::path pathAddr = GetDataDir() / "peers.dat";
    FILE* file = fsbridge::fopen(pathAddr, "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s : Failed to open file %s", __func__, pathAddr.string());

    // Write and commit header, data
    try {
        fileout << ssPeers;
    } catch (const std::exception& e) {
        return error("%s : Serialize or I/O error - %s", __func__, e.what());
    }
    FileCommit(fileout.Get());
    fileout.fclose();

    return true;
}

bool CAddrDB::Read(CAddrMan &addr) {
    // open input file, and associate with CAutoFile
    FILE* file = fsbridge::fopen(pathAddr, "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("%s : Failed to open file %s", __func__, pathAddr.string());

    // use file size to size memory buffer
    uint64_t fileSize = fs::file_size(pathAddr);
    uint64_t dataSize = fileSize - sizeof(uint256);
    // Don't try to resize to a negative number if file is small
    if (fileSize >= sizeof(uint256))
        dataSize = fileSize - sizeof(uint256);
    else
        return error("%s : Failed to open file %s", __func__, pathAddr.string());

    std::vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // read data and checksum from file
    try {
        filein.read((char *) &vchData[0], dataSize);
        filein >> hashIn;
    } catch (const std::exception& e) {
        return error("%s : Deserialize or I/O error - %s", __func__, e.what());
    }
    filein.fclose();

    CDataStream ssPeers(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssPeers.begin(), ssPeers.end());
    if (hashIn != hashTmp)
        return error("%s : Checksum mismatch, data corrupted", __func__);

    return Read(addr, ssPeers);
}

bool CAddrDB::Read(CAddrMan& addr, CDataStream& ssPeers)
{
    unsigned char pchMsgTmp[4];
    try {
        // de-serialize file header (network specific magic number) and ..
        ssPeers >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp)))
            return error("%s : Invalid network magic number", __func__);

        // de-serialize address data into one CAddrMan object
        ssPeers >> addr;
    } catch (const std::exception& e) {
        // de-serialization has failed, ensure addrman is left in a clean state
        addr.Clear();
        return error("%s : Deserialize or I/O error - %s", __func__, e.what());
    }

    return true;
}

unsigned int ReceiveFloodSize() { return 1000 * GetArg("-maxreceivebuffer", 5 * 1000); }

unsigned int SendBufferSize() { return 1000 * GetArg("-maxsendbuffer", 1 * 1000); }

CNode::CNode(SOCKET hSocketIn, CAddress addrIn, std::string addrNameIn, bool fInboundIn) :
    ssSend(SER_NETWORK, INIT_PROTO_VERSION),
    addrKnown(5000, 0.001),
    filterInventoryKnown(50000, 0.000001)
{
    nServices = NODE_NONE;
    nServicesExpected = NODE_NONE;
    hSocket = hSocketIn;
    nRecvVersion = INIT_PROTO_VERSION;
    nLastSend = 0;
    nLastRecv = 0;
    nSendBytes = 0;
    nRecvBytes = 0;
    nTimeConnected = GetTime();
    nTimeOffset = 0;
    addr = addrIn;
    addrName = addrNameIn == "" ? addr.ToStringIPPort() : addrNameIn;
    nVersion = 0;
    strSubVer = "";
    fWhitelisted = false;
    fOneShot = false;
    fClient = false; // set by version message
    fFeeler = false;
    fInbound = fInboundIn;
    fNetworkNode = false;
    fSuccessfullyConnected = false;
    fDisconnect = false;
    nRefCount = 0;
    nSendSize = 0;
    nSendOffset = 0;
    hashContinue = UINT256_ZERO;
    nStartingHeight = -1;
    filterInventoryKnown.reset();
    fGetAddr = false;
    nNextLocalAddrSend = 0;
    nNextAddrSend = 0;
    nNextInvSend = 0;
    fRelayTxes = false;
    pfilter = new CBloomFilter();
    nPingNonceSent = 0;
    nPingUsecStart = 0;
    nPingUsecTime = 0;
    fPingQueued = false;
    nMinPingUsecTime = std::numeric_limits<int64_t>::max();

    {
        LOCK(cs_nLastNodeId);
        id = nLastNodeId++;
    }

    if (fLogIPs)
        LogPrint(BCLog::NET, "Added connection to %s peer=%d\n", addrName, id);
    else
        LogPrint(BCLog::NET, "Added connection peer=%d\n", id);

    // Be shy and don't send version until we hear
    if (hSocket != INVALID_SOCKET && !fInbound)
        PushVersion();

    GetNodeSignals().InitializeNode(GetId(), this);
}

CNode::~CNode() {
    CloseSocket(hSocket);

    if (pfilter)
        delete pfilter;

    GetNodeSignals().FinalizeNode(GetId());
}

void CNode::AskFor(const CInv &inv, bool fImmediateRetry) {
    if (mapAskFor.size() > MAPASKFOR_MAX_SZ)
        return;
    // We're using mapAskFor as a priority queue,
    // the key is the earliest time the request can be sent
    int64_t nRequestTime;
    limitedmap<CInv, int64_t>::const_iterator it = mapAlreadyAskedFor.find(inv);
    if (it != mapAlreadyAskedFor.end())
        nRequestTime = it->second;
    else
        nRequestTime = 0;
    LogPrint(BCLog::NET, "askfor %s  %d (%s) peer=%d\n", inv.ToString(), nRequestTime,
             DateTimeStrFormat("%H:%M:%S", nRequestTime / 1000000), id);
    //LogPrintf("askfor %s  %d (%s) peer=%d\n", inv.ToString(), nRequestTime,
             //DateTimeStrFormat("%H:%M:%S", nRequestTime / 1000000), id);
    // Make sure not to reuse time indexes to keep things in the same order
    int64_t nNow = GetTimeMicros() - 1000000;
    static int64_t nLastTime;
    ++nLastTime;
    nNow = std::max(nNow, nLastTime);
    nLastTime = nNow;

    // Each retry is 2 minutes after the last
    nRequestTime = std::max(nRequestTime + 2 * 60 * 1000000, nNow);
    if (fImmediateRetry)
        nRequestTime = nNow;
    else
        nRequestTime = std::max(nRequestTime + 2 * 60 * 1000000, nNow);
    if (it != mapAlreadyAskedFor.end())
        mapAlreadyAskedFor.update(it, nRequestTime);
    else
        mapAlreadyAskedFor.insert(std::make_pair(inv, nRequestTime));
    mapAskFor.insert(std::make_pair(nRequestTime, inv));
}

void CNode::BeginMessage(const char *pszCommand) EXCLUSIVE_LOCK_FUNCTION(cs_vSend) {
    ENTER_CRITICAL_SECTION(cs_vSend);
    assert(ssSend.size() == 0);
    ssSend << CMessageHeader(pszCommand, 0);
    LogPrint(BCLog::NET, "sending: %s ", SanitizeString(pszCommand));
}

void CNode::AbortMessage() UNLOCK_FUNCTION(cs_vSend) {
    ssSend.clear();

    LEAVE_CRITICAL_SECTION(cs_vSend);

    LogPrint(BCLog::NET, "(aborted)\n");
}

void CNode::EndMessage() UNLOCK_FUNCTION(cs_vSend) {
    // The -*messagestest options are intentionally not documented in the help message,
    // since they are only used during development to debug the networking code and are
    // not intended for end-users.
    if (mapArgs.count("-dropmessagestest") && GetRand(GetArg("-dropmessagestest", 2)) == 0) {
        LogPrint(BCLog::NET, "dropmessages DROPPING SEND MESSAGE\n");
        AbortMessage();
        return;
    }
    if (mapArgs.count("-fuzzmessagestest"))
        Fuzz(GetArg("-fuzzmessagestest", 10));

    if (ssSend.size() == 0) {
        LEAVE_CRITICAL_SECTION(cs_vSend);
        return;
    }

    // Set the size
    unsigned int nSize = ssSend.size() - CMessageHeader::HEADER_SIZE;
    WriteLE32((uint8_t*)&ssSend[CMessageHeader::MESSAGE_SIZE_OFFSET], nSize);

    // Set the checksum
    uint256 hash = Hash(ssSend.begin() + CMessageHeader::HEADER_SIZE, ssSend.end());
    unsigned int nChecksum = 0;
    memcpy(&nChecksum, &hash, sizeof(nChecksum));
    assert(ssSend.size() >= CMessageHeader::CHECKSUM_OFFSET + sizeof(nChecksum));
    memcpy((char *) &ssSend[CMessageHeader::CHECKSUM_OFFSET], &nChecksum, sizeof(nChecksum));

    LogPrint(BCLog::NET, "(%d bytes) peer=%d\n", nSize, id);

    std::deque<CSerializeData>::iterator it = vSendMsg.insert(vSendMsg.end(), CSerializeData());
    ssSend.GetAndClear(*it);
    nSendSize += (*it).size();

    // If write queue empty, attempt "optimistic write"
    if (it == vSendMsg.begin())
        SocketSendData(this);

    LEAVE_CRITICAL_SECTION(cs_vSend);
}

//
// CBanDB
//

CBanDB::CBanDB() {
    pathBanlist = GetDataDir() / "banlist.dat";
}

bool CBanDB::Write(const banmap_t &banSet) {
    // Generate random temporary filename
    unsigned short randv = 0;
    GetRandBytes((unsigned char *) &randv, sizeof(randv));
    std::string tmpfn = strprintf("banlist.dat.%04x", randv);
    // serialize banlist, checksum data up to that point, then append csum
    CDataStream ssBanlist(SER_DISK, CLIENT_VERSION);
    ssBanlist << FLATDATA(Params().MessageStart());
    ssBanlist << banSet;
    uint256 hash = Hash(ssBanlist.begin(), ssBanlist.end());
    ssBanlist << hash;
    // open temp output file, and associate with CAutoFile
    fs::path pathTmp = GetDataDir() / tmpfn;
    FILE *file = fsbridge::fopen(pathTmp, "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s: Failed to open file %s", __func__, pathTmp.string());
    // Write and commit header, data
    try {
        fileout << ssBanlist;
    }
    catch (const std::exception& e) {
        return error("%s: Serialize or I/O error - %s", __func__, e.what());
    }
    FileCommit(fileout.Get());
    fileout.fclose();
    // replace existing banlist.dat, if any, with new banlist.dat.XXXX
    if (!RenameOver(pathTmp, pathBanlist))
        return error("%s: Rename-into-place failed", __func__);
    return true;
}

bool CBanDB::Read(banmap_t &banSet) {
    // open input file, and associate with CAutoFile
    FILE *file = fsbridge::fopen(pathBanlist, "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("%s: Failed to open file %s", __func__, pathBanlist.string());
    // use file size to size memory buffer
    uint64_t fileSize = fs::file_size(pathBanlist);
    uint64_t dataSize = 0;
    // Don't try to resize to a negative number if file is small
    if (fileSize >= sizeof(uint256))
        dataSize = fileSize - sizeof(uint256);
    std::vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;
    // read data and checksum from file
    try {
        filein.read((char *) &vchData[0], dataSize);
        filein >> hashIn;
    }
    catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }
    filein.fclose();
    CDataStream ssBanlist(vchData, SER_DISK, CLIENT_VERSION);
    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssBanlist.begin(), ssBanlist.end());
    if (hashIn != hashTmp)
        return error("%s: Checksum mismatch, data corrupted", __func__);
    unsigned char pchMsgTmp[4];
    try {
        // de-serialize file header (network specific magic number) and ..
        ssBanlist >> FLATDATA(pchMsgTmp);
        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp)))
            return error("%s: Invalid network magic number", __func__);
        // de-serialize address data into one CAddrMan object
        ssBanlist >> banSet;
    }
    catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }
    return true;
}

// valid, reachable and routable address (except for RegTest)
bool validateMasternodeIP(const std::string& addrStr)
{
    CNetAddr resolved;
    if (LookupHost(addrStr.c_str(), resolved, false)) {
        return ((IsReachable(resolved) && resolved.IsRoutable()) ||
                (Params().IsRegTestNet() && resolved.IsValid()));
    }
    return false;
}

int64_t PoissonNextSend(int64_t nNow, int average_interval_seconds) {
    return nNow + (int64_t)(log1p(GetRand(1ULL << 48) * -0.0000000000000035527136788 /* -1/2^48 */) * average_interval_seconds * -1000000.0 + 0.5);
}
