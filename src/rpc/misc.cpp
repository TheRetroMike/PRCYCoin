// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2018 The PIVX developers
// Copyright (c) 2018-2020 The DAPS Project developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "base58.h"
#include "clientversion.h"
#include "httpserver.h"
#include "init.h"
#include "main.h"
#include "masternode-sync.h"
#include "net.h"
#include "netbase.h"
#include "rpc/server.h"
#include "timedata.h"
#include "util.h"

#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#include "wallet/walletdb.h"
#endif

#include <stdint.h>

#include <univalue.h>
#include <boost/assign/list_of.hpp>


/**
 * @note Do not add or change anything in the information returned by this
 * method. `getinfo` exists for backwards-compatibility only. It combines
 * information from wildly different sources in the program, which is a mess,
 * and is thus planned to be deprecated eventually.
 *
 * Based on the source of the information, new information should be added to:
 * - `getblockchaininfo`,
 * - `getnetworkinfo` or
 * - `getwalletinfo`
 *
 * Or alternatively, create a specific query method for the information.
 **/
UniValue getinfo(const UniValue &params, bool fHelp) {
    if (fHelp || params.size() != 0)
        throw std::runtime_error(
            "getinfo\n"
            "Returns an object containing various state info.\n"
            "\nResult:\n"
            "{\n"
            "  \"version\": xxxxx,           (numeric) the server version\n"
            "  \"protocolversion\": xxxxx,   (numeric) the protocol version\n"
            "  \"walletversion\": xxxxx,     (numeric) the wallet version\n"
            "  \"balance\": xxxxxxx,         (numeric) the total prcycoin balance of the wallet\n"
            "  \"blocks\": xxxxxx,           (numeric) the current number of blocks processed in the server\n"
            "  \"synced\": xxxxxx,           (boolean) if the server is synced or not\n"
            "  \"timeoffset\": xxxxx,        (numeric) the time offset\n"
            "  \"connections\": xxxxx,       (numeric) the number of connections\n"
            "  \"proxy\": \"host:port\",     (string, optional) the proxy used by the server\n"
            "  \"difficulty\": xxxxxx,       (numeric) the current difficulty\n"
            "  \"testnet\": true|false,      (boolean) if the server is using testnet or not\n"
            "  \"moneysupply\" : \"supply\"  (numeric) The money supply when this block was added to the blockchain\n"
            "  \"keypoololdest\": xxxxxx,    (numeric) the timestamp (seconds since GMT epoch) of the oldest pre-generated key in the key pool\n"
            "  \"keypoolsize\": xxxx,        (numeric) how many new keys are pre-generated\n"
            "  \"unlocked_until\": ttt,      (numeric) the timestamp in seconds since epoch (midnight Jan 1 1970 GMT) that the wallet is unlocked for transfers, or 0 if the wallet is locked\n"
            "  \"paytxfee\": x.xxxx,         (numeric) the transaction fee set in prcycoin/kb\n"
            "  \"relayfee\": x.xxxx,         (numeric) minimum relay fee for non-free transactions in prcycoin/kb\n"
            "  \"staking mode\": enabled|disabled,  (string) if staking is enabled or disabled\n"
            "  \"staking status\": active|inactive, (string) if staking is active or inactive\n"
            "  \"errors\": \"...\"           (string) any error messages\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("getinfo", "") + HelpExampleRpc("getinfo", ""));
    LOCK(cs_main);
    proxyType proxy;
    GetProxy(NET_IPV4, proxy);

    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("version", CLIENT_VERSION));
    obj.push_back(Pair("protocolversion", PROTOCOL_VERSION));
#ifdef ENABLE_WALLET
    if (pwalletMain) {
        obj.push_back(Pair("walletversion", pwalletMain->GetVersion()));
        obj.push_back(Pair("balance", ValueFromAmount(pwalletMain->GetBalance())));
    }
#endif
    obj.push_back(Pair("blocks", (int) chainActive.Height()));
    obj.push_back(Pair("synced", masternodeSync.IsBlockchainSynced()));
    obj.push_back(Pair("timeoffset", GetTimeOffset()));
    obj.push_back(Pair("connections", (int) vNodes.size()));
    obj.push_back(Pair("proxy", (proxy.IsValid() ? proxy.proxy.ToStringIPPort() : std::string())));
    obj.push_back(Pair("difficulty", (double) GetDifficulty()));
    obj.push_back(Pair("testnet", Params().TestnetToBeDeprecatedFieldRPC()));
    obj.push_back(Pair("moneysupply",ValueFromAmount(chainActive.Tip()->nMoneySupply)));

#ifdef ENABLE_WALLET
    if (pwalletMain) {
        obj.push_back(Pair("keypoololdest", pwalletMain->GetOldestKeyPoolTime()));
    }
    if (pwalletMain && pwalletMain->IsCrypted())
        obj.push_back(Pair("unlocked_until", nWalletUnlockTime));
    obj.push_back(Pair("paytxfee", ValueFromAmount(payTxFee.GetFeePerK())));
#endif
    obj.push_back(Pair("relayfee", ValueFromAmount(::minRelayTxFee.GetFeePerK())));
    bool nStaking = false;
    if (mapHashedBlocks.count(chainActive.Tip()->nHeight))
        nStaking = true;
    else if (mapHashedBlocks.count(chainActive.Tip()->nHeight - 1) && nLastCoinStakeSearchInterval)
        nStaking = true;
    if (pwalletMain->IsLocked()) {
        obj.push_back(Pair("staking mode", ("disabled")));
        obj.push_back(Pair("staking status", ("inactive (wallet locked)")));
    } else {
        obj.push_back(Pair("staking mode", (pwalletMain->ReadStakingStatus() ? "enabled" : "disabled")));
        if (vNodes.empty()) {
            obj.push_back(Pair("staking status", ("inactive (no peer connections)")));
        } else if (!masternodeSync.IsSynced()) {
            obj.push_back(Pair("staking status", ("inactive (syncing masternode list)")));
        } else if (!pwalletMain->MintableCoins() && pwalletMain->combineMode == CombineMode::ON) {
            obj.push_back(Pair("staking status", ("delayed (waiting for 100 blocks)")));
        } else if (!pwalletMain->MintableCoins()) {
            obj.push_back(Pair("staking status", ("inactive (no mintable coins)")));
        } else {
            obj.push_back(Pair("staking status", (nStaking ? "active (attempting to mint a block)" : "idle (waiting for next round)")));
        }
    }
    obj.push_back(Pair("errors", GetWarnings("statusbar")));
    return obj;
}

UniValue getversion(const UniValue &params, bool fHelp) {
    if (fHelp || params.size() != 0)
        throw std::runtime_error(
            "getversion\n"
            "Returns the server version.\n"
            "\nResult:\n"
            "{\n"
            "  \"version\": xxxxx,           (numeric) the server version\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("getversion", "") + HelpExampleRpc("getversion", ""));
    LOCK(cs_main);

    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("version", CLIENT_VERSION));
    return obj;
}

UniValue mnsync(const UniValue &params, bool fHelp) {
    std::string strMode;
    if (params.size() == 1)
        strMode = params[0].get_str();

    if (fHelp || params.size() != 1 || (strMode != "status" && strMode != "reset")) {
        throw std::runtime_error(
                "mnsync \"status|reset\"\n"
                "\nReturns the sync status or resets sync.\n"

                "\nArguments:\n"
                "1. \"mode\"    (string, required) either 'status' or 'reset'\n"

                "\nResult ('status' mode):\n"
                "{\n"
                "  \"IsBlockchainSynced\": true|false,    (boolean) 'true' if blockchain is synced\n"
                "  \"lastMasternodeList\": xxxx,        (numeric) Timestamp of last MN list message\n"
                "  \"lastMasternodeWinner\": xxxx,      (numeric) Timestamp of last MN winner message\n"
                "  \"lastBudgetItem\": xxxx,            (numeric) Timestamp of last MN budget message\n"
                "  \"lastFailure\": xxxx,           (numeric) Timestamp of last failed sync\n"
                "  \"nCountFailures\": n,           (numeric) Number of failed syncs (total)\n"
                "  \"sumMasternodeList\": n,        (numeric) Number of MN list messages (total)\n"
                "  \"sumMasternodeWinner\": n,      (numeric) Number of MN winner messages (total)\n"
                "  \"sumBudgetItemProp\": n,        (numeric) Number of MN budget messages (total)\n"
                "  \"sumBudgetItemFin\": n,         (numeric) Number of MN budget finalization messages (total)\n"
                "  \"countMasternodeList\": n,      (numeric) Number of MN list messages (local)\n"
                "  \"countMasternodeWinner\": n,    (numeric) Number of MN winner messages (local)\n"
                "  \"countBudgetItemProp\": n,      (numeric) Number of MN budget messages (local)\n"
                "  \"countBudgetItemFin\": n,       (numeric) Number of MN budget finalization messages (local)\n"
                "  \"RequestedMasternodeAssets\": n, (numeric) Status code of last sync phase\n"
                "  \"RequestedMasternodeAttempt\": n, (numeric) Status code of last sync attempt\n"
                "}\n"

                "\nResult ('reset' mode):\n"
                "\"status\"     (string) 'success'\n"
                "\nExamples:\n" +
                HelpExampleCli("mnsync", "\"status\"") + HelpExampleRpc("mnsync", "\"status\""));
    }

    if (strMode == "status") {
        UniValue obj(UniValue::VOBJ);

        obj.push_back(Pair("IsBlockchainSynced", masternodeSync.IsBlockchainSynced()));
        obj.push_back(Pair("lastMasternodeList", masternodeSync.lastMasternodeList));
        obj.push_back(Pair("lastMasternodeWinner", masternodeSync.lastMasternodeWinner));
        obj.push_back(Pair("lastBudgetItem", masternodeSync.lastBudgetItem));
        obj.push_back(Pair("lastFailure", masternodeSync.lastFailure));
        obj.push_back(Pair("nCountFailures", masternodeSync.nCountFailures));
        obj.push_back(Pair("sumMasternodeList", masternodeSync.sumMasternodeList));
        obj.push_back(Pair("sumMasternodeWinner", masternodeSync.sumMasternodeWinner));
        obj.push_back(Pair("sumBudgetItemProp", masternodeSync.sumBudgetItemProp));
        obj.push_back(Pair("sumBudgetItemFin", masternodeSync.sumBudgetItemFin));
        obj.push_back(Pair("countMasternodeList", masternodeSync.countMasternodeList));
        obj.push_back(Pair("countMasternodeWinner", masternodeSync.countMasternodeWinner));
        obj.push_back(Pair("countBudgetItemProp", masternodeSync.countBudgetItemProp));
        obj.push_back(Pair("countBudgetItemFin", masternodeSync.countBudgetItemFin));
        obj.push_back(Pair("RequestedMasternodeAssets", masternodeSync.RequestedMasternodeAssets));
        obj.push_back(Pair("RequestedMasternodeAttempt", masternodeSync.RequestedMasternodeAttempt));

        return obj;
    }

    if (strMode == "reset") {
        masternodeSync.Reset();
        return "success";
    }
    return "failure";
}

#ifdef ENABLE_WALLET
class DescribeAddressVisitor : public boost::static_visitor<UniValue>
{
private:
    isminetype mine;

public:
    DescribeAddressVisitor(isminetype mineIn) : mine(mineIn) {}

    UniValue operator()(const CNoDestination &dest) const { return UniValue(UniValue::VOBJ); }

    UniValue operator()(const CKeyID &keyID) const
    {
        UniValue obj(UniValue::VOBJ);
        CPubKey vchPubKey;
        obj.push_back(Pair("isscript", false));
        if (mine == ISMINE_SPENDABLE) {
            pwalletMain->GetPubKey(keyID, vchPubKey);
            obj.push_back(Pair("pubkey", HexStr(vchPubKey)));
            obj.push_back(Pair("iscompressed", vchPubKey.IsCompressed()));
        }
        return obj;
    }

    UniValue operator()(const CScriptID &scriptID) const
    {
        UniValue obj(UniValue::VOBJ);
        obj.push_back(Pair("isscript", true));
        CScript subscript;
        pwalletMain->GetCScript(scriptID, subscript);
        std::vector<CTxDestination> addresses;
        txnouttype whichType;
        int nRequired;
        ExtractDestinations(subscript, whichType, addresses, nRequired);
        obj.push_back(Pair("script", GetTxnOutputType(whichType)));
        obj.push_back(Pair("hex", HexStr(subscript.begin(), subscript.end())));
        UniValue a(UniValue::VARR);
        for (const CTxDestination& addr : addresses)
            a.push_back(CBitcoinAddress(addr).ToString());
        obj.push_back(Pair("addresses", a));
        if (whichType == TX_MULTISIG)
            obj.push_back(Pair("sigsrequired", nRequired));
        return obj;
    }
};
#endif

UniValue validateaddress(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error(
                "validateaddress \"prcycoinaddress\"\n"
                "\nReturn information about the given prcycoin address.\n"
                "\nArguments:\n"
                "1. \"prcycoinaddress\"     (string, required) The prcycoin address to validate\n"
                "\nResult:\n"
                "{\n"
                "  \"isvalid\" : true|false,         (boolean) If the address is valid or not. If not, this is the only property returned.\n"
                "  \"address\" : \"prcycoinaddress\", (string) The prcycoin address validated\n"
                "  \"scriptPubKey\" : \"hex\",       (string) The hex encoded scriptPubKey generated by the address\n"
                "  \"ismine\" : true|false,          (boolean) If the address is yours or not\n"
                "  \"iswatchonly\" : true|false,     (boolean) If the address is watchonly\n"
                "  \"isscript\" : true|false,        (boolean) If the key is a script\n"
                "  \"hex\" : \"hex\",                (string, optional) The redeemscript for the P2SH address\n"
                "  \"pubkey\" : \"publickeyhex\",    (string) The hex value of the raw public key\n"
                "  \"iscompressed\" : true|false,    (boolean) If the address is compressed\n"
                "  \"account\" : \"account\"         (string) The account associated with the address, \"\" is the default account\n"
                "}\n"
                "\nExamples:\n" +
                HelpExampleCli("validateaddress", "\"1PSSGeFHDnKNxiEyFrD1wcEaHr9hrQDDWc\"") +
                HelpExampleRpc("validateaddress", "\"1PSSGeFHDnKNxiEyFrD1wcEaHr9hrQDDWc\""));

#ifdef ENABLE_WALLET
        LOCK2(cs_main, pwalletMain ? &pwalletMain->cs_wallet : NULL);
#else
    LOCK(cs_main);
#endif

    CBitcoinAddress address(params[0].get_str());
    bool isValid = address.IsValid();

    UniValue ret(UniValue::VOBJ);
    ret.push_back(Pair("isvalid", isValid));
    if (isValid) {
        CTxDestination dest = address.Get();
        std::string currentAddress = address.ToString();
        ret.push_back(Pair("address", currentAddress));
        CScript scriptPubKey = GetScriptForDestination(dest);
        ret.push_back(Pair("scriptPubKey", HexStr(scriptPubKey.begin(), scriptPubKey.end())));

#ifdef ENABLE_WALLET
        isminetype mine = pwalletMain ? IsMine(*pwalletMain, dest) : ISMINE_NO;
        ret.push_back(Pair("ismine", bool(mine & ISMINE_SPENDABLE)));
        ret.push_back(Pair("iswatchonly", bool(mine & ISMINE_WATCH_ONLY)));
        UniValue detail = boost::apply_visitor(DescribeAddressVisitor(mine), dest);
        ret.pushKVs(detail);
        if (pwalletMain && pwalletMain->mapAddressBook.count(dest))
            ret.push_back(Pair("account", pwalletMain->mapAddressBook[dest].name));
#endif
    }
    return ret;
}

UniValue validatestealthaddress(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error(
                "validatestealthaddress \"prcycoinstealthaddress\"\n"
                "\nReturn information about the given prcycoin stealth address.\n"
                "\nArguments:\n"
                "1. \"prcycoinstealthaddress\"     (string, required) The prcycoin stealth address to validate\n"
                "\nResult:\n"
                "{\n"
                "  \"isvalid\" : true|false,         (boolean) If the address is valid or not. If not, this is the only property returned.\n"
                "}\n"
                "\nExamples:\n" +
                HelpExampleCli("validatestealthaddress", "\"Pap5WCV4SjVMGLyYf98MEX82ErBEMVpg9ViQ1up3aBib6Fz4841SahrRXG6eSNSLBSNvEiGuQiWKXJC3RDfmotKv15oCrh6N2Ym\"") +
                HelpExampleRpc("validatestealthaddress", "\"Pap5WCV4SjVMGLyYf98MEX82ErBEMVpg9ViQ1up3aBib6Fz4841SahrRXG6eSNSLBSNvEiGuQiWKXJC3RDfmotKv15oCrh6N2Ym\""));
    EnsureWallet();

    std::string addr = params[0].get_str();

    UniValue ret(UniValue::VOBJ);
    CPubKey viewKey, spendKey;
    bool hasPaymentID;
    uint64_t paymentID;
    bool isValid = true;

    if (!CWallet::DecodeStealthAddress(addr, viewKey, spendKey, hasPaymentID, paymentID)) {
        isValid = false;
    }
    ret.push_back(Pair("isvalid", isValid));

    return ret;
}

/**
 * Used by addmultisigaddress / createmultisig:
 */
CScript _createmultisig_redeemScript(const UniValue& params) {
    int nRequired = params[0].get_int();
    const UniValue& keys = params[1].get_array();

    // Gather public keys
    if (nRequired < 1)
        throw std::runtime_error("a multisignature address must require at least one key to redeem");
    if ((int) keys.size() < nRequired)
        throw std::runtime_error(
                strprintf("not enough keys supplied "
                          "(got %u keys, but need at least %d to redeem)",
                          keys.size(), nRequired));
    if (keys.size() > 16)
        throw std::runtime_error(
                "Number of addresses involved in the multisignature address creation > 16\nReduce the number");
    std::vector <CPubKey> pubkeys;
    pubkeys.resize(keys.size());
    for (unsigned int i = 0; i < keys.size(); i++) {
        const std::string &ks = keys[i].get_str();
#ifdef ENABLE_WALLET
        // Case 1: PRCY address and we have full public key:
        CBitcoinAddress address(ks);
        if (pwalletMain && address.IsValid()) {
            CKeyID keyID;
            if (!address.GetKeyID(keyID))
                throw std::runtime_error(
                    strprintf("%s does not refer to a key", ks));
            CPubKey vchPubKey;
            if (!pwalletMain->GetPubKey(keyID, vchPubKey))
                throw std::runtime_error(
                    strprintf("no full public key for address %s", ks));
            if (!vchPubKey.IsFullyValid())
                throw std::runtime_error(" Invalid public key: " + ks);
            pubkeys[i] = vchPubKey;
        }

        // Case 2: hex public key
        else
#endif
        if (IsHex(ks)) {
            CPubKey vchPubKey(ParseHex(ks));
            if (!vchPubKey.IsFullyValid())
                throw std::runtime_error(" Invalid public key: " + ks);
            pubkeys[i] = vchPubKey;
        } else {
            throw std::runtime_error(" Invalid public key: " + ks);
        }
    }
    CScript result = GetScriptForMultisig(nRequired, pubkeys);

    if (result.size() > MAX_SCRIPT_ELEMENT_SIZE)
        throw std::runtime_error(
                strprintf("redeemScript exceeds size limit: %d > %d", result.size(), MAX_SCRIPT_ELEMENT_SIZE));

    return result;
}

UniValue createmultisig(const UniValue& params, bool fHelp) {
    if (fHelp || params.size() < 2 || params.size() > 2) {
        std::string msg = "createmultisig nrequired [\"key\",...]\n"
                     "\nCreates a multi-signature address with n signature of m keys required.\n"
                     "It returns a json object with the address and redeemScript.\n"

                     "\nArguments:\n"
                     "1. nrequired      (numeric, required) The number of required signatures out of the n keys or addresses.\n"
                     "2. \"keys\"       (string, required) A json array of keys which are prcycoin addresses or hex-encoded public keys\n"
                     "     [\n"
                     "       \"key\"    (string) prcycoin address or hex-encoded public key\n"
                     "       ,...\n"
                     "     ]\n"

                     "\nResult:\n"
                     "{\n"
                     "  \"address\":\"multisigaddress\",  (string) The value of the new multisig address.\n"
                     "  \"redeemScript\":\"script\"       (string) The string value of the hex-encoded redemption script.\n"
                     "}\n"

                     "\nExamples:\n"
                     "\nCreate a multisig address from 2 addresses\n" +
                     HelpExampleCli("createmultisig",
                                    "2 \"[\\\"16sSauSf5pF2UkUwvKGq4qjNRzBZYqgEL5\\\",\\\"171sgjn4YtPu27adkKGrdDwzRTxnRkBfKV\\\"]\"") +
                     "\nAs a json rpc call\n" + HelpExampleRpc("createmultisig",
                                                               "2, \"[\\\"16sSauSf5pF2UkUwvKGq4qjNRzBZYqgEL5\\\",\\\"171sgjn4YtPu27adkKGrdDwzRTxnRkBfKV\\\"]\"");
        throw std::runtime_error(msg);
    }

    // Construct using pay-to-script-hash:
    CScript inner = _createmultisig_redeemScript(params);
    CScriptID innerID(inner);
    CBitcoinAddress address(innerID);

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("address", address.ToString()));
    result.push_back(Pair("redeemScript", HexStr(inner.begin(), inner.end())));

    return result;
}

UniValue verifymessage(const UniValue& params, bool fHelp) {
    if (fHelp || params.size() != 3)
        throw std::runtime_error(
            "verifymessage \"prcycoinaddress\" \"signature\" \"message\"\n"
            "\nVerify a signed message\n"
            "\nArguments:\n"
            "1. \"prcycoinaddress\"  (string, required) The prcycoin address to use for the signature.\n"
            "2. \"signature\"       (string, required) The signature provided by the signer in base 64 encoding (see signmessage).\n"
            "3. \"message\"         (string, required) The message that was signed.\n"
            "\nResult:\n"
            "true|false   (boolean) If the signature is verified or not.\n"
            "\nExamples:\n"
            "\nUnlock the wallet for 30 seconds\n" +
            HelpExampleCli("unlockwallet", "\"mypassphrase\" 30") +
            "\nCreate the signature\n" + HelpExampleCli("signmessage", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XZ\" \"my message\"") +
            "\nVerify the signature\n" + HelpExampleCli("verifymessage", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XZ\" \"signature\" \"my message\"") +
            "\nAs json rpc\n" + HelpExampleRpc("verifymessage", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XZ\", \"signature\", \"my message\""));

    std::string strAddress = params[0].get_str();
    std::string strSign = params[1].get_str();
    std::string strMessage = params[2].get_str();

    CBitcoinAddress addr(strAddress);
    if (!addr.IsValid())
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

    CKeyID keyID;
    if (!addr.GetKeyID(keyID))
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

    bool fInvalid = false;
    std::vector<unsigned char> vchSig = DecodeBase64(strSign.c_str(), &fInvalid);

    if (fInvalid)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Malformed base64 encoding");

    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    CPubKey pubkey;
    if (!pubkey.RecoverCompact(ss.GetHash(), vchSig))
        return false;

    return (pubkey.GetID() == keyID);
}

UniValue setmocktime(const UniValue& params, bool fHelp) {
    if (fHelp || params.size() != 1)
        throw std::runtime_error(
                "setmocktime timestamp\n"
                "\nSet the local time to given timestamp (-regtest only)\n"
                "\nArguments:\n"
                "1. timestamp  (integer, required) Unix seconds-since-epoch timestamp\n"
                "   Pass 0 to go back to using the system time.");

    if (!Params().MineBlocksOnDemand())
        throw std::runtime_error("setmocktime for regression testing (-regtest mode) only");

    LOCK(cs_main);

    RPCTypeCheck(params, boost::assign::list_of(UniValue::VNUM));
    SetMockTime(params[0].get_int64());

    return NullUniValue;
}

void EnableOrDisableLogCategories(UniValue cats, bool enable) {
    cats = cats.get_array();
    for (unsigned int i = 0; i < cats.size(); ++i) {
        std::string cat = cats[i].get_str();

        bool success;
        if (enable) {
            success = g_logger->EnableCategory(cat);
        } else {
            success = g_logger->DisableCategory(cat);
        }

        if (!success)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "unknown logging category " + cat);
    }
}

UniValue logging(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 2) {
        throw std::runtime_error(
            "logging [include,...] <exclude>\n"
            "Gets and sets the logging configuration.\n"
            "When called without an argument, returns the list of categories that are currently being debug logged.\n"
            "When called with arguments, adds or removes categories from debug logging.\n"
            "The valid logging categories are: " + ListLogCategories() + "\n"
            "libevent logging is configured on startup and cannot be modified by this RPC during runtime."
            "Arguments:\n"
            "1. \"include\" (array of strings) add debug logging for these categories.\n"
            "2. \"exclude\" (array of strings) remove debug logging for these categories.\n"
            "\nResult: <categories>  (string): a list of the logging categories that are active.\n"
            "\nExamples:\n"
            + HelpExampleCli("logging", "\"[\\\"all\\\"]\" \"[\\\"http\\\"]\"")
            + HelpExampleRpc("logging", "[\"all\"], \"[libevent]\"")
        );
    }

    uint32_t original_log_categories = g_logger->GetCategoryMask();
    if (params.size() > 0 && params[0].isArray()) {
        EnableOrDisableLogCategories(params[0], true);
    }

    if (params.size() > 1 && params[1].isArray()) {
        EnableOrDisableLogCategories(params[1], false);
    }

    uint32_t updated_log_categories = g_logger->GetCategoryMask();
    uint32_t changed_log_categories = original_log_categories ^ updated_log_categories;

    // Update libevent logging if BCLog::LIBEVENT has changed.
    // If the library version doesn't allow it, UpdateHTTPServerLogging() returns false,
    // in which case we should clear the BCLog::LIBEVENT flag.
    // Throw an error if the user has explicitly asked to change only the libevent
    // flag and it failed.
    if (changed_log_categories & BCLog::LIBEVENT) {
        if (!UpdateHTTPServerLogging(g_logger->WillLogCategory(BCLog::LIBEVENT))) {
            g_logger->DisableCategory(BCLog::LIBEVENT);
            if (changed_log_categories == BCLog::LIBEVENT) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "libevent logging cannot be updated when using libevent before v2.1.1.");
            }
        }
    }

    UniValue result(UniValue::VOBJ);
    std::vector<CLogCategoryActive> vLogCatActive = ListActiveLogCategories();
    for (const auto& logCatActive : vLogCatActive) {
        result.pushKV(logCatActive.category, logCatActive.active);
    }

    return result;
}

#ifdef ENABLE_WALLET
UniValue getstakingstatus(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw std::runtime_error(
            "getstakingstatus\n"
            "Returns an object containing various staking information.\n"
            "\nResult:\n"
            "{\n"
            "  \"haveconnections\": true|false,     (boolean) if network connections are present\n"
            "  \"walletunlocked\": true|false,      (boolean) if the wallet is unlocked\n"
            "  \"mintablecoins\": true|false,       (boolean) if the wallet has mintable coins\n"
            "  \"enoughcoins\": true|false,         (boolean) if available coins are greater than reserve balance\n"
            "  \"masternodes-synced\": true|false,  (boolean) if masternode data is synced\n"
            "  \"staking mode\": enabled|disabled,  (string) if staking is enabled or disabled\n"
            "  \"staking status\": active|inactive, (string) if staking is active or inactive\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("getstakingstatus", "") + HelpExampleRpc("getstakingstatus", ""));

#ifdef ENABLE_WALLET
    LOCK2(cs_main, pwalletMain ? &pwalletMain->cs_wallet : NULL);
#else
    LOCK(cs_main);
#endif


    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("haveconnections", !vNodes.empty()));
    if (pwalletMain) {
        obj.push_back(Pair("walletunlocked", !pwalletMain->IsLocked()));
        obj.push_back(Pair("mintablecoins", pwalletMain->MintableCoins()));
        obj.push_back(Pair("enoughcoins", nReserveBalance <= pwalletMain->GetBalance()));
    }
    obj.push_back(Pair("masternodes-synced", masternodeSync.IsSynced()));

    bool nStaking = false;
    if (mapHashedBlocks.count(chainActive.Tip()->nHeight))
        nStaking = true;
    else if (mapHashedBlocks.count(chainActive.Tip()->nHeight - 1) && nLastCoinStakeSearchInterval)
        nStaking = true;
    if (pwalletMain->IsLocked()) {
        obj.push_back(Pair("staking mode", ("disabled")));
        obj.push_back(Pair("staking status", ("inactive (wallet locked)")));
    } else {
        obj.push_back(Pair("staking mode", (pwalletMain->ReadStakingStatus() ? "enabled" : "disabled")));
        if (vNodes.empty()) {
            obj.push_back(Pair("staking status", ("inactive (no peer connections)")));
        } else if (!masternodeSync.IsSynced()) {
            obj.push_back(Pair("staking status", ("inactive (syncing masternode list)")));
        } else if (!pwalletMain->MintableCoins() && pwalletMain->combineMode == CombineMode::ON) {
            obj.push_back(Pair("staking status", ("delayed (waiting for 100 blocks)")));
        } else if (!pwalletMain->MintableCoins()) {
            obj.push_back(Pair("staking status", ("inactive (no mintable coins)")));
        } else {
            obj.push_back(Pair("staking status", (nStaking ? "active (attempting to mint a block)" : "idle (waiting for next round)")));
        }
    }
    return obj;
}
#endif // ENABLE_WALLET
