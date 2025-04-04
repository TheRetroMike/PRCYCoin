// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2018 The PIVX developers
// Copyright (c) 2018-2020 The DAPS Project developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "main.h"

#include "addrman.h"
#include "amount.h"
#include "blocksignature.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "checkqueue.h"
#include "consensus/merkle.h"
#include "consensus/tx_verify.h"
#include "consensus/validation.h"
#include "fs.h"
#include "init.h"
#include "invalid.h"
#include "kernel.h"
#include "masternode-budget.h"
#include "masternode-payments.h"
#include "masternode-sync.h"
#include "masternodeman.h"
#include "merkleblock.h"
#include "net.h"
#include "poa.h"
#include "swifttx.h"
#include "txdb.h"
#include "txmempool.h"
#include "guiinterface.h"
#include "util.h"
#include "utilmoneystr.h"
#include "validationinterface.h"

#include <sstream>

#include <boost/algorithm/string/replace.hpp>
#include <boost/foreach.hpp>
#include <boost/thread.hpp>


#if defined(NDEBUG)
#error "PRCY cannot be compiled without assertions."
#endif

/**
 * Global state
 */


/**
 * Mutex to guard access to validation specific variables, such as reading
 * or changing the chainstate.
 *
 * This may also need to be locked when updating the transaction pool, e.g. on
 * AcceptToMemoryPool. See CTxMemPool::cs comment for details.
 *
 * The transaction pool has a separate lock to allow reading from it and the
 * chainstate at the same time.
 */
RecursiveMutex cs_main;

BlockMap mapBlockIndex;
std::map<uint256, uint256> mapProofOfStake;
std::map<unsigned int, unsigned int> mapHashedBlocks;
CChain chainActive;
CBlockIndex* pindexBestHeader = NULL;
int64_t nTimeBestReceived = 0;

// Best block section
Mutex g_best_block_mutex;
std::condition_variable g_best_block_cv;
uint256 g_best_block;

int nScriptCheckThreads = 0;
std::atomic<bool> fImporting{false};
std::atomic<bool> fReindex{false};
bool fTxIndex = true;
bool fIsBareMultisigStd = true;
bool fCheckBlockIndex = false;
bool fVerifyingBlocks = false;
size_t nCoinCacheUsage = 5000 * 300;

/* If the tip is older than this (in seconds), the node is considered to be in initial block download. */
int64_t nMaxTipAge = DEFAULT_MAX_TIP_AGE;

int MIN_RING_SIZE;
int MAX_RING_SIZE;
const int MAX_TX_INPUTS = 50;
const int MIN_TX_INPUTS_FOR_SWEEPING = 25;

/** Fees smaller than this (in duffs) are considered zero fee (for relaying and mining)
 * We are ~100 times smaller then bitcoin now (2015-06-23), set minRelayTxFee only 10 times higher
 * so it's still 10 times lower comparing to bitcoin.
 */
CFeeRate minRelayTxFee = CFeeRate(10000);

CTxMemPool mempool(::minRelayTxFee);

struct COrphanTx {
    CTransaction tx;
    NodeId fromPeer;
};
std::map<uint256, COrphanTx> mapOrphanTransactions;
std::map<uint256, std::set<uint256> > mapOrphanTransactionsByPrev;
std::map<uint256, int64_t> mapRejectedBlocks;

void EraseOrphansFor(NodeId peer);

static void CheckBlockIndex();

/** Constant stuff for coinbase transactions we create: */
CScript COINBASE_FLAGS;

const std::string strMessageMagic = "DarkNet Signed Message:\n";

// Internal stuff
namespace
{
struct CBlockIndexWorkComparator {
    bool operator()(CBlockIndex* pa, CBlockIndex* pb) const
    {
        // First sort by most total work, ...
        if (pa->nChainWork > pb->nChainWork) return false;
        if (pa->nChainWork < pb->nChainWork) return true;

        // ... then by earliest time received, ...
        if (pa->nSequenceId < pb->nSequenceId) return false;
        if (pa->nSequenceId > pb->nSequenceId) return true;

        // Use pointer address as tie breaker (should only happen with blocks
        // loaded from disk, as those all have id 0).
        if (pa < pb) return false;
        if (pa > pb) return true;

        // Identical blocks.
        return false;
    }
};

CBlockIndex* pindexBestInvalid;

/**
     * The set of all CBlockIndex entries with BLOCK_VALID_TRANSACTIONS (for itself and all ancestors) and
     * as good as our current tip or better. Entries may be failed, though.
     */
std::set<CBlockIndex*, CBlockIndexWorkComparator> setBlockIndexCandidates;
/** Number of nodes with fSyncStarted. */
int nSyncStarted = 0;
/** All pairs A->B, where A (or one if its ancestors) misses transactions, but B has transactions. */
std::multimap<CBlockIndex*, CBlockIndex*> mapBlocksUnlinked;

RecursiveMutex cs_LastBlockFile;
std::vector<CBlockFileInfo> vinfoBlockFile;
int nLastBlockFile = 0;

/**
     * Every received block is assigned a unique and increasing identifier, so we
     * know which one to give priority in case of a fork.
     */
RecursiveMutex cs_nBlockSequenceId;
/** Blocks loaded from disk are assigned id 0, so start the counter at 1. */
uint32_t nBlockSequenceId = 1;

/**
     * Sources of received blocks, to be able to send them reject messages or ban
     * them, if processing happens afterwards. Protected by cs_main.
     */
std::map<uint256, NodeId> mapBlockSource;

/**
     * Filter for transactions that were recently rejected by
     * AcceptToMemoryPool. These are not rerequested until the chain tip
     * changes, at which point the entire filter is reset. Protected by
     * cs_main.
     *
     * Without this filter we'd be re-requesting txs from each of our peers,
     * increasing bandwidth consumption considerably. For instance, with 100
     * peers, half of which relay a tx we don't accept, that might be a 50x
     * bandwidth increase. A flooding attacker attempting to roll-over the
     * filter using minimum-sized, 60byte, transactions might manage to send
     * 1000/sec if we have fast peers, so we pick 120,000 to give our peers a
     * two minute window to send invs to us.
     *
     * Decreasing the false positive rate is fairly cheap, so we pick one in a
     * million to make it highly unlikely for users to have issues with this
     * filter.
     *
     * Memory used: 1.7MB
     */
boost::scoped_ptr<CRollingBloomFilter> recentRejects;
uint256 hashRecentRejectsChainTip;

/** Blocks that are in flight, and that are in the queue to be downloaded. Protected by cs_main. */
struct QueuedBlock {
    uint256 hash;
    CBlockIndex* pindex;        //! Optional.
    int64_t nTime;              //! Time of "getdata" request in microseconds.
    int nValidatedQueuedBefore; //! Number of blocks queued with validated headers (globally) at the time this one is requested.
    bool fValidatedHeaders;     //! Whether this block has validated headers at the time of request.
};
std::map<uint256, std::pair<NodeId, std::list<QueuedBlock>::iterator> > mapBlocksInFlight;

/** Number of blocks in flight with validated headers. */
int nQueuedValidatedHeaders = 0;

/** Number of preferable block download peers. */
int nPreferredDownload = 0;

/** Dirty block index entries. */
std::set<CBlockIndex*> setDirtyBlockIndex;

/** Dirty block file entries. */
std::set<int> setDirtyFileInfo;
} // namespace

CAmount GetValueIn(CCoinsViewCache view, const CTransaction& tx)
{
    if (tx.IsCoinBase())
        return 0;

    CAmount nResult = 0;

    if (tx.IsCoinStake()) {
        for (unsigned int i = 0; i < tx.vin.size(); i++) {
            CAmount nValueIn; // = txPrev.vout[prevout.n].nValue;
            uint256 hashBlock;
            CTransaction txPrev;
            GetTransaction(tx.vin[i].prevout.hash, txPrev, hashBlock, true);
            const CTxOut& out = txPrev.vout[tx.vin[i].prevout.n];
            if (out.nValue > 0) {
                nResult += out.nValue;
            } else {
                uint256 val = out.maskValue.amount;
                uint256 mask = out.maskValue.mask;
                CKey decodedMask;
                CPubKey sharedSec;
                sharedSec.Set(tx.vin[i].encryptionKey.begin(), tx.vin[i].encryptionKey.begin() + 33);
                ECDHInfo::Decode(mask.begin(), val.begin(), sharedSec, decodedMask, nValueIn);
                //Verify commitment
                std::vector<unsigned char> commitment;
                CWallet::CreateCommitment(decodedMask.begin(), nValueIn, commitment);
                if (commitment != out.commitment) {
                    throw std::runtime_error("Commitment for coinstake not correct");
                }
                nResult += nValueIn;
            }
        }
    }

    return nResult;
}

//! Return priority of tx at height nHeight
double GetPriority(const CTransaction& tx, int nHeight)
{
    if (tx.IsCoinBase() || tx.IsCoinStake())
        return 0.0;
    double dResult = 0.0;
    /*for (const CTxIn& txin:  tx.vin) {
        std::vector<COutPoint> alldecoys = txin.decoys;
        alldecoys.push_back(txin.prevout);
        for (size_t j = 0; j < alldecoys.size(); j++) {
            CTransaction prev;
            uint256 bh;
            if (!GetTransaction(alldecoys[j].hash, prev, bh, true)) {
                return false;
            }

            if (mapBlockIndex.count(bh) < 1) continue;
            if (mapBlockIndex[bh]->nHeight < nHeight) {
                dResult += 1000 * COIN * (nHeight - mapBlockIndex[bh]->nHeight);
            }
        }
    }*/
    return 1000000000 + tx.ComputePriority(dResult);
}

bool IsSpentKeyImage(const std::string& kiHex, const uint256& againsHash)
{
    if (kiHex.empty()) return false;
    std::vector<uint256> bhs;
    if (!pblocktree->ReadKeyImages(kiHex, bhs)) {
        //not spent yet because not found in database
        return false;
    }
    if (bhs.empty()) {
        return false;
    }
    for (int i = 0; i < bhs.size(); i++) {
        uint256 bh = bhs[i];
        if (againsHash.IsNull()) {
            //check if bh is in main chain
            // Find the block it claims to be in
            BlockMap::iterator mi = mapBlockIndex.find(bh);
            if (mi == mapBlockIndex.end())
                continue;
            CBlockIndex* pindex = (*mi).second;
            if (pindex && chainActive.Contains(pindex))
                return true;
            continue; //receive from mempool
        } else {
            if (bh == againsHash && !againsHash.IsNull()) return false;

            //check whether bh and againsHash is in the same fork
            if (mapBlockIndex.count(bh) < 1) continue;
            CBlockIndex* pindex = mapBlockIndex[againsHash];
            CBlockIndex* bhIndex = mapBlockIndex[bh];
            CBlockIndex* ancestor = pindex->GetAncestor(bhIndex->nHeight);
            if (ancestor == bhIndex) return true;
        }
    }
    return false;
}

bool CheckKeyImageSpendInMainChain(const std::string& kiHex, int& confirmations)
{
    confirmations = 0;
    if (kiHex.empty()) return false;
    std::vector<uint256> bhs;
    if (!pblocktree->ReadKeyImages(kiHex, bhs)) {
        //not spent yet because not found in database
        return false;
    }
    if (bhs.empty()) {
        return false;
    }
    for (int i = 0; i < bhs.size(); i++) {
        uint256 bh = bhs[i];
        //check if bh is in main chain
        // Find the block it claims to be in
        BlockMap::iterator mi = mapBlockIndex.find(bh);
        if (mi == mapBlockIndex.end())
            continue;
        CBlockIndex* pindex = (*mi).second;
        if (pindex && chainActive.Contains(pindex)) {
            confirmations = 1 + chainActive.Height() - pindex->nHeight;
            return true;
        }
    }
    return false;
}

secp256k1_context2* GetContext()
{
    static secp256k1_context2* both;
    if (!both) both = secp256k1_context_create2(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    return both;
}

secp256k1_scratch_space2* GetScratch()
{
    static secp256k1_scratch_space2* scratch;
    if (!scratch) scratch = secp256k1_scratch_space_create(GetContext(), 1024 * 1024 * 512);
    return scratch;
}

secp256k1_bulletproof_generators* GetGenerator()
{
    static secp256k1_bulletproof_generators* generator;
    if (!generator) generator = secp256k1_bulletproof_generators_create_with_pregenerated(GetContext());
    return generator;
}

void DestroyContext()
{
    secp256k1_bulletproof_generators_destroy(GetContext(), GetGenerator());
    secp256k1_scratch_space_destroy(GetScratch());
    secp256k1_context_destroy(GetContext());
}

bool VerifyBulletProofAggregate(const CTransaction& tx)
{
    if (IsInitialBlockDownload()) return true;
    size_t len = tx.bulletproofs.size();
    if (tx.vout.size() >= 5) return false;

    if (len == 0) return false;
    const size_t MAX_VOUT = 5;
    secp256k1_pedersen_commitment commitments[MAX_VOUT];
    size_t i = 0;
    for (i = 0; i < tx.vout.size(); i++) {
        if (!secp256k1_pedersen_commitment_parse(GetContext(), &commitments[i], &(tx.vout[i].commitment[0])))
            throw std::runtime_error("Failed to parse pedersen commitment");
    }
    return secp256k1_bulletproof_rangeproof_verify(GetContext(), GetScratch(), GetGenerator(), &(tx.bulletproofs[0]), len, NULL, commitments, tx.vout.size(), 64, &secp256k1_generator_const_h, NULL, 0);
}

bool VerifyRingSignatureWithTxFee(const CTransaction& tx, CBlockIndex* pindex)
{
    if (tx.nTxFee < 0) return false;
    if (IsInitialBlockDownload()) return true;
    const size_t MAX_VIN = MAX_TX_INPUTS;
    SetRingSize(pindex->nHeight);
    const size_t MAX_DECOYS = MAX_RING_SIZE; //padding 1 for safety reasons
    const size_t MAX_VOUT = 5;

    if (tx.vin.size() > MAX_VIN) {
        LogPrintf("Tx input too many\n");
        return false;
    }
    for (size_t i = 1; i < tx.vin.size(); i++) {
        if (tx.vin[i].decoys.size() != tx.vin[0].decoys.size()) {
            LogPrintf("The number of decoys not equal for all inputs, input %d has %d decoys but input 0 has only %d\n", i, tx.vin[i].decoys.size(), tx.vin[0].decoys.size());
            return false;
        }
    }
    if (tx.vin.size() == 0) {
        LogPrintf("Transaction %s has no inputs\n", tx.GetHash().GetHex());
        return false;
    }

    if (tx.vin[0].decoys.size() > MAX_DECOYS || tx.vin[0].decoys.size() < MIN_RING_SIZE) {
        LogPrintf("The number of decoys RingSize %d not within range [%d, %d]\n", tx.vin[0].decoys.size(), MIN_RING_SIZE, MAX_RING_SIZE);
        return false; //maximum decoys = 15
    }

    unsigned char allInPubKeys[MAX_VIN + 1][MAX_DECOYS + 1][33];
    unsigned char allKeyImages[MAX_VIN + 1][33];
    unsigned char allInCommitments[MAX_VIN][MAX_DECOYS + 1][33];
    unsigned char allOutCommitments[MAX_VOUT][33];

    unsigned char SIJ[MAX_VIN + 1][MAX_DECOYS + 1][32];
    unsigned char LIJ[MAX_VIN + 1][MAX_DECOYS + 1][33];
    unsigned char RIJ[MAX_VIN + 1][MAX_DECOYS + 1][33];

    secp256k1_context2* both = GetContext();

    //generating LIJ and RIJ at PI
    for (size_t j = 0; j < tx.vin.size(); j++) {
        memcpy(allKeyImages[j], tx.vin[j].keyImage.begin(), 33);
    }

    //extract all public keys
    for (size_t i = 0; i < tx.vin.size(); i++) {
        std::vector<COutPoint> decoysForIn;
        decoysForIn.push_back(tx.vin[i].prevout);
        for (size_t j = 0; j < tx.vin[i].decoys.size(); j++) {
            decoysForIn.push_back(tx.vin[i].decoys[j]);
        }
        for (size_t j = 0; j < tx.vin[0].decoys.size() + 1; j++) {
            CTransaction txPrev;
            uint256 hashBlock;
            if (!GetTransaction(decoysForIn[j].hash, txPrev, hashBlock)) {
                LogPrintf("Failed to find transaction %s\n", decoysForIn[j].hash.GetHex());
                return false;
            }
            CBlockIndex* tip = chainActive.Tip();
            if (!pindex) tip = pindex;

            uint256 hashTip = tip->GetBlockHash();
            //verify that tip and hashBlock must be in the same fork
            CBlockIndex* atTheblock = mapBlockIndex[hashBlock];
            if (!atTheblock) {
                LogPrintf("%s: Decoy for transaction %s not in the same chain as block height=%s hash=%s\n", __func__, decoysForIn[j].hash.GetHex(), tip->nHeight, tip->GetBlockHash().GetHex());
                return false;
            } else {
                CBlockIndex* ancestor = tip->GetAncestor(atTheblock->nHeight);
                if (ancestor != atTheblock) {
                    LogPrintf("%s: Decoy for transaction %s not in the same chain as block height=%s hash=%s\n", __func__, decoysForIn[j].hash.GetHex(), tip->nHeight, tip->GetBlockHash().GetHex());
                    return false;
                }
            }

            CPubKey extractedPub;
            if (!ExtractPubKey(txPrev.vout[decoysForIn[j].n].scriptPubKey, extractedPub)) {
                LogPrintf("Failed to extract pubkey\n");
                return false;
            }
            memcpy(allInPubKeys[i][j], extractedPub.begin(), 33);
            memcpy(allInCommitments[i][j], &(txPrev.vout[decoysForIn[j].n].commitment[0]), 33);
        }
    }
    memcpy(allKeyImages[tx.vin.size()], tx.ntxFeeKeyImage.begin(), 33);

    for (size_t i = 0; i < tx.vin[0].decoys.size() + 1; i++) {
        std::vector<uint256> S_column = tx.S[i];
        for (size_t j = 0; j < tx.vin.size() + 1; j++) {
            memcpy(SIJ[j][i], S_column[j].begin(), 32);
        }
    }

    //compute allInPubKeys[tx.vin.size()][..]
    secp256k1_pedersen_commitment allInCommitmentsPacked[MAX_VIN][MAX_DECOYS + 1];
    secp256k1_pedersen_commitment allOutCommitmentsPacked[MAX_VOUT + 1]; //+1 for tx fee

    for (size_t i = 0; i < tx.vout.size(); i++) {
        if (tx.vout[i].commitment.empty()) {
            LogPrintf("Commitment can not be null\n");
            return false;
        }
        memcpy(allOutCommitments[i], &(tx.vout[i].commitment[0]), 33);
        if (!secp256k1_pedersen_commitment_parse(both, &allOutCommitmentsPacked[i], allOutCommitments[i])) {
            LogPrintf("Failed to parse commitment\n");
            return false;
        }
    }

    //commitment to tx fee, blind = 0
    unsigned char txFeeBlind[32];
    memset(txFeeBlind, 0, 32);
    if (!secp256k1_pedersen_commit(both, &allOutCommitmentsPacked[tx.vout.size()], txFeeBlind, tx.nTxFee, &secp256k1_generator_const_h, &secp256k1_generator_const_g))
        throw std::runtime_error("Failed to computed commitment");

    //filling the additional pubkey elements for decoys: allInPubKeys[wtxNew.vin.size()][..]
    //allInPubKeys[wtxNew.vin.size()][j] = sum of allInPubKeys[..][j] + sum of allInCommitments[..][j] + sum of allOutCommitments
    const secp256k1_pedersen_commitment* outCptr[MAX_VOUT + 1];
    for (size_t i = 0; i < tx.vout.size() + 1; i++) {
        outCptr[i] = &allOutCommitmentsPacked[i];
    }

    secp256k1_pedersen_commitment inPubKeysToCommitments[MAX_VIN][MAX_DECOYS + 1];
    for (size_t i = 0; i < tx.vin.size(); i++) {
        for (size_t j = 0; j < tx.vin[0].decoys.size() + 1; j++) {
            secp256k1_pedersen_serialized_pubkey_to_commitment(allInPubKeys[i][j], 33, &inPubKeysToCommitments[i][j]);
        }
    }

    for (size_t j = 0; j < tx.vin[0].decoys.size() + 1; j++) {
        const secp256k1_pedersen_commitment* inCptr[MAX_VIN * 2];
        for (size_t k = 0; k < tx.vin.size(); k++) {
            if (!secp256k1_pedersen_commitment_parse(both, &allInCommitmentsPacked[k][j], allInCommitments[k][j])) {
                LogPrintf("Failed to parse commitment\n");
                return false;
            }
            inCptr[k] = &allInCommitmentsPacked[k][j];
        }

        for (size_t k = tx.vin.size(); k < 2 * tx.vin.size(); k++) {
            inCptr[k] = &inPubKeysToCommitments[k - tx.vin.size()][j];
        }
        secp256k1_pedersen_commitment out;
        size_t length;
        if (!secp256k1_pedersen_commitment_sum(both, inCptr, tx.vin.size() * 2, outCptr, tx.vout.size() + 1, &out)) {
            LogPrintf("Failed to secp256k1_pedersen_commitment_sum\n");
            return false;
        }
        if (!secp256k1_pedersen_commitment_to_serialized_pubkey(&out, allInPubKeys[tx.vin.size()][j], &length)) {
            LogPrintf("Failed to serialized pubkey\n");
            return false;
        }
    }


    //verification
    unsigned char C[32];
    memcpy(C, tx.c.begin(), 32);
    for (size_t j = 0; j < tx.vin[0].decoys.size() + 1; j++) {
        for (size_t i = 0; i < tx.vin.size() + 1; i++) {
            //compute LIJ, RIJ
            unsigned char P[33];
            memcpy(P, allInPubKeys[i][j], 33);
            if (!secp256k1_ec_pubkey_tweak_mul(P, 33, C)) {
                LogPrintf("Failed to mul pubkey\n");
                return false;
            }

            if (!secp256k1_ec_pubkey_tweak_add(P, 33, SIJ[i][j])) {
                LogPrintf("Failed to add pubkey\n");
                return false;
            }

            memcpy(LIJ[i][j], P, 33);

            //compute RIJ
            unsigned char sh[33];
            CPubKey pkij;
            pkij.Set(allInPubKeys[i][j], allInPubKeys[i][j] + 33);
            PointHashingSuccessively(pkij, SIJ[i][j], sh);

            unsigned char ci[33];
            memcpy(ci, allKeyImages[i], 33);
            if (!secp256k1_ec_pubkey_tweak_mul(ci, 33, C)) {
                LogPrintf("Failed to mul tweak\n");
                return false;
            }

            //convert shp into commitment
            secp256k1_pedersen_commitment SHP_commitment;
            secp256k1_pedersen_serialized_pubkey_to_commitment(sh, 33, &SHP_commitment);

            //convert CI*I into commitment
            secp256k1_pedersen_commitment cii_commitment;
            secp256k1_pedersen_serialized_pubkey_to_commitment(ci, 33, &cii_commitment);

            const secp256k1_pedersen_commitment* twoElements[2];
            twoElements[0] = &SHP_commitment;
            twoElements[1] = &cii_commitment;

            secp256k1_pedersen_commitment sum;
            if (!secp256k1_pedersen_commitment_sum_pos(both, twoElements, 2, &sum))
                throw std::runtime_error("failed to compute secp256k1_pedersen_commitment_sum_pos");
            size_t tempLength;
            if (!secp256k1_pedersen_commitment_to_serialized_pubkey(&sum, RIJ[i][j], &tempLength))
                throw std::runtime_error("failed to serialize pedersen commitment");
        }

        //compute C
        unsigned char tempForHash[2 * (MAX_VIN + 1) * 33 + 32];
        unsigned char* tempForHashPtr = tempForHash;
        for (size_t i = 0; i < tx.vin.size() + 1; i++) {
            memcpy(tempForHashPtr, &(LIJ[i][j][0]), 33);
            tempForHashPtr += 33;
            memcpy(tempForHashPtr, &(RIJ[i][j][0]), 33);
            tempForHashPtr += 33;
        }
        uint256 ctsHash = GetTxSignatureHash(tx);
        memcpy(tempForHashPtr, ctsHash.begin(), 32);

        uint256 temppi1 = Hash(tempForHash, tempForHash + 2 * (tx.vin.size() + 1) * 33 + 32);
        memcpy(C, temppi1.begin(), 32);
    }
    //LogPrintf("Verifying\n");
    return HexStr(tx.c.begin(), tx.c.end()) == HexStr(C, C + 32);
}

bool ReVerifyPoSBlock(CBlockIndex* pindex)
{
    LOCK(cs_main);
    {
        if (!pindex) return false;
        CBlock block;
        if (!ReadBlockFromDisk(block, pindex)) return false;
        if (!pindex->IsProofOfStake()) return false;
        CAmount nFees = 0;
        CAmount nValueIn = 0;
        CAmount nValueOut = 0;
        for (unsigned int i = 0; i < block.vtx.size(); i++) {
            const CTransaction& tx = block.vtx[i];
            if (!tx.IsCoinStake()) {
                if (!tx.IsCoinAudit()) {
                    if (!VerifyRingSignatureWithTxFee(tx, pindex))
                        return false;
                    if (!VerifyBulletProofAggregate(tx))
                        return false;
                }
                nFees += tx.nTxFee;
            }
        }

        const CTransaction coinstake = block.vtx[1];
        CCoinsViewCache view(pcoinsTip);
        nValueIn = GetValueIn(view, coinstake);
        nValueOut = coinstake.GetValueOut();

        size_t numUTXO = coinstake.vout.size();
        if (mapBlockIndex.count(block.hashPrevBlock) < 1) {
            LogPrintf("%s: Previous block not found, received block %s, previous %s, current tip %s\n", __func__, block.GetHash().GetHex(), block.hashPrevBlock.GetHex(), chainActive.Tip()->GetBlockHash().GetHex());
            return false;
        }
        CAmount blockValue = GetBlockValue(mapBlockIndex[block.hashPrevBlock]->nHeight);
        const CTxOut& mnOut = coinstake.vout[numUTXO - 1];
        std::string mnsa(mnOut.masternodeStealthAddress.begin(), mnOut.masternodeStealthAddress.end());
        if (!VerifyDerivedAddress(mnOut, mnsa)) {
            LogPrintf("%s: Incorrect derived address for masternode rewards\n", __func__);
            return false;
        }

        // track money supply and mint amount info
        CAmount nMoneySupplyPrev = pindex->pprev ? pindex->pprev->nMoneySupply : 0;
        pindex->nMoneySupply = nMoneySupplyPrev + nValueOut - nValueIn - nFees;
        LogPrint(BCLog::SUPPLY, "%s: nMoneySupplyPrev=%d, pindex->nMoneySupply=%d, nFees = %d\n", __func__, nMoneySupplyPrev, pindex->nMoneySupply, nFees);
        pindex->nMint = pindex->nMoneySupply - nMoneySupplyPrev + nFees;

        //PoW phase redistributed fees to miner. PoS stage destroys fees.
        CAmount nExpectedMint = GetBlockValue(pindex->pprev->nHeight);
        nExpectedMint += nFees;

        if (!IsBlockValueValid(pindex->nHeight, nExpectedMint, pindex->nMint)) {
            LogPrintf("%s: reward pays too much (actual=%s vs limit=%s)\n", __func__, FormatMoney(pindex->nMint), FormatMoney(nExpectedMint));
            return false;
        }
        return true;
    }
}

uint256 GetTxSignatureHash(const CTransaction& tx)
{
    CTransactionSignature cts(tx);
    return cts.GetHash();
}

uint256 GetTxInSignatureHash(const CTxIn& txin)
{
    CTxInShortDigest cts(txin);
    return cts.GetHash();
}

//////////////////////////////////////////////////////////////////////////////
//
// Registration of network node signals.
//

namespace
{
struct CBlockReject {
    unsigned char chRejectCode;
    std::string strRejectReason;
    uint256 hashBlock;
};


class CNodeBlocks
{
public:
    CNodeBlocks():
            maxSize(0),
            maxAvg(0)
    {
        maxSize = GetArg("-blockspamfiltermaxsize", DEFAULT_BLOCK_SPAM_FILTER_MAX_SIZE);
        maxAvg = GetArg("-blockspamfiltermaxavg", DEFAULT_BLOCK_SPAM_FILTER_MAX_AVG);
    }

    bool onBlockReceived(int nHeight) {
        if(nHeight > 0 && maxSize && maxAvg) {
            addPoint(nHeight);
            return true;
        }
        return false;
    }

    bool updateState(CValidationState& state, bool ret)
    {
        // No Blocks
        size_t size = points.size();
        if(size == 0)
            return ret;

        // Compute the number of the received blocks
        size_t nBlocks = 0;
        for(auto point : points)
        {
            nBlocks += point.second;
        }

        // Compute the average value per height
        double nAvgValue = (double)nBlocks / size;

        // Ban the node if try to spam
        bool banNode = (nAvgValue >= 1.5 * maxAvg && size >= maxAvg) ||
                       (nAvgValue >= maxAvg && nBlocks >= maxSize) ||
                       (nBlocks >= maxSize * 3);
        if(banNode)
        {
            // Clear the points and ban the node
            points.clear();
            return state.DoS(100, error("block-spam ban node for sending spam"));
        }

        return ret;
    }

private:
    void addPoint(int height)
    {
        // Remove the last element in the list
        if(points.size() == maxSize)
        {
            points.erase(points.begin());
        }

        // Add the point to the list
        int occurrence = 0;
        auto mi = points.find(height);
        if (mi != points.end())
            occurrence = (*mi).second;
        occurrence++;
        points[height] = occurrence;
    }

private:
    std::map<int,int> points;
    size_t maxSize;
    size_t maxAvg;
};



 /**
 * Maintain validation-specific state about nodes, protected by cs_main, instead
 * by CNode's own locks. This simplifies asynchronous operation, where
 * processing of incoming data is done after the ProcessMessage call returns,
 * and we're no longer holding the node's locks.
 */
struct CNodeState {
    //! The peer's address
    CService address;
    //! Whether we have a fully established connection.
    bool fCurrentlyConnected;
    //! Accumulated misbehaviour score for this peer.
    int nMisbehavior;
    //! Whether this peer should be disconnected and banned (unless whitelisted).
    bool fShouldBan;
    //! String name of this peer (debugging/logging purposes).
    std::string name;
    //! List of asynchronously-determined block rejections to notify this peer about.
    std::vector<CBlockReject> rejects;
    //! The best known block we know this peer has announced.
    CBlockIndex* pindexBestKnownBlock;
    //! The hash of the last unknown block this peer has announced.
    uint256 hashLastUnknownBlock;
    //! The last full block we both have.
    CBlockIndex* pindexLastCommonBlock;
    //! Whether we've started headers synchronization with this peer.
    bool fSyncStarted;
    //! Since when we're stalling block download progress (in microseconds), or 0.
    int64_t nStallingSince;
    std::list<QueuedBlock> vBlocksInFlight;
    int nBlocksInFlight;
    //! Whether we consider this a preferred download peer.
    bool fPreferredDownload;

    CNodeBlocks nodeBlocks;

    CNodeState()
    {
        fCurrentlyConnected = false;
        nMisbehavior = 0;
        fShouldBan = false;
        pindexBestKnownBlock = NULL;
        hashLastUnknownBlock.SetNull();
        pindexLastCommonBlock = NULL;
        fSyncStarted = false;
        nStallingSince = 0;
        nBlocksInFlight = 0;
        fPreferredDownload = false;
    }
};

/** Map maintaining per-node state. Requires cs_main. */
std::map<NodeId, CNodeState> mapNodeState;

// Requires cs_main.
CNodeState* State(NodeId pnode)
{
    std::map<NodeId, CNodeState>::iterator it = mapNodeState.find(pnode);
    if (it == mapNodeState.end())
        return NULL;
    return &it->second;
}

int GetHeight()
{
    while (true) {
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain) {
            MilliSleep(50);
            continue;
        }
        return chainActive.Height();
    }
}

void UpdatePreferredDownload(CNode* node, CNodeState* state)
{
    nPreferredDownload -= state->fPreferredDownload;

    // Whether this node should be marked as a preferred download node.
    state->fPreferredDownload = (!node->fInbound || node->fWhitelisted) && !node->fOneShot && !node->fClient;

    nPreferredDownload += state->fPreferredDownload;
}

void InitializeNode(NodeId nodeid, const CNode* pnode)
{
    LOCK(cs_main);
    CNodeState& state = mapNodeState.insert(std::make_pair(nodeid, CNodeState())).first->second;
    state.name = pnode->addrName;
    state.address = pnode->addr;
}

void FinalizeNode(NodeId nodeid)
{
    LOCK(cs_main);
    CNodeState* state = State(nodeid);

    if (!state)
        return;

    if (state->fSyncStarted)
        nSyncStarted--;

    if (state->nMisbehavior == 0 && state->fCurrentlyConnected) {
        AddressCurrentlyConnected(state->address);
    }

    for (const QueuedBlock& entry : state->vBlocksInFlight)
        mapBlocksInFlight.erase(entry.hash);
    EraseOrphansFor(nodeid);
    nPreferredDownload -= state->fPreferredDownload;

    mapNodeState.erase(nodeid);
}

// Requires cs_main.
void MarkBlockAsReceived(const uint256& hash)
{
    std::map<uint256, std::pair<NodeId, std::list<QueuedBlock>::iterator> >::iterator itInFlight = mapBlocksInFlight.find(hash);
    if (itInFlight != mapBlocksInFlight.end()) {
        CNodeState* state = State(itInFlight->second.first);
        nQueuedValidatedHeaders -= itInFlight->second.second->fValidatedHeaders;
        state->vBlocksInFlight.erase(itInFlight->second.second);
        state->nBlocksInFlight--;
        state->nStallingSince = 0;
        mapBlocksInFlight.erase(itInFlight);
    }
}

// Requires cs_main.
void MarkBlockAsInFlight(NodeId nodeid, const uint256& hash, CBlockIndex* pindex = NULL)
{
    CNodeState* state = State(nodeid);
    assert(state != NULL);

    // Make sure it's not listed somewhere already.
    MarkBlockAsReceived(hash);

    QueuedBlock newentry = {hash, pindex, GetTimeMicros(), nQueuedValidatedHeaders, pindex != NULL};
    nQueuedValidatedHeaders += newentry.fValidatedHeaders;
    std::list<QueuedBlock>::iterator it = state->vBlocksInFlight.insert(state->vBlocksInFlight.end(), newentry);
    state->nBlocksInFlight++;
    mapBlocksInFlight[hash] = std::make_pair(nodeid, it);
}

/** Check whether the last unknown block a peer advertiszed is not yet known. */
void ProcessBlockAvailability(NodeId nodeid)
{
    CNodeState* state = State(nodeid);
    assert(state != NULL);

    if (!state->hashLastUnknownBlock.IsNull()) {
        BlockMap::iterator itOld = mapBlockIndex.find(state->hashLastUnknownBlock);
        if (itOld != mapBlockIndex.end() && itOld->second == NULL) {
            LogPrint(BCLog::NET, "erasing block %s", itOld->first.GetHex());
            mapBlockIndex.erase(itOld);
        }
        itOld = mapBlockIndex.find(state->hashLastUnknownBlock);
        if (itOld != mapBlockIndex.end() && itOld->second->nChainWork > 0) {
            if (state->pindexBestKnownBlock == NULL ||
                itOld->second->nChainWork >= state->pindexBestKnownBlock->nChainWork)
                state->pindexBestKnownBlock = itOld->second;
            state->hashLastUnknownBlock.SetNull();
        }
    }
}

/** Update tracking information about which blocks a peer is assumed to have. */
void UpdateBlockAvailability(NodeId nodeid, const uint256& hash)
{
    CNodeState* state = State(nodeid);
    assert(state != NULL);
    ProcessBlockAvailability(nodeid);
    BlockMap::iterator it = mapBlockIndex.find(hash);
    if (it != mapBlockIndex.end() && it->second == NULL) {
        mapBlockIndex.erase(it);
    }
    it = mapBlockIndex.find(hash);
    if (it != mapBlockIndex.end() && it->second->nChainWork > 0) {
        // An actually better block was announced.
        if (state->pindexBestKnownBlock == NULL ||
            it->second->nChainWork >= state->pindexBestKnownBlock->nChainWork)
            state->pindexBestKnownBlock = it->second;
    } else {
        // An unknown block was announced; just assume that the latest one is the best one.
        state->hashLastUnknownBlock = hash;
    }
}

/** Find the last common ancestor two blocks have.
 *  Both pa and pb must be non-NULL. */
CBlockIndex* LastCommonAncestor(CBlockIndex* pa, CBlockIndex* pb)
{
    if (pa->nHeight > pb->nHeight) {
        pa = pa->GetAncestor(pb->nHeight);
    } else if (pb->nHeight > pa->nHeight) {
        pb = pb->GetAncestor(pa->nHeight);
    }

    while (pa != pb && pa && pb) {
        pa = pa->pprev;
        pb = pb->pprev;
    }

    // Eventually all chain branches meet at the genesis block.
    assert(pa == pb);
    return pa;
}

/** Update pindexLastCommonBlock and add not-in-flight missing successors to vBlocks, until it has
 *  at most count entries. */
void FindNextBlocksToDownload(NodeId nodeid, unsigned int count, std::vector<CBlockIndex*>& vBlocks, NodeId& nodeStaller)
{
    if (count == 0)
        return;

    vBlocks.reserve(vBlocks.size() + count);
    CNodeState* state = State(nodeid);
    assert(state != NULL);

    // Make sure pindexBestKnownBlock is up to date, we'll need it.
    ProcessBlockAvailability(nodeid);

    if (state->pindexBestKnownBlock == NULL ||
        state->pindexBestKnownBlock->nChainWork < chainActive.Tip()->nChainWork) {
        // This peer has nothing interesting.
        return;
    }

    if (state->pindexLastCommonBlock == NULL) {
        // Bootstrap quickly by guessing a parent of our best tip is the forking point.
        // Guessing wrong in either direction is not a problem.
        state->pindexLastCommonBlock = chainActive[std::min(state->pindexBestKnownBlock->nHeight,
            chainActive.Height())];
    }

    // If the peer reorganized, our previous pindexLastCommonBlock may not be an ancestor
    // of their current tip anymore. Go back enough to fix that.
    state->pindexLastCommonBlock = LastCommonAncestor(state->pindexLastCommonBlock, state->pindexBestKnownBlock);
    if (state->pindexLastCommonBlock == state->pindexBestKnownBlock)
        return;

    std::vector<CBlockIndex*> vToFetch;
    CBlockIndex* pindexWalk = state->pindexLastCommonBlock;
    // Never fetch further than the best block we know the peer has, or more than BLOCK_DOWNLOAD_WINDOW + 1 beyond the last
    // linked block we have in common with this peer. The +1 is so we can detect stalling, namely if we would be able to
    // download that next block if the window were 1 larger.
    int nWindowEnd = state->pindexLastCommonBlock->nHeight + BLOCK_DOWNLOAD_WINDOW;
    int nMaxHeight = std::min<int>(state->pindexBestKnownBlock->nHeight, nWindowEnd + 1);
    NodeId waitingfor = -1;
    while (pindexWalk->nHeight < nMaxHeight) {
        // Read up to 128 (or more, if more blocks than that are needed) successors of pindexWalk (towards
        // pindexBestKnownBlock) into vToFetch. We fetch 128, because CBlockIndex::GetAncestor may be as expensive
        // as iterating over ~100 CBlockIndex* entries anyway.
        int nToFetch = std::min(nMaxHeight - pindexWalk->nHeight, std::max<int>(count - vBlocks.size(), 128));
        vToFetch.resize(nToFetch);
        pindexWalk = state->pindexBestKnownBlock->GetAncestor(pindexWalk->nHeight + nToFetch);
        vToFetch[nToFetch - 1] = pindexWalk;
        for (unsigned int i = nToFetch - 1; i > 0; i--) {
            vToFetch[i - 1] = vToFetch[i]->pprev;
        }

        // Iterate over those blocks in vToFetch (in forward direction), adding the ones that
        // are not yet downloaded and not in flight to vBlocks. In the mean time, update
        // pindexLastCommonBlock as long as all ancestors are already downloaded.
        for (CBlockIndex* pindex : vToFetch) {
            if (!pindex->IsValid(BLOCK_VALID_TREE)) {
                // We consider the chain that this peer is on invalid.
                return;
            }
            if (pindex->nStatus & BLOCK_HAVE_DATA) {
                if (pindex->nChainTx)
                    state->pindexLastCommonBlock = pindex;
            } else if (mapBlocksInFlight.count(pindex->GetBlockHash()) == 0) {
                // The block is not already downloaded, and not yet in flight.
                if (pindex->nHeight > nWindowEnd) {
                    // We reached the end of the window.
                    if (vBlocks.size() == 0 && waitingfor != nodeid) {
                        // We aren't able to fetch anything, but we would be if the download window was one larger.
                        nodeStaller = waitingfor;
                    }
                    return;
                }
                vBlocks.push_back(pindex);
                if (vBlocks.size() == count) {
                    return;
                }
            } else if (waitingfor == -1) {
                // This is the first already-in-flight block.
                waitingfor = mapBlocksInFlight[pindex->GetBlockHash()].first;
            }
        }
    }
}

} // namespace

bool GetNodeStateStats(NodeId nodeid, CNodeStateStats& stats)
{
    LOCK(cs_main);
    CNodeState* state = State(nodeid);
    if (state == NULL)
        return false;
    stats.nMisbehavior = state->nMisbehavior;
    stats.nSyncHeight = state->pindexBestKnownBlock ? state->pindexBestKnownBlock->nHeight : -1;
    stats.nCommonHeight = state->pindexLastCommonBlock ? state->pindexLastCommonBlock->nHeight : -1;
    for (const QueuedBlock& queue : state->vBlocksInFlight) {
        if (queue.pindex)
            stats.vHeightInFlight.push_back(queue.pindex->nHeight);
    }
    return true;
}

void RegisterNodeSignals(CNodeSignals& nodeSignals)
{
    nodeSignals.GetHeight.connect(&GetHeight);
    nodeSignals.ProcessMessages.connect(&ProcessMessages);
    nodeSignals.SendMessages.connect(&SendMessages);
    nodeSignals.InitializeNode.connect(&InitializeNode);
    nodeSignals.FinalizeNode.connect(&FinalizeNode);
}

void UnregisterNodeSignals(CNodeSignals& nodeSignals)
{
    nodeSignals.GetHeight.disconnect(&GetHeight);
    nodeSignals.ProcessMessages.disconnect(&ProcessMessages);
    nodeSignals.SendMessages.disconnect(&SendMessages);
    nodeSignals.InitializeNode.disconnect(&InitializeNode);
    nodeSignals.FinalizeNode.disconnect(&FinalizeNode);
}

CBlockIndex* FindForkInGlobalIndex(const CChain& chain, const CBlockLocator& locator)
{
    // Find the first block the caller has in the main chain
    for (const uint256& hash : locator.vHave) {
        BlockMap::iterator mi = mapBlockIndex.find(hash);
        if (mi != mapBlockIndex.end()) {
            CBlockIndex* pindex = (*mi).second;
            if (pindex && chain.Contains(pindex)) {
                return pindex;
            }
        }
    }
    return chain.Genesis();
}

CCoinsViewCache* pcoinsTip = NULL;
CBlockTreeDB* pblocktree = NULL;

//////////////////////////////////////////////////////////////////////////////
//
// mapOrphanTransactions
//

bool AddOrphanTx(const CTransaction& tx, NodeId peer)
{
    uint256 hash = tx.GetHash();
    if (mapOrphanTransactions.count(hash))
        return false;

    // Ignore big transactions, to avoid a
    // send-big-orphans memory exhaustion attack. If a peer has a legitimate
    // large transaction with a missing parent then we assume
    // it will rebroadcast it later, after the parent transaction(s)
    // have been mined or received.
    // 10,000 orphans, each of which is at most 5,000 bytes big is
    // at most 500 megabytes of orphans:
    unsigned int sz = tx.GetSerializeSize(SER_NETWORK, CTransaction::CURRENT_VERSION);
    if (sz > 5000) {
        LogPrint(BCLog::MEMPOOL, "ignoring large orphan tx (size: %u, hash: %s)\n", sz, hash.ToString());
        return false;
    }

    mapOrphanTransactions[hash].tx = tx;
    mapOrphanTransactions[hash].fromPeer = peer;
    for (const CTxIn& txin : tx.vin)
        mapOrphanTransactionsByPrev[txin.prevout.hash].insert(hash);

    LogPrint(BCLog::MEMPOOL, "stored orphan tx %s (mapsz %u prevsz %u)\n", hash.ToString(),
        mapOrphanTransactions.size(), mapOrphanTransactionsByPrev.size());
    return true;
}

void static EraseOrphanTx(uint256 hash)
{
    std::map<uint256, COrphanTx>::iterator it = mapOrphanTransactions.find(hash);
    if (it == mapOrphanTransactions.end())
        return;
    for (const CTxIn& txin : it->second.tx.vin) {
        std::map<uint256, std::set<uint256> >::iterator itPrev = mapOrphanTransactionsByPrev.find(txin.prevout.hash);
        if (itPrev == mapOrphanTransactionsByPrev.end())
            continue;
        itPrev->second.erase(hash);
        if (itPrev->second.empty())
            mapOrphanTransactionsByPrev.erase(itPrev);
    }
    mapOrphanTransactions.erase(it);
}

void EraseOrphansFor(NodeId peer)
{
    int nErased = 0;
    std::map<uint256, COrphanTx>::iterator iter = mapOrphanTransactions.begin();
    while (iter != mapOrphanTransactions.end()) {
        std::map<uint256, COrphanTx>::iterator maybeErase = iter++; // increment to avoid iterator becoming invalid
        if (maybeErase->second.fromPeer == peer) {
            EraseOrphanTx(maybeErase->second.tx.GetHash());
            ++nErased;
        }
    }
    if (nErased > 0) LogPrint(BCLog::MEMPOOL, "Erased %d orphan tx from peer %d\n", nErased, peer);
}


unsigned int LimitOrphanTxSize(unsigned int nMaxOrphans)
{
    unsigned int nEvicted = 0;
    while (mapOrphanTransactions.size() > nMaxOrphans) {
        // Evict a random orphan:
        uint256 randomhash = GetRandHash();
        std::map<uint256, COrphanTx>::iterator it = mapOrphanTransactions.lower_bound(randomhash);
        if (it == mapOrphanTransactions.end())
            it = mapOrphanTransactions.begin();
        EraseOrphanTx(it->first);
        ++nEvicted;
    }
    return nEvicted;
}

bool IsStandardTx(const CTransaction& tx, std::string& reason)
{
    AssertLockHeld(cs_main);
    if (tx.nVersion > CTransaction::CURRENT_VERSION || tx.nVersion < 1) {
        reason = "version";
        return false;
    }

    // Treat non-final transactions as non-standard to prevent a specific type
    // of double-spend attack, as well as DoS attacks. (if the transaction
    // can't be mined, the attacker isn't expending resources broadcasting it)
    // Basically we don't want to propagate transactions that can't be included in
    // the next block.
    //
    // However, IsFinalTx() is confusing... Without arguments, it uses
    // chainActive.Height() to evaluate nLockTime; when a block is accepted, chainActive.Height()
    // is set to the value of nHeight in the block. However, when IsFinalTx()
    // is called within CBlock::AcceptBlock(), the height of the block *being*
    // evaluated is what is used. Thus if we want to know if a transaction can
    // be part of the *next* block, we need to call IsFinalTx() with one more
    // than chainActive.Height().
    //
    // Timestamps on the other hand don't get any special treatment, because we
    // can't know what timestamp the next block will have, and there aren't
    // timestamp applications where it matters.
    if (!IsFinalTx(tx, chainActive.Height() + 1)) {
        reason = "non-final";
        return false;
    }

    // Extremely large transactions with lots of inputs can cost the network
    // almost as much to process as they cost the sender in fees, because
    // computing signature hashes is O(ninputs*txsize). Limiting transactions
    // to MAX_STANDARD_TX_SIZE mitigates CPU exhaustion attacks.
    unsigned int sz = tx.GetSerializeSize(SER_NETWORK, CTransaction::CURRENT_VERSION);
    unsigned int nMaxSize = MAX_STANDARD_TX_SIZE;
    if (sz >= nMaxSize) {
        reason = "tx-size";
        return false;
    }

    for (const CTxIn& txin : tx.vin) {
        // Biggest 'standard' txin is a 15-of-15 P2SH multisig with compressed
        // keys. (remember the 520 byte limit on redeemScript size) That works
        // out to a (15*(33+1))+3=513 byte redeemScript, 513+1+15*(73+1)+3=1627
        // bytes of scriptSig, which we round off to 1650 bytes for some minor
        // future-proofing. That's also enough to spend a 20-of-20
        // CHECKMULTISIG scriptPubKey, though such a scriptPubKey is not
        // considered standard)
        if (txin.scriptSig.size() > 1650) {
            reason = "scriptsig-size";
            return false;
        }
        if (!txin.scriptSig.IsPushOnly()) {
            reason = "scriptsig-not-pushonly";
            return false;
        }
    }

    unsigned int nDataOut = 0;
    txnouttype whichType;
    for (const CTxOut& txout : tx.vout) {
        if (!::IsStandard(txout.scriptPubKey, whichType)) {
            reason = "scriptpubkey";
            return false;
        }

        if (whichType == TX_NULL_DATA)
            nDataOut++;
        else if ((whichType == TX_MULTISIG) && (!fIsBareMultisigStd)) {
            reason = "bare-multisig";
            return false;
        } else if (txout.nValue != 0 && txout.IsDust(::minRelayTxFee)) {
            reason = "dust";
            return false;
        }
    }

    // only one OP_RETURN txout is permitted
    if (nDataOut > 1) {
        reason = "multi-op-return";
        return false;
    }

    return true;
}

/**
 * Check transaction inputs to mitigate two
 * potential denial-of-service attacks:
 *
 * 1. scriptSigs with extra data stuffed into them,
 *    not consumed by scriptPubKey (or P2SH script)
 * 2. P2SH scripts with a crazy number of expensive
 *    CHECKSIG/CHECKMULTISIG operations
 */
bool AreInputsStandard(const CTransaction& tx, const CCoinsViewCache& mapInputs)
{
    if (tx.IsCoinBase())
        return true; // coinbase has no inputs

    for (unsigned int i = 0; i < tx.vin.size(); i++) {
        CTransaction txPrev;
        uint256 hashBlockPrev;
        if (!GetTransaction(tx.vin[i].prevout.hash, txPrev, hashBlockPrev)) {
            continue; // previous transaction not in main chain
        }

        const CTxOut& prev = txPrev.vout[tx.vin[i].prevout.n];

        std::vector<std::vector<unsigned char> > vSolutions;
        txnouttype whichType;
        // get the scriptPubKey corresponding to this input:
        const CScript& prevScript = prev.scriptPubKey;
        if (!Solver(prevScript, whichType, vSolutions))
            return false;
        int nArgsExpected = ScriptSigArgsExpected(whichType, vSolutions);
        if (nArgsExpected < 0)
            return false;

        if (tx.vin[i].decoys.size() > 0) {
            continue;
        }

        // Transactions with extra stuff in their scriptSigs are
        // non-standard. Note that this EvalScript() call will
        // be quick, because if there are any operations
        // beside "push data" in the scriptSig
        // IsStandard() will have already returned false
        // and this method isn't called.
        std::vector<std::vector<unsigned char> > stack;
        if (!EvalScript(stack, tx.vin[i].scriptSig, false, BaseSignatureChecker()))
            return false;

        if (whichType == TX_SCRIPTHASH) {
            if (stack.empty())
                return false;
            CScript subscript(stack.back().begin(), stack.back().end());
            std::vector<std::vector<unsigned char> > vSolutions2;
            txnouttype whichType2;
            if (Solver(subscript, whichType2, vSolutions2)) {
                int tmpExpected = ScriptSigArgsExpected(whichType2, vSolutions2);
                if (tmpExpected < 0)
                    return false;
                nArgsExpected += tmpExpected;
            } else {
                // Any other Script with less than 15 sigops OK:
                unsigned int sigops = subscript.GetSigOpCount(true);
                // ... extra data left on the stack after execution is OK, too:
                return (sigops <= MAX_P2SH_SIGOPS);
            }
        }

        if (stack.size() != (unsigned int)nArgsExpected)
            return false;
    }

    return true;
}

int GetInputAge(CTxIn& vin)
{
    CCoinsView viewDummy;
    CCoinsViewCache view(&viewDummy);
    {
        LOCK(mempool.cs);
        CCoinsViewMemPool viewMempool(pcoinsTip, mempool);
        view.SetBackend(viewMempool); // temporarily switch cache backend to db+mempool view

        const CCoins* coins = view.AccessCoins(vin.prevout.hash);

        if (coins) {
            if (coins->nHeight < 0) return 0;
            return WITH_LOCK(cs_main, return chainActive.Height() + 1) - coins->nHeight;
        } else
            return -1;
    }
}

int GetIXConfirmations(uint256 nTXHash)
{
    int sigs = 0;

    std::map<uint256, CTransactionLock>::iterator i = mapTxLocks.find(nTXHash);
    if (i != mapTxLocks.end()) {
        sigs = (*i).second.CountSignatures();
    }
    if (sigs >= SWIFTTX_SIGNATURES_REQUIRED) {
        return nSwiftTXDepth;
    }

    return 0;
}

bool VerifyShnorrKeyImageTxIn(const CTxIn& txin, uint256 ctsHash)
{
    COutPoint prevout = txin.prevout;
    CTransaction prev;
    uint256 bh;
    if (!GetTransaction(prevout.hash, prev, bh, true)) {
        return false;
    }
    uint256 s(txin.s);
    unsigned char S[33];
    CPubKey P;
    ExtractPubKey(prev.vout[prevout.n].scriptPubKey, P);
    PointHashingSuccessively(P, s.begin(), S);
    CPubKey R(txin.R.begin(), txin.R.end());

    //compute H(R)I = eI
    unsigned char buff[33 + 32];
    memcpy(buff, R.begin(), 33);
    memcpy(buff + 33, ctsHash.begin(), 32);
    uint256 e = Hash(buff, buff + 65);
    unsigned char eI[33];
    memcpy(eI, txin.keyImage.begin(), 33);
    if (!secp256k1_ec_pubkey_tweak_mul(eI, 33, e.begin())) return false;

    secp256k1_pedersen_commitment R_commitment;
    secp256k1_pedersen_serialized_pubkey_to_commitment(R.begin(), 33, &R_commitment);

    //convert CI*I into commitment
    secp256k1_pedersen_commitment eI_commitment;
    secp256k1_pedersen_serialized_pubkey_to_commitment(eI, 33, &eI_commitment);

    const secp256k1_pedersen_commitment* twoElements[2];
    twoElements[0] = &R_commitment;
    twoElements[1] = &eI_commitment;
    secp256k1_pedersen_commitment sum;
    if (!secp256k1_pedersen_commitment_sum_pos(GetContext(), twoElements, 2, &sum))
        throw std::runtime_error("failed to compute secp256k1_pedersen_commitment_sum_pos");
    size_t tempLength;
    unsigned char recomputed[33];
    if (!secp256k1_pedersen_commitment_to_serialized_pubkey(&sum, recomputed, &tempLength))
        throw std::runtime_error("failed to serialize pedersen commitment");

    for (int i = 0; i < 33; i++)
        if (S[i] != recomputed[i]) return false;
    return true;
}

bool VerifyShnorrKeyImageTx(const CTransaction& tx)
{
    //check if a transaction is staking or spending collateral
    //this assumes that the transaction is already checked for either a staking transaction or transactions spending only UTXOs of 2.5K PRCY
    if (!tx.IsCoinStake()) return true;
    uint256 cts = GetTxInSignatureHash(tx.vin[0]);
    return VerifyShnorrKeyImageTxIn(tx.vin[0], cts);
}



bool CheckFinalTx(const CTransaction& tx, int flags)
{
    AssertLockHeld(cs_main);

    // By convention a negative value for flags indicates that the
    // current network-enforced consensus rules should be used. In
    // a future soft-fork scenario that would mean checking which
    // rules would be enforced for the next block and setting the
    // appropriate flags. At the present time no soft-forks are
    // scheduled, so no flags are set.
    flags = std::max(flags, 0);

    // CheckFinalTx() uses chainActive.Height()+1 to evaluate
    // nLockTime because when IsFinalTx() is called within
    // CBlock::AcceptBlock(), the height of the block *being*
    // evaluated is what is used. Thus if we want to know if a
    // transaction can be part of the *next* block, we need to call
    // IsFinalTx() with one more than chainActive.Height().
    const int nBlockHeight = chainActive.Height() + 1;

    // BIP113 will require that time-locked transactions have nLockTime set to
    // less than the median time of the previous block they're contained in.
    // When the next block is created its previous block will be the current
    // chain tip, so we use that to calculate the median time passed to
    // IsFinalTx() if LOCKTIME_MEDIAN_TIME_PAST is set.
    const int64_t nBlockTime = (flags & LOCKTIME_MEDIAN_TIME_PAST) ? chainActive.Tip()->GetMedianTimePast() : GetAdjustedTime();

    return IsFinalTx(tx, nBlockHeight, nBlockTime);
}

CAmount GetMinRelayFee(const CTransaction& tx, unsigned int nBytes, bool fAllowFree)
{
    {
        LOCK(mempool.cs);
        uint256 hash = tx.GetHash();
        double dPriorityDelta = 0;
        CAmount nFeeDelta = 0;
        mempool.ApplyDeltas(hash, dPriorityDelta, nFeeDelta);
        if (dPriorityDelta > 0 || nFeeDelta > 0)
            return 0;
    }

    CAmount nMinFee = ::minRelayTxFee.GetFee(nBytes);

    if (fAllowFree) {
        // There is a free transaction area in blocks created by most miners,
        // * If we are relaying we allow transactions up to DEFAULT_BLOCK_PRIORITY_SIZE - 1000
        //   to be considered to fall into this category. We don't want to encourage sending
        //   multiple transactions instead of one big transaction to avoid fees.
        if (nBytes < (DEFAULT_BLOCK_PRIORITY_SIZE - 1000))
            nMinFee = 0;
    }

    return nMinFee;
}

bool CheckHaveInputs(const CCoinsViewCache& view, const CTransaction& tx)
{
    CBlockIndex* pindexPrev = mapBlockIndex.find(view.GetBestBlock())->second;
    int nSpendHeight = pindexPrev->nHeight + 1;
    if (!tx.IsCoinBase()) {
        for (unsigned int i = 0; i < tx.vin.size(); i++) {
            //check output and decoys
            std::vector<COutPoint> alldecoys = tx.vin[i].decoys;

            alldecoys.push_back(tx.vin[i].prevout);
            for (size_t j = 0; j < alldecoys.size(); j++) {
                CTransaction prev;
                uint256 bh;
                if (!GetTransaction(alldecoys[j].hash, prev, bh, true)) {
                    return false;
                }

                if (mapBlockIndex.count(bh) < 1) return false;
                if (prev.IsCoinStake() || prev.IsCoinAudit() || prev.IsCoinBase()) {
                    if (nSpendHeight - mapBlockIndex[bh]->nHeight < Params().COINBASE_MATURITY()) return false;
                }

                CBlockIndex* tip = chainActive.Tip();
                if (!pindexPrev) tip = pindexPrev;

                uint256 hashTip = tip->GetBlockHash();
                //verify that tip and hashBlock must be in the same fork
                CBlockIndex* atTheblock = mapBlockIndex[bh];
                if (!atTheblock) {
                    LogPrintf("%s: Decoy for transaction %s not in the same chain as block height=%s hash=%s\n", __func__, alldecoys[j].hash.GetHex(), tip->nHeight, tip->GetBlockHash().GetHex());
                    return false;
                } else {
                    CBlockIndex* ancestor = tip->GetAncestor(atTheblock->nHeight);
                    if (ancestor != atTheblock) {
                        LogPrintf("%s: Decoy for transaction %s not in the same chain as block height=%s hash=%s\n", __func__, alldecoys[j].hash.GetHex(), tip->nHeight, tip->GetBlockHash().GetHex());
                        return false;
                    }
                }
            }
            if (!tx.IsCoinStake()) {
                if (tx.vin[i].decoys.size() != tx.vin[0].decoys.size()) {
                    LogPrintf("%s: Transaction does not have the same ring size for inputs\n", __func__);
                    return false;
                }
            }
        }

        if (tx.IsCoinStake()) {
            if (!VerifyShnorrKeyImageTx(tx)) {
                LogPrintf("%s: Failed to verify correctness of key image of staking transaction\n", __func__);
                return false;
            }
        }
    }
    return true;
}


bool AcceptToMemoryPool(CTxMemPool& pool, CValidationState& state, const CTransaction& tx, bool fLimitFree, bool* pfMissingInputs, bool fRejectInsaneFee, bool ignoreFees)
{
    AssertLockHeld(cs_main);
    if (pfMissingInputs)
        *pfMissingInputs = false;

    // Check transaction
    if (!CheckTransaction(tx, true, state))
        return state.DoS(100, error("%s : CheckTransaction failed", __func__), REJECT_INVALID, "bad-tx");

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return state.DoS(100, error("%s : coinbase as individual tx",
                __func__), REJECT_INVALID, "coinbase");

    //Coinstake is also only valid in a block, not as a loose transaction
    if (tx.IsCoinStake())
        return state.DoS(100, error("%s : coinstake as individual tx (id=%s): %s",
                __func__, tx.GetHash().GetHex(), tx.ToString()), REJECT_INVALID, "coinstake");

    // Only accept nLockTime-using transactions that can be mined in the next
    // block; we don't want our mempool filled up with transactions that can't
    // be mined yet.
    if (!CheckFinalTx(tx, STANDARD_LOCKTIME_VERIFY_FLAGS))
        return state.DoS(0, false, REJECT_NONSTANDARD, "non-final");

    // Rather not work on nonstandard transactions (unless -testnet/-regtest)
    std::string reason;
    if (Params().RequireStandard() && !IsStandardTx(tx, reason))
        return state.DoS(0,
            error("AcceptToMemoryPool : nonstandard transaction: %s", reason),
            REJECT_NONSTANDARD, reason);
    // is it already in the memory pool?
    uint256 hash = tx.GetHash();
    if (pool.exists(hash)) {
        return error("%s tx already in mempool", __func__);
    }
    // ----------- swiftTX transaction scanning -----------

    {
        CCoinsView dummy;
        CCoinsViewCache view(&dummy);
        CAmount nValueIn = 0;
        {
            LOCK(pool.cs);
            CCoinsViewMemPool viewMemPool(pcoinsTip, pool);
            view.SetBackend(viewMemPool);
            // do we already have it?
            if (view.HaveCoins(hash)) {
                return false;
            }

            int banscore;
            if (!tx.IsCoinStake() && !tx.IsCoinBase() && !tx.IsCoinAudit()) {
                if (!tx.IsCoinAudit()) {
                    if (masternodeSync.IsBlockchainSynced()) {
                        banscore = 100;
                    } else {
                        banscore = 1;
                    }
                    if (!VerifyRingSignatureWithTxFee(tx, chainActive.Tip())) {
                        return state.DoS(banscore, error("AcceptToMemoryPool() : Ring Signature check for transaction %s failed", tx.GetHash().ToString()),
                            REJECT_INVALID, "bad-ring-signature");
                    }
                    if (!VerifyBulletProofAggregate(tx))
                        return state.DoS(100, error("AcceptToMemoryPool() : Bulletproof check for transaction %s failed", tx.GetHash().ToString()),
                            REJECT_INVALID, "bad-bulletproof");
                }
            }

            // Check key images not duplicated with what in db
            for (const CTxIn& txin : tx.vin) {
                const CKeyImage& keyImage = txin.keyImage;
                if (IsSpentKeyImage(keyImage.GetHex(), UINT256_ZERO)) {
                    return state.Invalid(error("AcceptToMemoryPool : key image already spent %s", keyImage.GetHex()),
                        REJECT_DUPLICATE, "bad-txns-inputs-spent");
                }
                // check for invalid/fraudulent inputs
                if (!ValidOutPoint(txin.prevout)) {
                    return state.Invalid(error("%s : tried to spend invalid input %s in tx %s", __func__, txin.prevout.ToString(),
                                                tx.GetHash().GetHex()), REJECT_INVALID, "bad-txns-invalid-inputs");
                }
            }
            // check for invalid/fraudulent decoys
            for (unsigned int i = 0; i < tx.vin.size(); i++) {
                if (tx.IsCoinBase()) continue;
                //check output and decoys
                std::vector<COutPoint> alldecoys = tx.vin[i].decoys;

                alldecoys.push_back(tx.vin[i].prevout);
                for (size_t j = 0; j < alldecoys.size(); j++) {
                    CTransaction prev;
                    uint256 bh;
                    if (!GetTransaction(alldecoys[j].hash, prev, bh, true)) {
                        return false;
                    }
                    if (mapBlockIndex.count(bh) < 1) return false;
                    if (!ValidOutPoint(alldecoys[j])) {
                        return state.DoS(100, error("%s : tried to spend invalid decoy %s in tx %s", __func__, alldecoys[j].ToString(),
                                                    tx.GetHash().GetHex()), REJECT_INVALID, "bad-txns-invalid-inputs");
                    }
                }
            }

            // Bring the best block into scope
            view.GetBestBlock();
            nValueIn = GetValueIn(view, tx);

            // we have all inputs cached now, so switch back to dummy, so we don't need to keep lock on mempool
            view.SetBackend(dummy);
        }

        // Check for non-standard pay-to-script-hash in inputs
        if (Params().RequireStandard() && !AreInputsStandard(tx, view))
            return error("AcceptToMemoryPool: nonstandard transaction input");
        // Check that the transaction doesn't have an excessive number of
        // sigops, making it impossible to mine. Since the coinbase transaction
        // itself can contain sigops MAX_TX_SIGOPS is less than
        // MAX_BLOCK_SIGOPS; we still consider this an invalid rather than
        // merely non-standard transaction.
        {
            unsigned int nSigOps = GetLegacySigOpCount(tx);
            unsigned int nMaxSigOps = MAX_TX_SIGOPS_CURRENT;
            if (nSigOps > nMaxSigOps)
                return state.DoS(0,
                    error("AcceptToMemoryPool : too many sigops %s, %d > %d",
                        hash.ToString(), nSigOps, nMaxSigOps),
                    REJECT_NONSTANDARD, "bad-txns-too-many-sigops");
        }

        CAmount nFees = tx.nTxFee;
        double dPriority = GetPriority(tx, chainActive.Height());

        CTxMemPoolEntry entry(tx, nFees, GetTime(), dPriority, chainActive.Height());
        unsigned int nSize = entry.GetTxSize();

        // Don't accept it if it can't get into a block
        if (!ignoreFees) {
            CAmount txMinFee = GetMinRelayFee(tx, nSize, true);
            if (fLimitFree && nFees < txMinFee)
                return state.DoS(0, error("AcceptToMemoryPool : not enough fees %s, %d < %d", hash.ToString(), nFees, txMinFee),
                    REJECT_INSUFFICIENTFEE, "insufficient fee");

            // Continuously rate-limit free (really, very-low-fee) transactions
            // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
            // be annoying or make others' transactions take longer to confirm.
            if (fLimitFree && nFees < ::minRelayTxFee.GetFee(nSize - 300)) {
                static RecursiveMutex csFreeLimiter;
                static double dFreeCount;
                static int64_t nLastTime;
                int64_t nNow = GetTime();

                LOCK(csFreeLimiter);

                // Use an exponentially decaying ~10-minute window:
                dFreeCount *= pow(1.0 - 1.0 / 600.0, (double)(nNow - nLastTime));
                nLastTime = nNow;
                // -limitfreerelay unit is thousand-bytes-per-minute
                // At default rate it would take over a month to fill 1GB
                if (dFreeCount >= GetArg("-limitfreerelay", 30) * 10 * 1000)
                    return state.DoS(0, error("AcceptToMemoryPool : free transaction rejected by rate limiter"),
                        REJECT_INSUFFICIENTFEE, "rate limited free transaction");
                LogPrint(BCLog::MEMPOOL, "Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount + nSize);
                dFreeCount += nSize;
            }
        }

        bool fCLTVIsActivated = chainActive.Tip()->nHeight >= Params().BIP65ActivationHeight();

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        int flags = STANDARD_SCRIPT_VERIFY_FLAGS;
        if (fCLTVIsActivated)
            flags |= SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;
        if (!CheckInputs(tx, state, view, true, flags, true)) {
            return error("AcceptToMemoryPool: ConnectInputs failed %s", hash.ToString());
        }

        // Check again against just the consensus-critical mandatory script
        // verification flags, in case of bugs in the standard flags that cause
        // transactions to pass as valid when they're actually invalid. For
        // instance the STRICTENC flag was incorrectly allowing certain
        // CHECKSIG NOT scripts to pass, even though they were invalid.
        //
        // There is a similar check in CreateNewBlock() to prevent creating
        // invalid blocks, however allowing such transactions into the mempool
        // can be exploited as a DoS attack.
        flags = MANDATORY_SCRIPT_VERIFY_FLAGS;
        if (fCLTVIsActivated)
            flags |= SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;
        if (!CheckInputs(tx, state, view, true, flags, true)) {
            return error(
                "AcceptToMemoryPool: BUG! PLEASE REPORT THIS! ConnectInputs failed against MANDATORY but not STANDARD flags %s",
                hash.ToString());
        }
        // Store transaction in memory
        pool.addUnchecked(hash, entry);
    }
    SyncWithWallets(tx, nullptr);

    if (pwalletMain) {
        LOCK(pwalletMain->cs_wallet);
        if (pwalletMain->mapWallet.count(tx.GetHash()) == 1) {
            for (size_t i = 0; i < tx.vin.size(); i++) {
                std::string outpoint = tx.vin[i].prevout.hash.GetHex() + std::to_string(tx.vin[i].prevout.n);
                if (pwalletMain->outpointToKeyImages[outpoint] == tx.vin[i].keyImage) {
                    pwalletMain->inSpendQueueOutpoints[tx.vin[i].prevout] = true;
                    continue;
                }

                for (size_t j = 0; j < tx.vin[i].decoys.size(); j++) {
                    std::string outpoint = tx.vin[i].decoys[j].hash.GetHex() + std::to_string(tx.vin[i].decoys[j].n);
                    if (pwalletMain->outpointToKeyImages[outpoint] == tx.vin[i].keyImage) {
                        pwalletMain->inSpendQueueOutpoints[tx.vin[i].decoys[j]] = true;
                        break;
                    }
                }
            }
        }
    }

    return true;
}

bool AcceptableInputs(CTxMemPool& pool, CValidationState& state, const CTransaction& tx, bool fLimitFree, bool* pfMissingInputs, bool fRejectInsaneFee, bool isDSTX)
{
    AssertLockHeld(cs_main);
    if (pfMissingInputs)
        *pfMissingInputs = false;

    const int chainHeight = chainActive.Height();

    if (!CheckTransaction(tx, true, state))
        return error("AcceptableInputs: CheckTransaction failed");

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return state.DoS(100, error("AcceptableInputs: coinbase as individual tx"),
            REJECT_INVALID, "coinbase");

    // Rather not work on nonstandard transactions (unless -testnet/-regtest)
    std::string reason;

    // is it already in the memory pool?
    uint256 hash = tx.GetHash();
    if (pool.exists(hash))
        return false;

    // ----------- swiftTX transaction scanning -----------

    for (const CTxIn& in : tx.vin) {
        if (mapLockedInputs.count(in.prevout)) {
            if (mapLockedInputs[in.prevout] != tx.GetHash()) {
                return state.DoS(0,
                    error("AcceptableInputs : conflicts with existing transaction lock: %s", reason),
                    REJECT_INVALID, "tx-lock-conflict");
            }
        }
    }

    // Check for conflicts with in-memory transactions
    {
        LOCK(pool.cs); // protect pool.mapNextTx
        for (const auto &in : tx.vin) {
            COutPoint outpoint = in.prevout;
            if (pool.mapNextTx.count(outpoint)) {
                // Disable replacement feature for now
                return false;
            }
        }
    }


    {
        CCoinsView dummy;
        CCoinsViewCache view(&dummy);

        CAmount nValueIn = 0;
        {
            LOCK(pool.cs);
            CCoinsViewMemPool viewMemPool(pcoinsTip, pool);
            view.SetBackend(viewMemPool);

            // do we already have it?
            if (view.HaveCoins(hash))
                return false;

            // do all inputs exist?
            // Note that this does not check for the presence of actual outputs (see the next check for that),
            // only helps filling in pfMissingInputs (to determine missing vs spent).
            for (const CTxIn& txin : tx.vin) {
                if (!view.HaveCoins(txin.prevout.hash)) {
                    if (pfMissingInputs)
                        *pfMissingInputs = true;
                    return false;
                }
                //Check for invalid/fraudulent inputs
                if (!ValidOutPoint(txin.prevout)) {
                    return state.Invalid(error("%s : tried to spend invalid input %s in tx %s", __func__, txin.prevout.ToString(),
                                                tx.GetHash().GetHex()), REJECT_INVALID, "bad-txns-invalid-inputs");
                }
            }

            // check for invalid/fraudulent inputs
            for (unsigned int i = 0; i < tx.vin.size(); i++) {
                if (tx.IsCoinBase()) continue;
                //check output and decoys
                std::vector<COutPoint> alldecoys = tx.vin[i].decoys;

                alldecoys.push_back(tx.vin[i].prevout);
                for (size_t j = 0; j < alldecoys.size(); j++) {
                    CTransaction prev;
                    uint256 bh;
                    if (!GetTransaction(alldecoys[j].hash, prev, bh, true)) {
                        return false;
                    }
                    if (mapBlockIndex.count(bh) < 1) return false;
                    if (!ValidOutPoint(alldecoys[j])) {
                        return state.DoS(100, error("%s : tried to spend invalid decoy %s in tx %s", __func__, alldecoys[j].ToString(),
                                                    tx.GetHash().GetHex()), REJECT_INVALID, "bad-txns-invalid-inputs");
                    }
                }
            }

            // are the actual inputs available?
            if (!CheckHaveInputs(view, tx))
                return state.Invalid(error("AcceptableInputs : inputs already spent"),
                    REJECT_DUPLICATE, "bad-txns-inputs-spent");

            // Bring the best block into scope
            view.GetBestBlock();

            nValueIn = GetValueIn(view, tx);

            // we have all inputs cached now, so switch back to dummy, so we don't need to keep lock on mempool
            view.SetBackend(dummy);
        }

        // Check that the transaction doesn't have an excessive number of
        // sigops, making it impossible to mine. Since the coinbase transaction
        // itself can contain sigops MAX_TX_SIGOPS is less than
        // MAX_BLOCK_SIGOPS; we still consider this an invalid rather than
        // merely non-standard transaction.
        unsigned int nSigOps = GetLegacySigOpCount(tx);
        unsigned int nMaxSigOps = MAX_TX_SIGOPS_CURRENT;
        if (nSigOps > nMaxSigOps)
            return state.DoS(0,
                error("AcceptableInputs : too many sigops %s, %d > %d",
                    hash.ToString(), nSigOps, nMaxSigOps),
                REJECT_NONSTANDARD, "bad-txns-too-many-sigops");

        CAmount nValueOut = tx.GetValueOut();
        CAmount nFees = tx.nTxFee;
        double dPriority = GetPriority(tx, chainHeight);

        CTxMemPoolEntry entry(tx, nFees, GetTime(), dPriority, chainHeight);
        unsigned int nSize = entry.GetTxSize();

        // Don't accept it if it can't get into a block
        // but prioritise dstx and don't check fees for it
        if (isDSTX) {
            mempool.PrioritiseTransaction(hash, hash.ToString(), 1000, 0.1 * COIN);
        } else { // same as !ignoreFees for AcceptToMemoryPool
            CAmount txMinFee = GetMinRelayFee(tx, nSize, true);
            if (fLimitFree && nFees < txMinFee)
                return state.DoS(0, error("AcceptableInputs : not enough fees %s, %d < %d", hash.ToString(), nFees, txMinFee),
                    REJECT_INSUFFICIENTFEE, "insufficient fee");

            // Require that free transactions have sufficient priority to be mined in the next block.
            //TODO: @campv need to recompute the fee
            /*if (GetBoolArg("-relaypriority", true) && nFees < ::minRelayTxFee.GetFee(nSize) &&
                !AllowFree(view.GetPriority(tx, chainHeight + 1))) {
                return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "insufficient priority");
            }*/

            // Continuously rate-limit free (really, very-low-fee) transactions
            // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
            // be annoying or make others' transactions take longer to confirm.
            if (fLimitFree && nFees < ::minRelayTxFee.GetFee(nSize)) {
                static RecursiveMutex csFreeLimiter;
                static double dFreeCount;
                static int64_t nLastTime;
                int64_t nNow = GetTime();

                LOCK(csFreeLimiter);

                // Use an exponentially decaying ~10-minute window:
                dFreeCount *= pow(1.0 - 1.0 / 600.0, (double)(nNow - nLastTime));
                nLastTime = nNow;
                // -limitfreerelay unit is thousand-bytes-per-minute
                // At default rate it would take over a month to fill 1GB
                if (dFreeCount >= GetArg("-limitfreerelay", 30) * 10 * 1000)
                    return state.DoS(0, error("AcceptableInputs : free transaction rejected by rate limiter"),
                        REJECT_INSUFFICIENTFEE, "rate limited free transaction");
                LogPrint(BCLog::MEMPOOL, "Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount + nSize);
                dFreeCount += nSize;
            }
        }

        if (fRejectInsaneFee && nFees > ::minRelayTxFee.GetFee(nSize) * 10000)
            return error("AcceptableInputs: insane fees %s, %d > %d",
                hash.ToString(),
                nFees, ::minRelayTxFee.GetFee(nSize) * 10000);

        bool fCLTVIsActivated = chainActive.Tip()->nHeight >= Params().BIP65ActivationHeight();

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        int flags = STANDARD_SCRIPT_VERIFY_FLAGS;
        if (fCLTVIsActivated)
            flags |= SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;
        if (!CheckInputs(tx, state, view, false, flags, true)) {
            return error("AcceptableInputs: ConnectInputs failed %s", hash.ToString());
        }

        // Check again against just the consensus-critical mandatory script
        // verification flags, in case of bugs in the standard flags that cause
        // transactions to pass as valid when they're actually invalid. For
        // instance the STRICTENC flag was incorrectly allowing certain
        // CHECKSIG NOT scripts to pass, even though they were invalid.
        //
        // There is a similar check in CreateNewBlock() to prevent creating
        // invalid blocks, however allowing such transactions into the mempool
        // can be exploited as a DoS attack.
        // for any real tx this will be checked on AcceptToMemoryPool anyway
    }

    return true;
}

/** Return transaction in tx, and if it was found inside a block, its hash is placed in hashBlock */
bool GetTransaction(const uint256& hash, CTransaction& txOut, uint256& hashBlock, bool fAllowSlow, CBlockIndex* blockIndex)
{
    CBlockIndex* pindexSlow = blockIndex;

    LOCK(cs_main);

    if (!blockIndex) {
        if (mempool.lookup(hash, txOut)) {
            return true;
        }

        if (fTxIndex) {
            CDiskTxPos postx;
            if (pblocktree->ReadTxIndex(hash, postx)) {
                CAutoFile file(OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
                if (file.IsNull())
                    return error("%s: OpenBlockFile failed", __func__);
                CBlockHeader header;
                try {
                    file >> header;
                    fseek(file.Get(), postx.nTxOffset, SEEK_CUR);
                    file >> txOut;
                } catch (const std::exception& e) {
                    return error("%s : Deserialize or I/O error - %s", __func__, e.what());
                }
                hashBlock = header.GetHash();
                if (txOut.GetHash() != hash)
                    return error("%s : txid mismatch, %s, %s", __func__, txOut.GetHash().GetHex(), hash.GetHex());
                return true;
            }

            // transaction not found in the index, nothing more can be done
            return false;
        }

        if (fAllowSlow) { // use coin database to locate block that contains transaction, and scan it
            int nHeight = -1;
            {
                CCoinsViewCache& view = *pcoinsTip;
                const CCoins* coins = view.AccessCoins(hash);
                if (coins)
                    nHeight = coins->nHeight;
            }
            if (nHeight > 0)
                pindexSlow = chainActive[nHeight];
        }
    }

    if (pindexSlow) {
        CBlock block;
        if (ReadBlockFromDisk(block, pindexSlow)) {
            for (const CTransaction& tx : block.vtx) {
                if (tx.GetHash() == hash) {
                    txOut = tx;
                    hashBlock = pindexSlow->GetBlockHash();
                    return true;
                }
            }
        }
    }

    return false;
}


//////////////////////////////////////////////////////////////////////////////
//
// CBlock and CBlockIndex
//

bool WriteBlockToDisk(CBlock& block, CDiskBlockPos& pos)
{
    // Open history file to append
    CAutoFile fileout(OpenBlockFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("WriteBlockToDisk : OpenBlockFile failed");

    // Write index header
    unsigned int nSize = fileout.GetSerializeSize(block);
    fileout << FLATDATA(Params().MessageStart()) << nSize;

    // Write block
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0)
        return error("WriteBlockToDisk : ftell failed");
    pos.nPos = (unsigned int)fileOutPos;
    fileout << block;

    return true;
}

bool ReadBlockFromDisk(CBlock& block, const CDiskBlockPos& pos)
{
    block.SetNull();

    // Open history file to read
    CAutoFile filein(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("ReadBlockFromDisk : OpenBlockFile failed");

    // Read block
    try {
        filein >> block;
    } catch (const std::exception& e) {
        return error("%s : Deserialize or I/O error - %s", __func__, e.what());
    }

    // Check the header
    if (block.IsProofOfWork()) {
        if (!CheckProofOfWork(block.GetHash(), block.nBits))
            return error("ReadBlockFromDisk : Errors in block header");
    }

    return true;
}

bool ReadBlockFromDisk(CBlock& block, const CBlockIndex* pindex)
{
    if (!ReadBlockFromDisk(block, pindex->GetBlockPos()))
        return false;
    if (block.GetHash() != pindex->GetBlockHash()) {
        LogPrintf("%s : block=%s index=%s\n", __func__, block.GetHash().ToString().c_str(),
            pindex->GetBlockHash().ToString().c_str());
        return error("ReadBlockFromDisk(CBlock&, CBlockIndex*) : GetHash() doesn't match index");
    }
    return true;
}


double ConvertBitsToDouble(unsigned int nBits)
{
    int nShift = (nBits >> 24) & 0xff;

    double dDiff =
        (double)0x0000ffff / (double)(nBits & 0x00ffffff);

    while (nShift < 29) {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29) {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

CAmount GetBlockValue(int nHeight)
{
    LOCK(cs_main);

    if (Params().IsRegTestNet()) {
        if (nHeight == 0)
            return 250 * COIN;
    }

    int64_t nSubsidy = 0;
    int64_t nMoneySupply = chainActive.Tip()->nMoneySupply;

    if (nMoneySupply >= Params().TOTAL_SUPPLY) {
        //zero rewards when total supply reach 70M PRCY
        return 0;
    }
    if (nHeight < Params().LAST_POW_BLOCK()) {
        nSubsidy = 120000 * COIN;
    } else {
        nSubsidy = 1 * COIN;
    }

    if (nMoneySupply + nSubsidy >= Params().TOTAL_SUPPLY) {
        nSubsidy = Params().TOTAL_SUPPLY - nMoneySupply;
    }

    return nSubsidy;
}

CAmount GetSeeSaw(const CAmount& blockValue, int nMasternodeCount, int nHeight)
{
    //if a mn count is inserted into the function we are looking for a specific result for a masternode count
    if (nMasternodeCount < 1) {
        nMasternodeCount = mnodeman.size();
    }

    int64_t nMoneySupply = chainActive.Tip()->nMoneySupply;
    int64_t mNodeCoins = nMasternodeCount * Params().MNCollateralAmt();

    // Use this log to compare the masternode count for different clients
    LogPrintf("Adjusting seesaw at height %d with %d masternodes (without drift: %d) at %ld\n", nHeight,nMasternodeCount, nMasternodeCount - Params().MasternodeCountDrift(), GetTime());
    LogPrint(BCLog::SUPPLY, "%s: moneysupply=%s, nodecoins=%s \n", __func__, FormatMoney(nMoneySupply).c_str(), FormatMoney(mNodeCoins).c_str());

    CAmount ret = 0;
    if (mNodeCoins == 0) {
        ret = 0;
    } else if (nHeight <= Params().SoftFork()) {
        if (mNodeCoins <= (nMoneySupply * .05) && mNodeCoins > 0) {
            ret = blockValue * .6;
        } else if (mNodeCoins <= (nMoneySupply * .1) && mNodeCoins > (nMoneySupply * .05)) {
            ret = blockValue * .59;
        } else if (mNodeCoins <= (nMoneySupply * .15) && mNodeCoins > (nMoneySupply * .1)) {
            ret = blockValue * .58;
        } else if (mNodeCoins <= (nMoneySupply * .2) && mNodeCoins > (nMoneySupply * .15)) {
            ret = blockValue * .57;
        } else if (mNodeCoins <= (nMoneySupply * .25) && mNodeCoins > (nMoneySupply * .2)) {
            ret = blockValue * .56;
        } else if (mNodeCoins <= (nMoneySupply * .3) && mNodeCoins > (nMoneySupply * .25)) {
            ret = blockValue * .55;
        } else if (mNodeCoins <= (nMoneySupply * .35) && mNodeCoins > (nMoneySupply * .3)) {
            ret = blockValue * .54;
        } else if (mNodeCoins <= (nMoneySupply * .4) && mNodeCoins > (nMoneySupply * .35)) {
            ret = blockValue * .53;
        } else if (mNodeCoins <= (nMoneySupply * .45) && mNodeCoins > (nMoneySupply * .4)) {
            ret = blockValue * .52;
        } else if (mNodeCoins <= (nMoneySupply * .5) && mNodeCoins > (nMoneySupply * .45)) {
            ret = blockValue * .51;
        } else if (mNodeCoins <= (nMoneySupply * .525) && mNodeCoins > (nMoneySupply * .5)) {
            ret = blockValue * .50;
        } else if (mNodeCoins <= (nMoneySupply * .55) && mNodeCoins > (nMoneySupply * .525)) {
            ret = blockValue * .49;
        } else if (mNodeCoins <= (nMoneySupply * .6) && mNodeCoins > (nMoneySupply * .55)) {
            ret = blockValue * .48;
        } else if (mNodeCoins <= (nMoneySupply * .65) && mNodeCoins > (nMoneySupply * .6)) {
            ret = blockValue * .47;
        } else if (mNodeCoins <= (nMoneySupply * .7) && mNodeCoins > (nMoneySupply * .65)) {
            ret = blockValue * .46;
        } else if (mNodeCoins <= (nMoneySupply * .75) && mNodeCoins > (nMoneySupply * .7)) {
            ret = blockValue * .45;
        } else if (mNodeCoins <= (nMoneySupply * .8) && mNodeCoins > (nMoneySupply * .75)) {
            ret = blockValue * .44;
        } else if (mNodeCoins <= (nMoneySupply * .85) && mNodeCoins > (nMoneySupply * .8)) {
            ret = blockValue * .43;
        } else if (mNodeCoins <= (nMoneySupply * .9) && mNodeCoins > (nMoneySupply * .85)) {
            ret = blockValue * .42;
        } else if (mNodeCoins <= (nMoneySupply * .95) && mNodeCoins > (nMoneySupply * .9)) {
            ret = blockValue * .41;
        } else {
            ret = blockValue * .40;
        }
	} else if (nHeight > Params().SoftFork()) {
		if (mNodeCoins <= (nMoneySupply * .01) && mNodeCoins > 0) {
            ret = blockValue * .6;
        } else if (mNodeCoins <= (nMoneySupply * .02) && mNodeCoins > (nMoneySupply * .01)) {
            ret = blockValue * .598;
        } else if (mNodeCoins <= (nMoneySupply * .03) && mNodeCoins > (nMoneySupply * .02)) {
            ret = blockValue * .596;
        } else if (mNodeCoins <= (nMoneySupply * .04) && mNodeCoins > (nMoneySupply * .03)) {
            ret = blockValue * .594;
        } else if (mNodeCoins <= (nMoneySupply * .05) && mNodeCoins > (nMoneySupply * .04)) {
            ret = blockValue * .592;
        } else if (mNodeCoins <= (nMoneySupply * .06) && mNodeCoins > (nMoneySupply * .05)) {
            ret = blockValue * .59;
        } else if (mNodeCoins <= (nMoneySupply * .07) && mNodeCoins > (nMoneySupply * .06)) {
            ret = blockValue * .588;
        } else if (mNodeCoins <= (nMoneySupply * .08) && mNodeCoins > (nMoneySupply * .07)) {
            ret = blockValue * .586;
        } else if (mNodeCoins <= (nMoneySupply * .09) && mNodeCoins > (nMoneySupply * .08)) {
            ret = blockValue * .584;
        } else if (mNodeCoins <= (nMoneySupply * .1) && mNodeCoins > (nMoneySupply * .09)) {
            ret = blockValue * .582;
        } else if (mNodeCoins <= (nMoneySupply * .11) && mNodeCoins > (nMoneySupply * .1)) {
            ret = blockValue * .58;
        } else if (mNodeCoins <= (nMoneySupply * .12) && mNodeCoins > (nMoneySupply * .11)) {
            ret = blockValue * .578;
        } else if (mNodeCoins <= (nMoneySupply * .13) && mNodeCoins > (nMoneySupply * .12)) {
            ret = blockValue * .576;
        } else if (mNodeCoins <= (nMoneySupply * .14) && mNodeCoins > (nMoneySupply * .13)) {
            ret = blockValue * .574;
        } else if (mNodeCoins <= (nMoneySupply * .15) && mNodeCoins > (nMoneySupply * .14)) {
            ret = blockValue * .572;
        } else if (mNodeCoins <= (nMoneySupply * .16) && mNodeCoins > (nMoneySupply * .15)) {
            ret = blockValue * .57;
        } else if (mNodeCoins <= (nMoneySupply * .17) && mNodeCoins > (nMoneySupply * .16)) {
            ret = blockValue * .568;
        } else if (mNodeCoins <= (nMoneySupply * .18) && mNodeCoins > (nMoneySupply * .17)) {
            ret = blockValue * .566;
        } else if (mNodeCoins <= (nMoneySupply * .19) && mNodeCoins > (nMoneySupply * .18)) {
            ret = blockValue * .564;
        } else if (mNodeCoins <= (nMoneySupply * .20) && mNodeCoins > (nMoneySupply * .19)) {
            ret = blockValue * .562;
        } else if (mNodeCoins <= (nMoneySupply * .21) && mNodeCoins > (nMoneySupply * .20)) {
            ret = blockValue * .56;
        } else if (mNodeCoins <= (nMoneySupply * .22) && mNodeCoins > (nMoneySupply * .21)) {
            ret = blockValue * .558;
        } else if (mNodeCoins <= (nMoneySupply * .23) && mNodeCoins > (nMoneySupply * .22)) {
            ret = blockValue * .556;
        } else if (mNodeCoins <= (nMoneySupply * .24) && mNodeCoins > (nMoneySupply * .23)) {
            ret = blockValue * .554;
        } else if (mNodeCoins <= (nMoneySupply * .25) && mNodeCoins > (nMoneySupply * .24)) {
            ret = blockValue * .552;
        } else if (mNodeCoins <= (nMoneySupply * .26) && mNodeCoins > (nMoneySupply * .25)) {
            ret = blockValue * .55;
        } else if (mNodeCoins <= (nMoneySupply * .27) && mNodeCoins > (nMoneySupply * .26)) {
            ret = blockValue * .548;
        } else if (mNodeCoins <= (nMoneySupply * .28) && mNodeCoins > (nMoneySupply * .27)) {
            ret = blockValue * .546;
        } else if (mNodeCoins <= (nMoneySupply * .29) && mNodeCoins > (nMoneySupply * .28)) {
            ret = blockValue * .544;
        } else if (mNodeCoins <= (nMoneySupply * .30) && mNodeCoins > (nMoneySupply * .29)) {
            ret = blockValue * .542;
        } else if (mNodeCoins <= (nMoneySupply * .31) && mNodeCoins > (nMoneySupply * .30)) {
            ret = blockValue * .54;
        } else if (mNodeCoins <= (nMoneySupply * .32) && mNodeCoins > (nMoneySupply * .31)) {
            ret = blockValue * .538;
        } else if (mNodeCoins <= (nMoneySupply * .33) && mNodeCoins > (nMoneySupply * .32)) {
            ret = blockValue * .536;
        } else if (mNodeCoins <= (nMoneySupply * .34) && mNodeCoins > (nMoneySupply * .33)) {
            ret = blockValue * .534;
        } else if (mNodeCoins <= (nMoneySupply * .35) && mNodeCoins > (nMoneySupply * .34)) {
            ret = blockValue * .532;
        } else if (mNodeCoins <= (nMoneySupply * .36) && mNodeCoins > (nMoneySupply * .35)) {
            ret = blockValue * .53;
        } else if (mNodeCoins <= (nMoneySupply * .37) && mNodeCoins > (nMoneySupply * .36)) {
            ret = blockValue * .528;
        } else if (mNodeCoins <= (nMoneySupply * .38) && mNodeCoins > (nMoneySupply * .37)) {
            ret = blockValue * .526;
        } else if (mNodeCoins <= (nMoneySupply * .39) && mNodeCoins > (nMoneySupply * .38)) {
            ret = blockValue * .524;
        } else if (mNodeCoins <= (nMoneySupply * .40) && mNodeCoins > (nMoneySupply * .39)) {
            ret = blockValue * .522;
        } else if (mNodeCoins <= (nMoneySupply * .41) && mNodeCoins > (nMoneySupply * .40)) {
            ret = blockValue * .52;
        } else if (mNodeCoins <= (nMoneySupply * .42) && mNodeCoins > (nMoneySupply * .41)) {
            ret = blockValue * .518;
        } else if (mNodeCoins <= (nMoneySupply * .43) && mNodeCoins > (nMoneySupply * .42)) {
            ret = blockValue * .516;
        } else if (mNodeCoins <= (nMoneySupply * .44) && mNodeCoins > (nMoneySupply * .43)) {
            ret = blockValue * .514;
        } else if (mNodeCoins <= (nMoneySupply * .45) && mNodeCoins > (nMoneySupply * .44)) {
            ret = blockValue * .512;
        } else if (mNodeCoins <= (nMoneySupply * .46) && mNodeCoins > (nMoneySupply * .45)) {
            ret = blockValue * .51;
        } else if (mNodeCoins <= (nMoneySupply * .47) && mNodeCoins > (nMoneySupply * .46)) {
            ret = blockValue * .508;
        } else if (mNodeCoins <= (nMoneySupply * .48) && mNodeCoins > (nMoneySupply * .47)) {
            ret = blockValue * .506;
        } else if (mNodeCoins <= (nMoneySupply * .49) && mNodeCoins > (nMoneySupply * .48)) {
            ret = blockValue * .504;
        } else if (mNodeCoins <= (nMoneySupply * .50) && mNodeCoins > (nMoneySupply * .49)) {
            ret = blockValue * .502;
        } else if (mNodeCoins <= (nMoneySupply * .51) && mNodeCoins > (nMoneySupply * .5)) {
            ret = blockValue * .50;
        } else if (mNodeCoins <= (nMoneySupply * .52) && mNodeCoins > (nMoneySupply * .51)) {
            ret = blockValue * .498;
        } else if (mNodeCoins <= (nMoneySupply * .53) && mNodeCoins > (nMoneySupply * .52)) {
            ret = blockValue * .496;
        } else if (mNodeCoins <= (nMoneySupply * .54) && mNodeCoins > (nMoneySupply * .53)) {
            ret = blockValue * .494;
        } else if (mNodeCoins <= (nMoneySupply * .55) && mNodeCoins > (nMoneySupply * .54)) {
            ret = blockValue * .492;
        } else if (mNodeCoins <= (nMoneySupply * .56) && mNodeCoins > (nMoneySupply * .55)) {
            ret = blockValue * .49;
        } else if (mNodeCoins <= (nMoneySupply * .57) && mNodeCoins > (nMoneySupply * .56)) {
            ret = blockValue * .488;
        } else if (mNodeCoins <= (nMoneySupply * .58) && mNodeCoins > (nMoneySupply * .57)) {
            ret = blockValue * .486;
        } else if (mNodeCoins <= (nMoneySupply * .59) && mNodeCoins > (nMoneySupply * .58)) {
            ret = blockValue * .484;
        } else if (mNodeCoins <= (nMoneySupply * .60) && mNodeCoins > (nMoneySupply * .59)) {
            ret = blockValue * .482;
        } else if (mNodeCoins <= (nMoneySupply * .61) && mNodeCoins > (nMoneySupply * .60)) {
            ret = blockValue * .48;
        } else if (mNodeCoins <= (nMoneySupply * .62) && mNodeCoins > (nMoneySupply * .61)) {
            ret = blockValue * .478;
        } else if (mNodeCoins <= (nMoneySupply * .63) && mNodeCoins > (nMoneySupply * .62)) {
            ret = blockValue * .476;
        } else if (mNodeCoins <= (nMoneySupply * .64) && mNodeCoins > (nMoneySupply * .63)) {
            ret = blockValue * .474;
        } else if (mNodeCoins <= (nMoneySupply * .65) && mNodeCoins > (nMoneySupply * .64)) {
            ret = blockValue * .472;
        } else if (mNodeCoins <= (nMoneySupply * .66) && mNodeCoins > (nMoneySupply * .65)) {
            ret = blockValue * .47;
        } else if (mNodeCoins <= (nMoneySupply * .67) && mNodeCoins > (nMoneySupply * .66)) {
            ret = blockValue * .468;
        } else if (mNodeCoins <= (nMoneySupply * .68) && mNodeCoins > (nMoneySupply * .67)) {
            ret = blockValue * .466;
        } else if (mNodeCoins <= (nMoneySupply * .69) && mNodeCoins > (nMoneySupply * .68)) {
            ret = blockValue * .464;
        } else if (mNodeCoins <= (nMoneySupply * .70) && mNodeCoins > (nMoneySupply * .69)) {
            ret = blockValue * .462;
        } else if (mNodeCoins <= (nMoneySupply * .71) && mNodeCoins > (nMoneySupply * .7)) {
            ret = blockValue * .46;
        } else if (mNodeCoins <= (nMoneySupply * .72) && mNodeCoins > (nMoneySupply * .71)) {
            ret = blockValue * .458;
        } else if (mNodeCoins <= (nMoneySupply * .73) && mNodeCoins > (nMoneySupply * .72)) {
            ret = blockValue * .456;
        } else if (mNodeCoins <= (nMoneySupply * .74) && mNodeCoins > (nMoneySupply * .73)) {
            ret = blockValue * .454;
        } else if (mNodeCoins <= (nMoneySupply * .75) && mNodeCoins > (nMoneySupply * .74)) {
            ret = blockValue * .452;
        } else if (mNodeCoins <= (nMoneySupply * .76) && mNodeCoins > (nMoneySupply * .75)) {
            ret = blockValue * .45;
        } else if (mNodeCoins <= (nMoneySupply * .77) && mNodeCoins > (nMoneySupply * .76)) {
            ret = blockValue * .448;
        } else if (mNodeCoins <= (nMoneySupply * .78) && mNodeCoins > (nMoneySupply * .77)) {
            ret = blockValue * .446;
        } else if (mNodeCoins <= (nMoneySupply * .79) && mNodeCoins > (nMoneySupply * .78)) {
            ret = blockValue * .444;
        } else if (mNodeCoins <= (nMoneySupply * .80) && mNodeCoins > (nMoneySupply * .79)) {
            ret = blockValue * .442;
        } else if (mNodeCoins <= (nMoneySupply * .81) && mNodeCoins > (nMoneySupply * .8)) {
            ret = blockValue * .44;
        } else if (mNodeCoins <= (nMoneySupply * .82) && mNodeCoins > (nMoneySupply * .81)) {
            ret = blockValue * .438;
        } else if (mNodeCoins <= (nMoneySupply * .83) && mNodeCoins > (nMoneySupply * .82)) {
            ret = blockValue * .436;
        } else if (mNodeCoins <= (nMoneySupply * .84) && mNodeCoins > (nMoneySupply * .83)) {
            ret = blockValue * .434;
        } else if (mNodeCoins <= (nMoneySupply * .85) && mNodeCoins > (nMoneySupply * .84)) {
            ret = blockValue * .432;
        } else if (mNodeCoins <= (nMoneySupply * .86) && mNodeCoins > (nMoneySupply * .85)) {
            ret = blockValue * .43;
        } else if (mNodeCoins <= (nMoneySupply * .87) && mNodeCoins > (nMoneySupply * .86)) {
            ret = blockValue * .428;
        } else if (mNodeCoins <= (nMoneySupply * .88) && mNodeCoins > (nMoneySupply * .87)) {
            ret = blockValue * .426;
        } else if (mNodeCoins <= (nMoneySupply * .89) && mNodeCoins > (nMoneySupply * .88)) {
            ret = blockValue * .424;
        } else if (mNodeCoins <= (nMoneySupply * .90) && mNodeCoins > (nMoneySupply * .89)) {
            ret = blockValue * .422;
        } else if (mNodeCoins <= (nMoneySupply * .91) && mNodeCoins > (nMoneySupply * .90)) {
            ret = blockValue * .42;
        } else if (mNodeCoins <= (nMoneySupply * .92) && mNodeCoins > (nMoneySupply * .91)) {
            ret = blockValue * .418;
        } else if (mNodeCoins <= (nMoneySupply * .93) && mNodeCoins > (nMoneySupply * .92)) {
            ret = blockValue * .416;
        } else if (mNodeCoins <= (nMoneySupply * .94) && mNodeCoins > (nMoneySupply * .93)) {
            ret = blockValue * .414;
        } else if (mNodeCoins <= (nMoneySupply * .95) && mNodeCoins > (nMoneySupply * .94)) {
            ret = blockValue * .412;
        } else if (mNodeCoins <= (nMoneySupply * .96) && mNodeCoins > (nMoneySupply * .95)) {
            ret = blockValue * .41;
        } else if (mNodeCoins <= (nMoneySupply * .97) && mNodeCoins > (nMoneySupply * .96)) {
            ret = blockValue * .408;
        } else if (mNodeCoins <= (nMoneySupply * .98) && mNodeCoins > (nMoneySupply * .97)) {
            ret = blockValue * .404;
        } else if (mNodeCoins <= (nMoneySupply * .99) && mNodeCoins > (nMoneySupply * .98)) {
            ret = blockValue * .402;
        } else {
            ret = blockValue * .40;
        }
    }
    return ret;
}

int64_t GetMasternodePayment(int nHeight, int64_t blockValue, int nMasternodeCount)
{
    int64_t ret = 0;
    int64_t blockValueForSeeSaw = blockValue;

    if (nHeight >= Params().LAST_POW_BLOCK()) {
        return GetSeeSaw(blockValueForSeeSaw, nMasternodeCount, nHeight);
    }

    return ret;
}

void SetRingSize(int nHeight)
{
    if (chainActive.Tip() == NULL) return;
    if (nHeight == 0) {
        nHeight = chainActive.Tip()->nHeight;
    }

    // Original Ring Sizes on all networks
    MIN_RING_SIZE = 11;
    MAX_RING_SIZE = 15;

    // Ring Sizes after the Hard Fork block
    // Add any Ring Size increases as the last check
    if (nHeight >= Params().HardForkRingSize()) {
        MIN_RING_SIZE = 27;
        MAX_RING_SIZE = 32;
    }

    // Testnet Hard Forks were different
    if (Params().NetworkID() == CBaseChainParams::TESTNET) {
        if (nHeight >= Params().HardForkRingSize()) {
            MIN_RING_SIZE = 25;
            MAX_RING_SIZE = 30;
        }
        if (nHeight >= Params().HardForkRingSize2()) {
            MIN_RING_SIZE = 30;
            MAX_RING_SIZE = 32;
        }
    }

    LogPrint(BCLog::SELECTCOINS, "%s: height %d: min ring size %d, max ring size: %d\n", __func__, nHeight, MIN_RING_SIZE, MAX_RING_SIZE);
    return;
}

bool IsInitialBlockDownload()
{
    LOCK(cs_main);
    const int chainHeight = chainActive.Height();
    if (fImporting || fReindex || fVerifyingBlocks || chainHeight < Checkpoints::GetTotalBlocksEstimate())
        return true;
    static bool lockIBDState = false;
    if (lockIBDState)
        return false;
    bool state = (chainHeight < pindexBestHeader->nHeight - 24 * 6 ||
                  pindexBestHeader->GetBlockTime() < GetTime() - nMaxTipAge);
    if (!state)
        lockIBDState = true;
    return state;
}

bool fLargeWorkForkFound = false;
bool fLargeWorkInvalidChainFound = false;
CBlockIndex *pindexBestForkTip = NULL, *pindexBestForkBase = NULL;

static void AlertNotify(const std::string& strMessage, bool fThread)
{
    uiInterface.NotifyAlertChanged();
    std::string strCmd = GetArg("-alertnotify", "");
    if (strCmd.empty()) return;

    // Alert text should be plain ascii coming from a trusted source, but to
    // be safe we first strip anything not in safeChars, then add single quotes around
    // the whole string before passing it to the shell:
    std::string singleQuote("'");
    std::string safeStatus = SanitizeString(strMessage);
    safeStatus = singleQuote+safeStatus+singleQuote;
    boost::replace_all(strCmd, "%s", safeStatus);

    if (fThread)
        boost::thread t(runCommand, strCmd); // thread runs free
    else
        runCommand(strCmd);
}

bool VerifyZeroBlindCommitment(const CTxOut& out)
{
    if (out.nValue == 0) return true;
    unsigned char zeroBlind[32];
    std::vector<unsigned char> commitment;
    CWallet::CreateCommitmentWithZeroBlind(out.nValue, zeroBlind, commitment);
    return commitment == out.commitment;
}

bool VerifyDerivedAddress(const CTxOut& out, std::string stealth)
{
    CPubKey addressGenPub, pubView, pubSpend;
    bool hasPaymentID;
    uint64_t paymentID;
    if (!CWallet::DecodeStealthAddress(stealth, pubView, pubSpend, hasPaymentID, paymentID)) {
        LogPrintf("%s: Cannot decode foundational address\n", __func__);
        return false;
    }

    //reconstruct destination address from masternode address and tx private key
    CKey addressTxPriv;
    addressTxPriv.Set(&(out.txPriv[0]), &(out.txPriv[0]) + 32, true);
    CPubKey foundationTxPub = addressTxPriv.GetPubKey();
    CPubKey origin(out.txPub.begin(), out.txPub.end());
    if (foundationTxPub != origin) return false;
    CWallet::ComputeStealthDestination(addressTxPriv, pubView, pubSpend, addressGenPub);
    CScript foundationalScript = GetScriptForDestination(addressGenPub);
    return foundationalScript == out.scriptPubKey;
}

void CheckForkWarningConditions()
{
    AssertLockHeld(cs_main);
    // Before we get past initial download, we cannot reliably alert about forks
    // (we assume we don't get stuck on a fork before the last checkpoint)
    if (IsInitialBlockDownload())
        return;

    // If our best fork is no longer within 72 blocks (+/- 3 hours if no one mines it)
    // of our head, drop it
    if (pindexBestForkTip && chainActive.Height() - pindexBestForkTip->nHeight >= 72)
        pindexBestForkTip = NULL;

    if (pindexBestForkTip || (pindexBestInvalid && pindexBestInvalid->nChainWork > chainActive.Tip()->nChainWork +
                                                                                       (GetBlockProof(*chainActive.Tip()) *
                                                                                           6))) {
        if (!fLargeWorkForkFound && pindexBestForkBase) {
            if (pindexBestForkBase->phashBlock) {
                std::string warning = std::string("'Warning: Large-work fork detected, forking after block ") +
                                      pindexBestForkBase->phashBlock->ToString() + std::string("'");
                AlertNotify(warning, true);
            }
        }
        if (pindexBestForkTip && pindexBestForkBase) {
            if (pindexBestForkBase->phashBlock) {
                LogPrintf(
                    "CheckForkWarningConditions: Warning: Large valid fork found\n  forking the chain at height %d (%s)\n  lasting to height %d (%s).\nChain state database corruption likely.\n",
                    pindexBestForkBase->nHeight, pindexBestForkBase->phashBlock->ToString(),
                    pindexBestForkTip->nHeight, pindexBestForkTip->phashBlock->ToString());
                fLargeWorkForkFound = true;
            }
        } else {
            LogPrintf(
                "CheckForkWarningConditions: Warning: Found invalid chain at least ~6 blocks longer than our best chain.\nChain state database corruption likely.\n");
            fLargeWorkInvalidChainFound = true;
        }
    } else {
        fLargeWorkForkFound = false;
        fLargeWorkInvalidChainFound = false;
    }
}

void CheckForkWarningConditionsOnNewFork(CBlockIndex* pindexNewForkTip)
{
    AssertLockHeld(cs_main);
    // If we are on a fork that is sufficiently large, set a warning flag
    CBlockIndex* pfork = pindexNewForkTip;
    CBlockIndex* plonger = chainActive.Tip();
    while (pfork && pfork != plonger) {
        while (plonger && plonger->nHeight > pfork->nHeight)
            plonger = plonger->pprev;
        if (pfork == plonger)
            break;
        pfork = pfork->pprev;
    }

    // We define a condition which we should warn the user about as a fork of at least 7 blocks
    // who's tip is within 72 blocks (+/- 3 hours if no one mines it) of ours
    // or a chain that is entirely longer than ours and invalid (note that this should be detected by both)
    // We use 7 blocks rather arbitrarily as it represents just under 10% of sustained network
    // hash rate operating on the fork.
    // We define it this way because it allows us to only store the highest fork tip (+ base) which meets
    // the 7-block condition and from this always have the most-likely-to-cause-warning fork
    if (pfork &&
        (!pindexBestForkTip || (pindexBestForkTip && pindexNewForkTip->nHeight > pindexBestForkTip->nHeight)) &&
        pindexNewForkTip->nChainWork - pfork->nChainWork > (GetBlockProof(*pfork) * 7) &&
        chainActive.Height() - pindexNewForkTip->nHeight < 72) {
        pindexBestForkTip = pindexNewForkTip;
        pindexBestForkBase = pfork;
    }

    CheckForkWarningConditions();
}

// Requires cs_main.
void Misbehaving(NodeId pnode, int howmuch) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    if (howmuch == 0)
        return;

    CNodeState* state = State(pnode);
    if (state == NULL)
        return;

    CAddress cAddr(state->address, NODE_NETWORK);
    if (CNode::IsWhitelistedRange(cAddr)) return;

    state->nMisbehavior += howmuch;
    int banscore = GetArg("-banscore", 100);
    if (state->nMisbehavior >= banscore && state->nMisbehavior - howmuch < banscore) {
        LogPrintf("Misbehaving: %s (%d -> %d) BAN THRESHOLD EXCEEDED\n", state->name, state->nMisbehavior - howmuch,
            state->nMisbehavior);
        state->fShouldBan = true;
    } else
        LogPrintf("Misbehaving: %s (%d -> %d)\n", state->name, state->nMisbehavior - howmuch, state->nMisbehavior);
}

void static InvalidChainFound(CBlockIndex* pindexNew)
{
    if (!pindexBestInvalid || pindexNew->nChainWork > pindexBestInvalid->nChainWork)
        pindexBestInvalid = pindexNew;

    LogPrintf("InvalidChainFound: invalid block=%s  height=%d  log2_work=%.8g  date=%s\n",
        pindexNew->GetBlockHash().ToString(), pindexNew->nHeight,
        log(pindexNew->nChainWork.getdouble()) / log(2.0), DateTimeStrFormat("%Y-%m-%d %H:%M:%S", pindexNew->GetBlockTime()));

    const CBlockIndex* pChainTip = mapBlockIndex[chainActive.Tip()->GetBlockHash()];
    LogPrintf("InvalidChainFound:  current best=%s  height=%d  log2_work=%.8g  date=%s\n",
        pChainTip->GetBlockHash().GetHex(), pChainTip->nHeight, log(pChainTip->nChainWork.getdouble()) / log(2.0),
        DateTimeStrFormat("%Y-%m-%d %H:%M:%S", pChainTip->GetBlockTime()));
    CheckForkWarningConditions();
}

void static InvalidBlockFound(CBlockIndex* pindex, const CValidationState& state)
{
    int nDoS = 0;
    if (state.IsInvalid(nDoS)) {
        std::map<uint256, NodeId>::iterator it = mapBlockSource.find(pindex->GetBlockHash());
        if (it != mapBlockSource.end() && State(it->second)) {
            CBlockReject reject = {state.GetRejectCode(), state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH),
                pindex->GetBlockHash()};
            State(it->second)->rejects.push_back(reject);
            if (nDoS > 0) {
                LOCK(cs_main);
                Misbehaving(it->second, nDoS);
            }
        }
    }
    if (!state.CorruptionPossible()) {
        pindex->nStatus |= BLOCK_FAILED_VALID;
        setDirtyBlockIndex.insert(pindex);
        setBlockIndexCandidates.erase(pindex);
        InvalidChainFound(pindex);
    }
}

void UpdateCoins(const CTransaction& tx, CCoinsViewCache& inputs, CTxUndo& txundo, int nHeight)
{
    // mark inputs spent
    if (!tx.IsCoinAudit() && !tx.IsCoinBase() && tx.IsCoinStake()) {
        txundo.vprevout.reserve(tx.vin.size());
        for (const CTxIn& txin : tx.vin) {
            txundo.vprevout.push_back(CTxInUndo());
            bool ret = inputs.ModifyCoins(txin.prevout.hash)->Spend(txin.prevout, txundo.vprevout.back());
            //assert(ret);
        }
    }

    // add outputs
    inputs.ModifyCoins(tx.GetHash())->FromTx(tx, nHeight);
}

bool CScriptCheck::operator()()
{
    const CScript& scriptSig = ptxTo->vin[nIn].scriptSig;
    if (!VerifyScript(scriptSig, scriptPubKey, nFlags, CachingTransactionSignatureChecker(ptxTo, nIn, cacheStore),
            &error)) {
        if (error != SCRIPT_ERR_EVAL_FALSE) {
            return ::error("CScriptCheck(): %s:%d VerifySignature failed: %s", ptxTo->GetHash().ToString(), nIn,
                ScriptErrorString(error));
        }
    }
    return true;
}

std::map<COutPoint, COutPoint> mapInvalidOutPoints;

// Populate global map (mapInvalidOutPoints) of invalid/fraudulent OutPoints that are banned from being used on the chain.
CAmount nFilteredThroughBittrex = 0;
bool fListPopulatedAfterLock = false;

bool ValidOutPoint(const COutPoint out, int nHeight)
{
    bool isInvalid = invalid_out::ContainsOutPoint(out);

    return !isInvalid;
}

CAmount GetInvalidUTXOValue()
{
    CAmount nValue = 0;
    for (auto it : mapInvalidOutPoints) {
        const COutPoint out = it.first;
        bool fSpent = false;
        CCoinsViewCache cache(pcoinsTip);
        const CCoins* coins = cache.AccessCoins(out.hash);
        if (!coins || !coins->IsAvailable(out.n))
            fSpent = true;

        if (!fSpent)
            nValue += coins->vout[out.n].nValue;
    }

    return nValue;
}

bool CheckInputs(const CTransaction& tx, CValidationState& state, const CCoinsViewCache& inputs, bool fScriptChecks, unsigned int flags, bool cacheStore, std::vector<CScriptCheck>* pvChecks)
{
    if (!tx.IsCoinBase()) {
        if (pvChecks)
            pvChecks->reserve(tx.vin.size());

        // This doesn't trigger the DoS code on purpose; if it did, it would make it easier
        // for an attacker to attempt to split the network.
        if (!CheckHaveInputs(inputs, tx))
            return state.Invalid(error("CheckInputs() : %s inputs unavailable", tx.GetHash().ToString()));

        // While checking, GetBestBlock() refers to the parent block.
        // This is also true for mempool checks.
        CBlockIndex* pindexPrev = mapBlockIndex.find(inputs.GetBestBlock())->second;
        int nSpendHeight = pindexPrev->nHeight + 1;
        if (tx.IsCoinStake()) {
            for (unsigned int i = 0; i < tx.vin.size(); i++) {
                const COutPoint& prevout = tx.vin[i].prevout;
                CTransaction prev;
                uint256 bh;
                if (!GetTransaction(prevout.hash, prev, bh, true)) {
                    return state.Invalid(
                        error("CheckInputs() : Inputs not available"),
                        REJECT_INVALID, "bad-txns");
                }

                // If prev is coinbase, check that it's matured
                if (prev.IsCoinBase() || prev.IsCoinStake()) {
                    if (nSpendHeight - mapBlockIndex[bh]->nHeight < Params().COINBASE_MATURITY())
                        return state.Invalid(
                            error("CheckInputs() : tried to spend coinbase at depth %d, coinstake=%d",
                                nSpendHeight - mapBlockIndex[bh]->nHeight, prev.IsCoinStake()),
                            REJECT_INVALID, "bad-txns-premature-spend-of-coinbase");
                }
            }
        }

        // The first loop above does all the inexpensive checks.
        // Only if ALL inputs pass do we perform expensive ECDSA signature checks.
        // Helps prevent CPU exhaustion attacks.

        // Skip ECDSA signature verification when connecting blocks
        // before the last block chain checkpoint. This is safe because block merkle hashes are
        // still computed and checked, and any change will be caught at the next checkpoint.
        //standard transaction does not have script
        if (fScriptChecks && tx.IsCoinStake()) {
            for (unsigned int i = 0; i < tx.vin.size(); i++) {
                const COutPoint& prevout = tx.vin[i].prevout;
                CTransaction prev;
                uint256 bh;
                if (!GetTransaction(prevout.hash, prev, bh, true)) {
                    return state.Invalid(
                        error("CheckInputs() : Inputs not available"),
                        REJECT_INVALID, "bad-txns");
                }
                CCoins coins(prev, mapBlockIndex[bh]->nHeight);

                // Verify signature
                CScriptCheck check(coins, tx, i, flags, cacheStore);
                if (pvChecks) {
                    pvChecks->push_back(CScriptCheck());
                    check.swap(pvChecks->back());
                } else if (!check()) {
                    if (flags & STANDARD_NOT_MANDATORY_VERIFY_FLAGS) {
                        // Check whether the failure was caused by a
                        // non-mandatory script verification check, such as
                        // non-standard DER encodings or non-null dummy
                        // arguments; if so, don't trigger DoS protection to
                        // avoid splitting the network between upgraded and
                        // non-upgraded nodes.
                        CScriptCheck check(coins, tx, i,
                            flags & ~STANDARD_NOT_MANDATORY_VERIFY_FLAGS, cacheStore);
                        if (check())
                            return state.Invalid(false, REJECT_NONSTANDARD,
                                strprintf("non-mandatory-script-verify-flag (%s)",
                                    ScriptErrorString(check.GetScriptError())));
                    }
                    // Failures of other flags indicate a transaction that is
                    // invalid in new blocks, e.g. a invalid P2SH. We DoS ban
                    // such nodes as they are not following the protocol. That
                    // said during an upgrade careful thought should be taken
                    // as to the correct behavior - we may want to continue
                    // peering with non-upgraded nodes even after a soft-fork
                    // super-majority vote has passed.
                    return state.DoS(100, false, REJECT_INVALID, strprintf("mandatory-script-verify-flag-failed (%s)", ScriptErrorString(check.GetScriptError())));
                }
            }
        }
    }

    return true;
}

/** Abort with a message */
static bool AbortNode(const std::string& strMessage, const std::string& userMessage="")
{
    strMiscWarning = strMessage;
    LogPrintf("*** %s\n", strMessage);
    uiInterface.ThreadSafeMessageBox(
        userMessage.empty() ? _("Error: A fatal internal error occured, see debug.log for details") : userMessage,
        "", CClientUIInterface::MSG_ERROR);
    StartShutdown();
    return false;
}

static bool AbortNode(CValidationState& state, const std::string& strMessage, const std::string& userMessage="")
{
    AbortNode(strMessage, userMessage);
    return state.Error(strMessage);
}

bool DisconnectBlock(CBlock& block, CValidationState& state, CBlockIndex* pindex, CCoinsViewCache& view, bool* pfClean)
{
    if (pindex->GetBlockHash() != view.GetBestBlock())
        LogPrintf("%s : pindex=%s view=%s\n", __func__, pindex->GetBlockHash().GetHex(), view.GetBestBlock().GetHex());
    assert(pindex->GetBlockHash() == view.GetBestBlock());

    if (pfClean)
        *pfClean = false;

    bool fClean = true;

    CBlockUndo blockUndo;
    CDiskBlockPos pos = pindex->GetUndoPos();
    if (pos.IsNull())
        return error("DisconnectBlock() : no undo data available");
    if (!blockUndo.ReadFromDisk(pos, pindex->pprev->GetBlockHash()))
        return error("DisconnectBlock() : failure reading undo data");

    if (blockUndo.vtxundo.size() + 1 != block.vtx.size())
        return error("DisconnectBlock() : block and undo data inconsistent");

    // undo transactions in reverse order
    for (int i = block.vtx.size() - 1; i >= 0; i--) {
        const CTransaction& tx = block.vtx[i];

        uint256 hash = tx.GetHash();

        // Check that all outputs are available and match the outputs in the block itself
        // exactly. Note that transactions with only provably unspendable outputs won't
        // have outputs available even in the block itself, so we handle that case
        // specially with outsEmpty.
        {
            CCoins outsEmpty;
            CCoinsModifier outs = view.ModifyCoins(hash);
            outs->ClearUnspendable();

            CCoins outsBlock(tx, pindex->nHeight);
            // The CCoins serialization does not serialize negative numbers.
            // No network rules currently depend on the version here, so an inconsistency is harmless
            // but it must be corrected before txout nversion ever influences a network rule.
            if (outsBlock.nVersion < 0)
                outs->nVersion = outsBlock.nVersion;
            if (*outs != outsBlock)
                fClean = fClean && error("DisconnectBlock() : added transaction mismatch? database corrupted");

            // remove outputs
            outs->Clear();
        }

        // restore inputs
        if (!tx.IsCoinBase() && tx.IsCoinStake()) { // not coinbases because they dont have traditional inputs
            const CTxUndo& txundo = blockUndo.vtxundo[i - 1];
            if (txundo.vprevout.size() != tx.vin.size())
                return error(
                    "DisconnectBlock() : transaction and undo data inconsistent - txundo.vprevout.siz=%d tx.vin.siz=%d",
                    txundo.vprevout.size(), tx.vin.size());
            for (unsigned int j = tx.vin.size(); j-- > 0;) {
                const COutPoint& out = tx.vin[j].prevout;
                const CTxInUndo& undo = txundo.vprevout[j];
                CCoinsModifier coins = view.ModifyCoins(out.hash);
                if (undo.nHeight != 0) {
                    // undo data contains height: this is the last output of the prevout tx being spent
                    if (!coins->IsPruned())
                        fClean = fClean && error("DisconnectBlock() : undo data overwriting existing transaction");
                    coins->Clear();
                    coins->fCoinBase = undo.fCoinBase;
                    coins->nHeight = undo.nHeight;
                    coins->nVersion = undo.nVersion;
                } else {
                    if (coins->IsPruned())
                        fClean = fClean && error("DisconnectBlock() : undo data adding output to missing transaction");
                }
                if (coins->IsAvailable(out.n))
                    fClean = fClean && error("DisconnectBlock() : undo data overwriting existing output");
                if (coins->vout.size() < out.n + 1)
                    coins->vout.resize(out.n + 1);
                coins->vout[out.n] = undo.txout;
            }
        }
    }

    // move best block pointer to prevout block
    view.SetBestBlock(pindex->pprev->GetBlockHash());

    if (pfClean) {
        *pfClean = fClean;
        return true;
    } else {
        return fClean;
    }
}

void static FlushBlockFile(bool fFinalize = false)
{
    LOCK(cs_LastBlockFile);

    CDiskBlockPos posOld(nLastBlockFile, 0);

    FILE* fileOld = OpenBlockFile(posOld);
    if (fileOld) {
        if (fFinalize)
            TruncateFile(fileOld, vinfoBlockFile[nLastBlockFile].nSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }

    fileOld = OpenUndoFile(posOld);
    if (fileOld) {
        if (fFinalize)
            TruncateFile(fileOld, vinfoBlockFile[nLastBlockFile].nUndoSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }
}

bool FindUndoPos(CValidationState& state, int nFile, CDiskBlockPos& pos, unsigned int nAddSize);

static CCheckQueue<CScriptCheck> scriptcheckqueue(128);

void ThreadScriptCheck()
{
    util::ThreadRename("prcycoin-scriptch");
    scriptcheckqueue.Thread();
}

bool RecalculatePRCYSupply(int nHeightStart)
{
    const int chainHeight = chainActive.Height();
    if (nHeightStart > chainHeight)
        return false;

    CBlockIndex* pindex = chainActive[nHeightStart];
    CAmount nSupplyPrev = pindex->pprev->nMoneySupply;

    uiInterface.ShowProgress(_("Recalculating PRCY supply..."), 0);
    while (true) {
        if (pindex->nHeight % 1000 == 0) {
            LogPrintf("%s : block %d...\n", __func__, pindex->nHeight);
            int percent = std::max(1, std::min(99, (int)((double)((pindex->nHeight - nHeightStart) * 100) / (chainHeight - nHeightStart))));
            uiInterface.ShowProgress(_("Recalculating PRCY supply..."), percent);
        }
        CBlock block;
        assert(ReadBlockFromDisk(block, pindex));

        CAmount nValueIn = 0;
        CAmount nValueOut = 0;
        CAmount nFees = 0;
        for (const CTransaction& tx : block.vtx) {
            nFees += tx.nTxFee;
            if (tx.IsCoinStake()) {
                for (unsigned int i = 0; i < tx.vin.size(); i++) {
                    CAmount nTemp; // = txPrev.vout[prevout.n].nValue;
                    uint256 hashBlock;
                    CTransaction txPrev;
                    GetTransaction(tx.vin[i].prevout.hash, txPrev, hashBlock, true);
                    const CTxOut& out = txPrev.vout[tx.vin[i].prevout.n];
                    if (out.nValue > 0) {
                        //UTXO created by coinbase/coin audit/coinstake transaction
                        if (!VerifyZeroBlindCommitment(out)) {
                            throw std::runtime_error("Commitment for coinstake not correct: failed to verify blind commitment");
                        }
                        nValueIn += out.nValue;
                    } else {
                        uint256 val = out.maskValue.amount;
                        uint256 mask = out.maskValue.mask;
                        CKey decodedMask;
                        CPubKey sharedSec;
                        sharedSec.Set(tx.vin[i].encryptionKey.begin(), tx.vin[i].encryptionKey.begin() + 33);
                        ECDHInfo::Decode(mask.begin(), val.begin(), sharedSec, decodedMask, nTemp);
                        //Verify commitment
                        std::vector<unsigned char> commitment;
                        CWallet::CreateCommitment(decodedMask.begin(), nTemp, commitment);
                        if (commitment != out.commitment) {
                            throw std::runtime_error("Commitment for coinstake not correct");
                        }
                        nValueIn += nTemp;
                    }
                }
            }

            for (unsigned int i = 0; i < tx.vout.size(); i++) {
                if (i == 0 && tx.IsCoinStake())
                    continue;

                nValueOut += tx.vout[i].nValue;
            }
        }

        // Rewrite money supply
        pindex->nMoneySupply = nSupplyPrev + nValueOut - nValueIn - nFees;
        nSupplyPrev = pindex->nMoneySupply;

        assert(pblocktree->WriteBlockIndex(CDiskBlockIndex(pindex)));

        if (pindex->nHeight < chainHeight)
            pindex = chainActive.Next(pindex);
        else
            break;
    }
    return true;
}

static int64_t nTimeVerify = 0;
static int64_t nTimeConnect = 0;
static int64_t nTimeIndex = 0;
static int64_t nTimeCallbacks = 0;
static int64_t nTimeTotal = 0;

bool ConnectBlock(const CBlock& block, CValidationState& state, CBlockIndex* pindex, CCoinsViewCache& view, bool fJustCheck, bool fAlreadyChecked)
{
    AssertLockHeld(cs_main);
    // Check it again in case a previous version let a bad block in
    if (!fAlreadyChecked && !CheckBlock(block, state, !fJustCheck, !fJustCheck))
        return false;

    //Check PoA block time
    if (block.IsPoABlockByVersion() && !CheckPoAblockTime(block)) {
        return state.Invalid(error("ConnectBlock(): Time elapsed between two PoA blocks is too short"),
            REJECT_INVALID, "time-too-new");
    }
    //Check PoA block not auditing PoS blocks audited by its previous PoA block
    if (block.IsPoABlockByVersion() && !CheckPoABlockNotAuditingOverlap(block)) {
        return state.Invalid(error("ConnectBlock(): PoA block auditing PoS blocks previously audited by its parent"),
            REJECT_INVALID, "overlap-audit");
    }

    /**
     * @Audit ConnectBlock
     */
    if (!fVerifyingBlocks && block.IsProofOfAudit()) {
        //Check PoA consensus rules
        if (!CheckPoAContainRecentHash(block)) {
            return state.DoS(100, error("ConnectBlock(): PoA block should contain only non-audited recent PoS blocks"),
                REJECT_INVALID, "blocks-already-audited");
        }

        if (!CheckNumberOfAuditedPoSBlocks(block, pindex)) {
            return state.DoS(100, error("ConnectBlock(): A PoA block should audit at least 59 PoS blocks and no more than 120 PoS blocks (65 max after block 169869)"),
                             REJECT_INVALID, "incorrect-number-audited-blocks");
        }

        if (!CheckPoABlockNotContainingPoABlockInfo(block, pindex)) {
            return state.DoS(100, error("ConnectBlock(): A PoA block should not audit any existing PoA blocks"),
                             REJECT_INVALID, "auditing-poa-block");
        }

        if (!CheckPoABlockRewardAmount(block, pindex)) {
            return state.DoS(100, error("ConnectBlock(): This PoA block reward does not match the value it should"),
                             REJECT_INVALID, "incorrect-reward");
        }

        if (!CheckPoABlockPaddingAmount(block, pindex)) {
            return state.DoS(100, error("ConnectBlock(): This PoA block does not have the correct padding"),
                             REJECT_INVALID, "incorrect-padding");
        }

        if (block.GetBlockTime() >= GetAdjustedTime() + 2 * 60) {
            return state.DoS(100, error("ConnectBlock(): A PoA block should not be in the future"),
                             REJECT_INVALID, "time-in-future");
        }
    }

    // verify that the view's current state corresponds to the previous block
    uint256 hashPrevBlock = pindex->pprev == NULL ? UINT256_ZERO : pindex->pprev->GetBlockHash();
    if (hashPrevBlock != view.GetBestBlock())
        LogPrintf("%s: hashPrev=%s view=%s\n", __func__, hashPrevBlock.ToString().c_str(),
            view.GetBestBlock().ToString().c_str());
    assert(hashPrevBlock == view.GetBestBlock());
    // Special case for the genesis block, skipping connection of its transactions
    // (its coinbase is unspendable)
    if (block.GetHash() == Params().HashGenesisBlock()) {
        view.SetBestBlock(pindex->GetBlockHash());
        return true;
    }
    int nHeight = pindex->nHeight;
    if (nHeight <= Params().LAST_POW_BLOCK() && block.IsProofOfStake())
        return state.DoS(100, error("ConnectBlock() : PoS period not active"),
            REJECT_INVALID, "PoS-early");

    if (nHeight > Params().LAST_POW_BLOCK() && block.IsProofOfWork())
        return state.DoS(100, error("ConnectBlock() : PoW period ended"),
            REJECT_INVALID, "PoW-ended");

    bool fScriptChecks = nHeight >= Checkpoints::GetTotalBlocksEstimate();

    // If scripts won't be checked anyways, don't bother seeing if CLTV is activated
    bool fCLTVIsActivated = false;
    if (fScriptChecks && pindex->pprev) {
        fCLTVIsActivated = pindex->pprev->nHeight >= Params().BIP65ActivationHeight();
    }

    CCheckQueueControl<CScriptCheck> control(fScriptChecks && nScriptCheckThreads ? &scriptcheckqueue : nullptr);

    int64_t nTimeStart = GetTimeMicros();
    CAmount nFees = 0;
    int nInputs = 0;
    unsigned int nSigOps = 0;
    CDiskTxPos pos(pindex->GetBlockPos(), GetSizeOfCompactSize(block.vtx.size()));
    std::vector<std::pair<uint256, CDiskTxPos> > vPos;
    vPos.reserve(block.vtx.size());
    CBlockUndo blockundo;
    blockundo.vtxundo.reserve(block.vtx.size() - 1);
    CAmount nValueOut = 0;
    CAmount nValueIn = 0;
    unsigned int nMaxBlockSigOps = MAX_BLOCK_SIGOPS_CURRENT;
    for (unsigned int i = 0; i < block.vtx.size(); i++) {
        const CTransaction& tx = block.vtx[i];
        nInputs += tx.vin.size();
        nSigOps += GetLegacySigOpCount(tx);
        if (!block.IsPoABlockByVersion() && nSigOps > nMaxBlockSigOps)
            return state.DoS(100, error("ConnectBlock() : too many sigops"),
                REJECT_INVALID, "bad-blk-sigops");

        if (!block.IsPoABlockByVersion() && !tx.IsCoinBase()) {
            if (!tx.IsCoinStake()) {
                if (!tx.IsCoinAudit()) {
                    if (!VerifyRingSignatureWithTxFee(tx, pindex))
                        return state.DoS(100, error("ConnectBlock() : Ring Signature check for transaction %s failed", tx.GetHash().ToString()),
                            REJECT_INVALID, "bad-ring-signature");
                    if (!VerifyBulletProofAggregate(tx))
                        return state.DoS(100, error("ConnectBlock() : Bulletproof check for transaction %s failed", tx.GetHash().ToString()),
                            REJECT_INVALID, "bad-bulletproof");
                }
            }

            // Check that the inputs are not marked as invalid/fraudulent
            uint256 bh = pindex->GetBlockHash();
            for (const CTxIn& in : tx.vin) {
                const CKeyImage& keyImage = in.keyImage;
                std::string kh = keyImage.GetHex();
                if (IsSpentKeyImage(kh, bh)) {
                    //remove transaction from the pool?
                    return state.Invalid(error("ConnectBlock() : key image already spent"),
                        REJECT_DUPLICATE, "bad-txns-inputs-spent");
                }
                pblocktree->WriteKeyImage(keyImage.GetHex(), bh);
                if (pwalletMain != NULL && !pwalletMain->IsLocked()) {
                    if (pwalletMain->GetDebit(in, ISMINE_ALL)) {
                        pwalletMain->keyImagesSpends[keyImage.GetHex()] = true;
                    }
                    pwalletMain->pendingKeyImages.remove(keyImage.GetHex());
                }
                if (!ValidOutPoint(in.prevout) && nHeight > Params().FixChecks()) {
                    return state.DoS(100, error("%s : tried to spend invalid input %s in tx %s", __func__, in.prevout.ToString(),
                                  tx.GetHash().GetHex()), REJECT_INVALID, "bad-txns-invalid-inputs");
                }
            }

            for (unsigned int i = 0; i < tx.vin.size(); i++) {
                if (tx.IsCoinBase()) continue;
                //check output and decoys
                std::vector<COutPoint> alldecoys = tx.vin[i].decoys;

                alldecoys.push_back(tx.vin[i].prevout);
                for (size_t j = 0; j < alldecoys.size(); j++) {
                    CTransaction prev;
                    uint256 bh;
                    if (!GetTransaction(alldecoys[j].hash, prev, bh, true)) {
                        return false;
                    }
                    if (mapBlockIndex.count(bh) < 1) return false;
                    if (!ValidOutPoint(alldecoys[j]) && nHeight > Params().FixChecks()) {
                        return state.DoS(100, error("%s : tried to spend invalid decoy %s in tx %s", __func__, alldecoys[j].ToString(),
                                                    tx.GetHash().GetHex()), REJECT_INVALID, "bad-txns-invalid-inputs");
                    }
                }
            }

            if (!tx.IsCoinStake() && tx.nTxFee < MIN_FEE && nHeight >= Params().HardFork())
                return state.Invalid(error("ConnectBlock() : Fee below Minimum. Network spam detected."),
                    REJECT_INVALID, "bad-txns-low-fee");
            if (!tx.IsCoinStake())
                nFees += tx.nTxFee;
            CAmount valTemp = GetValueIn(view, tx);
            nValueIn += valTemp;

            std::vector<CScriptCheck> vChecks;
			unsigned int flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_DERSIG;
            if (fCLTVIsActivated)
                flags |= SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;

            bool fCacheResults = fJustCheck; /* Don't cache results if we're actually connecting blocks (still consult the cache, though) */
            if (!CheckInputs(tx, state, view, fScriptChecks, flags, fCacheResults, nScriptCheckThreads ? &vChecks : NULL))
                return false;
            control.Add(vChecks);
        }
        nValueOut += tx.GetValueOut();

        CTxUndo undoDummy;
        if (i > 0) {
            blockundo.vtxundo.emplace_back();
        }
        UpdateCoins(tx, view, i == 0 ? undoDummy : blockundo.vtxundo.back(), nHeight);

        vPos.emplace_back(tx.GetHash(), pos);
        pos.nTxOffset += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);
    }

    if (block.IsProofOfStake()) {
        const CAmount minStakingAmount = Params().MinimumStakeAmount();
        const CTransaction coinstake = block.vtx[1];
        size_t numUTXO = coinstake.vout.size();
        if (mapBlockIndex.count(block.hashPrevBlock) < 1) {
            return state.DoS(100, error("ConnectBlock() : Previous block not found, received block %s, previous %s, current tip %s", block.GetHash().GetHex(), block.hashPrevBlock.GetHex(), chainActive.Tip()->GetBlockHash().GetHex()));
        }
        CAmount blockValue = GetBlockValue(mapBlockIndex[block.hashPrevBlock]->nHeight);
        const CTxOut& mnOut = coinstake.vout[numUTXO - 1];
        std::string mnsa(mnOut.masternodeStealthAddress.begin(), mnOut.masternodeStealthAddress.end());
        if (!VerifyDerivedAddress(mnOut, mnsa))
            return state.DoS(100, error("ConnectBlock() : Incorrect derived address for masternode rewards"));

        if (nHeight >= Params().HardFork() && nValueIn < minStakingAmount)
            return state.DoS(100, error("ConnectBlock() : amount (%d) not allowed for staking. Min amount: %d",
                    __func__, nValueIn, minStakingAmount), REJECT_INVALID, "bad-txns-stake");

        if (coinstake.vin.size() > 1 && nHeight > Params().FixChecks())
            return state.DoS(100, error("%s : multiple stake inputs not allowed", __func__), REJECT_INVALID, "bad-txns-stake");
    }

    // track money supply and mint amount info
    CAmount nMoneySupplyPrev = pindex->pprev ? pindex->pprev->nMoneySupply : 0;
    pindex->nMoneySupply = nMoneySupplyPrev + nValueOut - nValueIn - nFees;
    //LogPrintf("%s: nValueOut=%d, nValueIn=%d, nMoneySupplyPrev=%d, pindex->nMoneySupply=%d, nFees=%d", __func__, nValueOut, nValueIn, nMoneySupplyPrev, pindex->nMoneySupply, nFees);
    pindex->nMint = pindex->nMoneySupply - nMoneySupplyPrev + nFees;

    int64_t nTime1 = GetTimeMicros();
    nTimeConnect += nTime1 - nTimeStart;
    LogPrint(BCLog::BENCH, "      - Connect %u transactions: %.2fms (%.3fms/tx, %.3fms/txin) [%.2fs]\n",
        (unsigned)block.vtx.size(), 0.001 * (nTime1 - nTimeStart),
        0.001 * (nTime1 - nTimeStart) / block.vtx.size(),
        nInputs <= 1 ? 0 : 0.001 * (nTime1 - nTimeStart) / (nInputs - 1), nTimeConnect * 0.000001);

    //PoW phase redistributed fees to miner. PoS stage destroys fees.
    CAmount nExpectedMint = GetBlockValue(pindex->pprev->nHeight);
    nExpectedMint += nFees;

    //Check that the block does not overmint
    if (!block.IsPoABlockByVersion() && !IsBlockValueValid(pindex->nHeight, nExpectedMint, pindex->nMint)) {
        return state.DoS(100, error("ConnectBlock() : reward pays too much (actual=%s vs limit=%s)",
                FormatMoney(pindex->nMint), FormatMoney(nExpectedMint)), REJECT_INVALID, "bad-cb-amount");
    }

    if (!control.Wait())
        return state.DoS(100, error("%s: CheckQueue failed", __func__), REJECT_INVALID, "block-validation-failed");
    int64_t nTime2 = GetTimeMicros();
    nTimeVerify += nTime2 - nTimeStart;
    LogPrint(BCLog::BENCH, "    - Verify %u txins: %.2fms (%.3fms/txin) [%.2fs]\n", nInputs - 1,
        0.001 * (nTime2 - nTimeStart), nInputs <= 1 ? 0 : 0.001 * (nTime2 - nTimeStart) / (nInputs - 1),
        nTimeVerify * 0.000001);

    //IMPORTANT NOTE: Nothing before this point should actually store to disk (or even memory)
    if (fJustCheck)
        return true;

    // Write undo information to disk
    if (pindex->GetUndoPos().IsNull() || !pindex->IsValid(BLOCK_VALID_SCRIPTS)) {
        if (pindex->GetUndoPos().IsNull()) {
            CDiskBlockPos diskPosBlock;
            if (!FindUndoPos(state, pindex->nFile, diskPosBlock, ::GetSerializeSize(blockundo, SER_DISK, CLIENT_VERSION) + 40))
                return error("ConnectBlock() : FindUndoPos failed");
            if (!blockundo.WriteToDisk(diskPosBlock, pindex->pprev->GetBlockHash()))
                return AbortNode(state, "Failed to write undo data");

            // update nUndoPos in block index
            pindex->nUndoPos = diskPosBlock.nPos;
            pindex->nStatus |= BLOCK_HAVE_UNDO;
        }

        pindex->RaiseValidity(BLOCK_VALID_SCRIPTS);
        setDirtyBlockIndex.insert(pindex);
    }

    if (fTxIndex)
        if (!pblocktree->WriteTxIndex(vPos))
            return AbortNode(state, "Failed to write transaction index");

    // add this block to the view's block chain
    view.SetBestBlock(pindex->GetBlockHash());

    int64_t nTime3 = GetTimeMicros();
    nTimeIndex += nTime3 - nTime2;
    LogPrint(BCLog::BENCH, "    - Index writing: %.2fms [%.2fs]\n", 0.001 * (nTime3 - nTime2), nTimeIndex * 0.000001);

    // Watch for changes to the previous coinbase transaction.
    static uint256 hashPrevBestCoinBase;
    GetMainSignals().UpdatedTransaction(hashPrevBestCoinBase);
    hashPrevBestCoinBase = block.vtx[0].GetHash();

    int64_t nTime4 = GetTimeMicros();
    nTimeCallbacks += nTime4 - nTime3;
    LogPrint(BCLog::BENCH, "    - Callbacks: %.2fms [%.2fs]\n", 0.001 * (nTime4 - nTime3), nTimeCallbacks * 0.000001);

    return true;
}

enum FlushStateMode {
    FLUSH_STATE_IF_NEEDED,
    FLUSH_STATE_PERIODIC,
    FLUSH_STATE_ALWAYS
};

/**
 * Update the on-disk chain state.
 * The caches and indexes are flushed if either they're too large, forceWrite is set, or
 * fast is not set and it's been a while since the last write.
 */
bool static FlushStateToDisk(CValidationState& state, FlushStateMode mode)
{
    LOCK(cs_main);
    static int64_t nLastWrite = 0;
    static int64_t nLastFlush = 0;
    static int64_t nLastSetChain = 0;
    try {
        int64_t nNow = GetTimeMicros();
        // Avoid writing/flushing immediately after startup.
        if (nLastWrite == 0) {
            nLastWrite = nNow;
        }
        if (nLastFlush == 0) {
            nLastFlush = nNow;
        }
        if (nLastSetChain == 0) {
            nLastSetChain = nNow;
        }
        size_t cacheSize = pcoinsTip->DynamicMemoryUsage();
        // The cache is large and close to the limit, but we have time now (not in the middle of a block processing).
        bool fCacheLarge = mode == FLUSH_STATE_PERIODIC && cacheSize * (10.0/9) > nCoinCacheUsage;
        // The cache is over the limit, we have to write now.
        bool fCacheCritical = mode == FLUSH_STATE_IF_NEEDED && cacheSize > nCoinCacheUsage;
        // It's been a while since we wrote the block index to disk. Do this frequently, so we don't need to redownload after a crash.
        bool fPeriodicWrite = mode == FLUSH_STATE_PERIODIC && nNow > nLastWrite + (int64_t)DATABASE_WRITE_INTERVAL * 1000000;
        // It's been very long since we flushed the cache. Do this infrequently, to optimize cache usage.
        bool fPeriodicFlush = mode == FLUSH_STATE_PERIODIC && nNow > nLastFlush + (int64_t)DATABASE_FLUSH_INTERVAL * 1000000;
        // Combine all conditions that result in a full cache flush.
        bool fDoFullFlush = (mode == FLUSH_STATE_ALWAYS) || fCacheLarge || fCacheCritical || fPeriodicFlush;
        // Write blocks and block index to disk.
        if (fDoFullFlush || fPeriodicWrite) {
            // Depend on nMinDiskSpace to ensure we can write block index
            if (!CheckDiskSpace(0))
                return state.Error("out of disk space");
            // First make sure all block and undo data is flushed to disk.
            FlushBlockFile();
            // Then update all block file information (which may refer to block and undo files).
            {
                std::vector<std::pair<int, const CBlockFileInfo*> > vFiles;
                vFiles.reserve(setDirtyFileInfo.size());
                for (std::set<int>::iterator it = setDirtyFileInfo.begin(); it != setDirtyFileInfo.end(); ) {
                    vFiles.push_back(std::make_pair(*it, &vinfoBlockFile[*it]));
                    setDirtyFileInfo.erase(it++);
                }
                std::vector<const CBlockIndex*> vBlocks;
                vBlocks.reserve(setDirtyBlockIndex.size());
                for (std::set<CBlockIndex*>::iterator it = setDirtyBlockIndex.begin(); it != setDirtyBlockIndex.end(); ) {
                    vBlocks.push_back(*it);
                    setDirtyBlockIndex.erase(it++);
                }

                if (!pblocktree->WriteBatchSync(vFiles, nLastBlockFile, vBlocks)) {
                    return AbortNode(state, "Files to write to block index database");
                }
            }
            nLastWrite = nNow;
        }

        // Flush best chain related state. This can only be done if the blocks / block index write was also done.
        if (fDoFullFlush) {
            // Typical CCoins structures on disk are around 128 bytes in size.
            // Pushing a new one to the database can cause it to be written
            // twice (once in the log, and once in the tables). This is already
            // an overestimation, as most will delete an existing entry or
            // overwrite one. Still, use a conservative safety factor of 2.
            if (!CheckDiskSpace(128 * 2 * 2 * pcoinsTip->GetCacheSize()))
                return state.Error("out of disk space");
            // Flush the chainstate (which may refer to block index entries).
            if (!pcoinsTip->Flush())
                return AbortNode(state, "Failed to write to coin database");
            nLastFlush = nNow;
        }
        if ((mode == FLUSH_STATE_ALWAYS || mode == FLUSH_STATE_PERIODIC) && nNow > nLastSetChain + (int64_t)DATABASE_WRITE_INTERVAL * 1000000) {
            // Update best block in wallet (so we can detect restored wallets).
            GetMainSignals().SetBestChain(chainActive.GetLocator());
            nLastSetChain = nNow;
        }

    } catch (const std::runtime_error& e) {
        return AbortNode(state, std::string("System error while flushing: ") + e.what());
    }
    return true;
}

void FlushStateToDisk()
{
    CValidationState state;
    FlushStateToDisk(state, FLUSH_STATE_ALWAYS);
}

/** Update chainActive and related internal data structures. */
void static UpdateTip(CBlockIndex* pindexNew)
{
    chainActive.SetTip(pindexNew);

    // New best block
    nTimeBestReceived = GetTime();
    mempool.AddTransactionsUpdated(1);

    {
        LOCK(g_best_block_mutex);
        g_best_block = pindexNew->GetBlockHash();
        g_best_block_cv.notify_all();
    }

    const CBlockIndex* pChainTip = chainActive.Tip();
    LogPrintf("UpdateTip: new best=%s  height=%d version=%d  log2_work=%.8g  tx=%lu  date=%s progress=%f  cache=%.1fMiB(%utx)\n",
              pChainTip->GetBlockHash().GetHex(), pChainTip->nHeight, pChainTip->nVersion, log(pChainTip->nChainWork.getdouble()) / log(2.0), (unsigned long)pChainTip->nChainTx,
                DateTimeStrFormat("%Y-%m-%d %H:%M:%S", pChainTip->GetBlockTime()),
                Checkpoints::GuessVerificationProgress(pChainTip), pcoinsTip->DynamicMemoryUsage() * (1.0 / (1<<20)), pcoinsTip->GetCacheSize());

    // Check the version of the last 100 blocks to see if we need to upgrade:
    static bool fWarned = false;
    if (!IsInitialBlockDownload() && !fWarned) {
        int nUpgraded = 0;
        const CBlockIndex* pindex = chainActive.Tip();
        for (int i = 0; i < 100 && pindex != NULL; i++) {
            if (pindex->nVersion > CBlock::CURRENT_VERSION)
                ++nUpgraded;
            pindex = pindex->pprev;
        }
        if (nUpgraded > 0)
            LogPrintf("SetBestChain: %d of last 100 blocks above version %d\n", nUpgraded, (int)CBlock::CURRENT_VERSION);
        if (nUpgraded > 100 / 2) {
            // strMiscWarning is read by GetWarnings(), called by Qt and the JSON-RPC code to warn the user:
            strMiscWarning = _("Warning: This version is obsolete, upgrade required!");
            AlertNotify(strMiscWarning, true);
            fWarned = true;
        }
    }
}

/** Disconnect chainActive's tip. */
bool static DisconnectTip(CValidationState& state)
{
    CBlockIndex* pindexDelete = chainActive.Tip();
    assert(pindexDelete);
    mempool.check(pcoinsTip);
    // Read block from disk.
    CBlock block;
    if (!ReadBlockFromDisk(block, pindexDelete))
        return AbortNode(state, "Failed to read block");
    // Apply the block atomically to the chain state.
    int64_t nStart = GetTimeMicros();
    {
        CCoinsViewCache view(pcoinsTip);
        if (!DisconnectBlock(block, state, pindexDelete, view))
            return error("DisconnectTip() : DisconnectBlock %s failed", pindexDelete->GetBlockHash().ToString());
        assert(view.Flush());
    }
    LogPrint(BCLog::BENCH, "- Disconnect block: %.2fms\n", (GetTimeMicros() - nStart) * 0.001);
    // Write the chain state to disk, if necessary.
    if (!FlushStateToDisk(state, FLUSH_STATE_ALWAYS))
        return false;
    // Resurrect mempool transactions from the disconnected block.
    for (const CTransaction& tx : block.vtx) {
        // ignore validation errors in resurrected transactions
        std::list<CTransaction> removed;
        CValidationState stateDummy;
        if (tx.IsCoinBase() || tx.IsCoinStake() || !AcceptToMemoryPool(mempool, stateDummy, tx, false, NULL))
            mempool.remove(tx, removed, true);
    }
    mempool.removeCoinbaseSpends(pcoinsTip, pindexDelete->nHeight);
    mempool.check(pcoinsTip);
    // Update chainActive and related variables.
    UpdateTip(pindexDelete->pprev);
    // Let wallets know transactions went from 1-confirmed to
    // 0-confirmed or conflicted:void

    for (const CTransaction& tx : block.vtx) {
        SyncWithWallets(tx, NULL);
    }
    return true;
}

static int64_t nTimeReadFromDisk = 0;
static int64_t nTimeConnectTotal = 0;
static int64_t nTimeFlush = 0;
static int64_t nTimeChainState = 0;
static int64_t nTimePostConnect = 0;

/**
 * Connect a new block to chainActive. pblock is either NULL or a pointer to a CBlock
 * corresponding to pindexNew, to bypass loading it again from disk.
 */
bool static ConnectTip(CValidationState& state, CBlockIndex* pindexNew, CBlock* pblock, bool fAlreadyChecked)
{
    assert(pindexNew->pprev == chainActive.Tip());
    mempool.check(pcoinsTip);
    CCoinsViewCache view(pcoinsTip);

    if (pblock == NULL)
        fAlreadyChecked = false;

    // Read block from disk.
    int64_t nTime1 = GetTimeMicros();
    CBlock block;
    if (!pblock) {
        if (!ReadBlockFromDisk(block, pindexNew))
            return AbortNode(state, "Failed to read block");
        pblock = &block;
    }
    // Apply the block atomically to the chain state.
    int64_t nTime2 = GetTimeMicros();
    nTimeReadFromDisk += nTime2 - nTime1;
    int64_t nTime3;
    LogPrint(BCLog::BENCH, "  - Load block from disk: %.2fms [%.2fs]\n", (nTime2 - nTime1) * 0.001,
        nTimeReadFromDisk * 0.000001);
    {
        CInv inv(MSG_BLOCK, pindexNew->GetBlockHash());
        bool rv = ConnectBlock(*pblock, state, pindexNew, view, false, fAlreadyChecked);
        GetMainSignals().BlockChecked(*pblock, state);
        if (!rv) {
            if (state.IsInvalid())
                InvalidBlockFound(pindexNew, state);
            return error("ConnectTip() : ConnectBlock %s failed", pindexNew->GetBlockHash().ToString());
        }
        mapBlockSource.erase(inv.hash);
        nTime3 = GetTimeMicros();
        nTimeConnectTotal += nTime3 - nTime2;
        LogPrint(BCLog::BENCH, "  - Connect total: %.2fms [%.2fs]\n", (nTime3 - nTime2) * 0.001,
            nTimeConnectTotal * 0.000001);
        assert(view.Flush());
    }
    int64_t nTime4 = GetTimeMicros();
    nTimeFlush += nTime4 - nTime3;
    LogPrint(BCLog::BENCH, "  - Flush: %.2fms [%.2fs]\n", (nTime4 - nTime3) * 0.001, nTimeFlush * 0.000001);

    // Write the chain state to disk, if necessary. Always write to disk if this is the first of a new file.
    FlushStateMode flushMode = FLUSH_STATE_IF_NEEDED;
    if (pindexNew->pprev && (pindexNew->GetBlockPos().nFile != pindexNew->pprev->GetBlockPos().nFile))
        flushMode = FLUSH_STATE_ALWAYS;
    if (!FlushStateToDisk(state, flushMode))
        return false;
    int64_t nTime5 = GetTimeMicros();
    nTimeChainState += nTime5 - nTime4;
    LogPrint(BCLog::BENCH, "  - Writing chainstate: %.2fms [%.2fs]\n", (nTime5 - nTime4) * 0.001,
        nTimeChainState * 0.000001);

    // Remove conflicting transactions from the mempool.
    std::list<CTransaction> txConflicted;
    mempool.removeForBlock(pblock->vtx, pindexNew->nHeight, txConflicted);
    mempool.check(pcoinsTip);
    // Update chainActive & related variables.
    UpdateTip(pindexNew);
    // Tell wallet about transactions that went from mempool
    // to conflicted:
    for (const CTransaction& tx : txConflicted) {
        SyncWithWallets(tx, NULL);
    }
    // ... and about transactions that got confirmed:
    for (const CTransaction& tx : pblock->vtx) {
        SyncWithWallets(tx, pblock);
    }

    int64_t nTime6 = GetTimeMicros();
    nTimePostConnect += nTime6 - nTime5;
    nTimeTotal += nTime6 - nTime1;
    LogPrint(BCLog::BENCH, "  - Connect postprocess: %.2fms [%.2fs]\n", (nTime6 - nTime5) * 0.001,
        nTimePostConnect * 0.000001);
    LogPrint(BCLog::BENCH, "- Connect block: %.2fms [%.2fs]\n", (nTime6 - nTime1) * 0.001, nTimeTotal * 0.000001);
    return true;
}

bool DisconnectBlocks(int nBlocks)
{
    LOCK(cs_main);

    CValidationState state;

    LogPrintf("%s: Got command to replay %d blocks\n", __func__, nBlocks);
    for (int i = 0; i <= nBlocks; i++)
        DisconnectTip(state);

    return true;
}

void ReprocessBlocks(int nBlocks)
{
    std::map<uint256, int64_t>::iterator it = mapRejectedBlocks.begin();
    while (it != mapRejectedBlocks.end()) {
        //use a window twice as large as is usual for the nBlocks we want to reset
        if ((*it).second > GetTime() - (nBlocks * Params().TargetSpacing() * 2)) {
            BlockMap::iterator mi = mapBlockIndex.find((*it).first);
            if (mi != mapBlockIndex.end() && (*mi).second) {
                LOCK(cs_main);

                CBlockIndex* pindex = (*mi).second;
                LogPrintf("%s - %s\n", __func__, (*it).first.ToString());

                CValidationState state;
                ReconsiderBlock(state, pindex);
            }
        }
        ++it;
    }

    CValidationState state;
    {
        LOCK(cs_main);
        DisconnectBlocks(nBlocks);
    }

    if (state.IsValid()) {
        ActivateBestChain(state);
    }
}

void RemoveInvalidTransactionsFromMempool()
{
    LOCK2(cs_main, pwalletMain->cs_wallet);
    LOCK(mempool.cs);
    std::vector<CTransaction> tobeRemoveds;
    for (std::map<uint256, CTxMemPoolEntry>::const_iterator it = mempool.mapTx.begin(); it != mempool.mapTx.end(); ++it) {
        const CTransaction& tx = it->second.GetTx();
        for(size_t i = 0; i < tx.vin.size(); i++) {
            std::string kiHex = tx.vin[i].keyImage.GetHex();
            int confirm = 0;
            if (CheckKeyImageSpendInMainChain(kiHex, confirm)) {
                if (confirm > Params().MaxReorganizationDepth()) {
                    tobeRemoveds.push_back(tx);
                    break;
                }
            }
            bool needsBreak = false;
            std::vector<COutPoint> decoys = tx.vin[i].decoys;
            decoys.push_back(tx.vin[i].prevout);
            for (size_t j = 0; j < decoys.size(); j++) {
                CTransaction txPrev;
                uint256 hashBlock;
                if (!GetTransaction(decoys[j].hash, txPrev, hashBlock)) {
                    tobeRemoveds.push_back(tx);
                    needsBreak = true;
                    break;
                }

                CBlockIndex* atTheblock = mapBlockIndex[hashBlock];
                if (!atTheblock) {
                    tobeRemoveds.push_back(tx);
                    needsBreak = true;
                    break;
                } else {
                    if (chainActive.Contains(atTheblock)) continue;
                    if (1 + chainActive.Height() - atTheblock->nHeight > 100) {
                        tobeRemoveds.push_back(tx);
                        needsBreak = true;
                        break;
                    }
                }
            }
            if (needsBreak) break;
        }
    }
    std::list<CTransaction> removed;
    for(size_t i = 0; i < tobeRemoveds.size(); i++) {
        mempool.remove(tobeRemoveds[i], removed, true);
    }
}

/**
 * Return the tip of the chain with the most work in it, that isn't
 * known to be invalid (it's however far from certain to be valid).
 */
static CBlockIndex* FindMostWorkChain()
{
    do {
        CBlockIndex* pindexNew = NULL;

        // Find the best candidate header.
        {
            std::set<CBlockIndex*, CBlockIndexWorkComparator>::reverse_iterator it = setBlockIndexCandidates.rbegin();
            if (it == setBlockIndexCandidates.rend())
                return NULL;
            pindexNew = *it;
        }

        // Check whether all blocks on the path between the currently active chain and the candidate are valid.
        // Just going until the active chain is an optimization, as we know all blocks in it are valid already.
        CBlockIndex* pindexTest = pindexNew;
        bool fInvalidAncestor = false;
        while (pindexTest && !chainActive.Contains(pindexTest)) {
            assert(pindexTest->nChainTx || pindexTest->nHeight == 0);

            // Pruned nodes may have entries in setBlockIndexCandidates for
            // which block files have been deleted.  Remove those as candidates
            // for the most work chain if we come across them; we can't switch
            // to a chain unless we have all the non-active-chain parent blocks.
            bool fFailedChain = pindexTest->nStatus & BLOCK_FAILED_MASK;
            bool fMissingData = !(pindexTest->nStatus & BLOCK_HAVE_DATA);
            if (fFailedChain || fMissingData) {
                // Candidate chain is not usable (either invalid or missing data)
                if (fFailedChain &&
                    (pindexBestInvalid == NULL || pindexNew->nChainWork > pindexBestInvalid->nChainWork))
                    pindexBestInvalid = pindexNew;
                CBlockIndex* pindexFailed = pindexNew;
                // Remove the entire chain from the set.
                while (pindexTest != pindexFailed) {
                    if (fFailedChain) {
                        pindexFailed->nStatus |= BLOCK_FAILED_CHILD;
                    } else if (fMissingData) {
                        // If we're missing data, then add back to mapBlocksUnlinked,
                        // so that if the block arrives in the future we can try adding
                        // to setBlockIndexCandidates again.
                        mapBlocksUnlinked.insert(std::make_pair(pindexFailed->pprev, pindexFailed));
                    }
                    setBlockIndexCandidates.erase(pindexFailed);
                    pindexFailed = pindexFailed->pprev;
                }
                setBlockIndexCandidates.erase(pindexTest);
                fInvalidAncestor = true;
                break;
            }
            pindexTest = pindexTest->pprev;
        }
        if (!fInvalidAncestor)
            return pindexNew;
    } while (true);
}

/** Delete all entries in setBlockIndexCandidates that are worse than the current tip. */
static void PruneBlockIndexCandidates()
{
    // Note that we can't delete the current block itself, as we may need to return to it later in case a
    // reorganization to a better block fails.
    std::set<CBlockIndex*, CBlockIndexWorkComparator>::iterator it = setBlockIndexCandidates.begin();
    while (it != setBlockIndexCandidates.end() && setBlockIndexCandidates.value_comp()(*it, chainActive.Tip())) {
        setBlockIndexCandidates.erase(it++);
    }
    // Either the current tip or a successor of it we're working towards is left in setBlockIndexCandidates.
    assert(!setBlockIndexCandidates.empty());
}

/**
 * Try to make some progress towards making pindexMostWork the active block.
 * pblock is either NULL or a pointer to a CBlock corresponding to pindexMostWork.
 */
static bool
ActivateBestChainStep(CValidationState& state, CBlockIndex* pindexMostWork, CBlock* pblock, bool fAlreadyChecked)
{
    AssertLockHeld(cs_main);
    if (pblock == NULL)
        fAlreadyChecked = false;
    bool fInvalidFound = false;
    const CBlockIndex* pindexOldTip = chainActive.Tip();
    const CBlockIndex* pindexFork = chainActive.FindFork(pindexMostWork);

    // Disconnect active blocks which are no longer in the best chain.
    while (chainActive.Tip() && chainActive.Tip() != pindexFork) {
        if (!DisconnectTip(state))
            return false;
    }

    // Build list of new blocks to connect.
    std::vector<CBlockIndex*> vpindexToConnect;
    bool fContinue = true;
    int nHeight = pindexFork ? pindexFork->nHeight : -1;
    while (fContinue && nHeight != pindexMostWork->nHeight) {
        // Don't iterate the entire list of potential improvements toward the best tip, as we likely only need
        // a few blocks along the way.
        int nTargetHeight = std::min(nHeight + 32, pindexMostWork->nHeight);
        vpindexToConnect.clear();
        vpindexToConnect.reserve(nTargetHeight - nHeight);
        CBlockIndex* pindexIter = pindexMostWork->GetAncestor(nTargetHeight);
        while (pindexIter && pindexIter->nHeight != nHeight) {
            vpindexToConnect.push_back(pindexIter);
            pindexIter = pindexIter->pprev;
        }
        nHeight = nTargetHeight;

        // Connect new blocks.
        BOOST_REVERSE_FOREACH (CBlockIndex* pindexConnect, vpindexToConnect) {
            if (!ConnectTip(state, pindexConnect, pindexConnect == pindexMostWork ? pblock : NULL, fAlreadyChecked)) {
                if (state.IsInvalid()) {
                    // The block violates a consensus rule.
                    if (!state.CorruptionPossible())
                        InvalidChainFound(vpindexToConnect.back());
                    state = CValidationState();
                    fInvalidFound = true;
                    fContinue = false;
                    break;
                } else {
                    // A system error occurred (disk space, database error, ...).
                    return false;
                }
            } else {
                PruneBlockIndexCandidates();
                if (!pindexOldTip || chainActive.Tip()->nChainWork > pindexOldTip->nChainWork) {
                    // We're in a better position than we were. Return temporarily to release the lock.
                    fContinue = false;
                    break;
                }
            }
        }
    }

    // Callbacks/notifications for a new best chain.
    if (fInvalidFound)
        CheckForkWarningConditionsOnNewFork(vpindexToConnect.back());
    else
        CheckForkWarningConditions();

    return true;
}

/**
 * Make the best chain active, in multiple steps. The result is either failure
 * or an activated best chain. pblock is either NULL or a pointer to a block
 * that is already loaded (to avoid loading it again from disk).
 */
bool ActivateBestChain(CValidationState& state, CBlock* pblock, bool fAlreadyChecked)
{
    // Note that while we're often called here from ProcessNewBlock, this is
    // far from a guarantee. Things in the P2P/RPC will often end up calling
    // us in the middle of ProcessNewBlock - do not assume pblock is set
    // sanely for performance or correctness!
    AssertLockNotHeld(cs_main);

    CBlockIndex* pindexNewTip = nullptr;
    CBlockIndex* pindexMostWork = nullptr;
    do {
        boost::this_thread::interruption_point();

        const CBlockIndex *pindexFork;
        bool fInitialDownload;
        while (true) {
            TRY_LOCK(cs_main, lockMain);
            if (!lockMain) {
                MilliSleep(50);
                continue;
            }
            CBlockIndex *pindexOldTip = chainActive.Tip();
            pindexMostWork = FindMostWorkChain();
            // Whether we have anything to do at all.
            if (pindexMostWork == NULL || pindexMostWork == chainActive.Tip())
                return true;
            if (!ActivateBestChainStep(state, pindexMostWork,
                    pblock && pblock->GetHash() == pindexMostWork->GetBlockHash() ? pblock : NULL,
                    fAlreadyChecked))
                return false;
            pindexNewTip = chainActive.Tip();
            pindexFork = chainActive.FindFork(pindexOldTip);
            fInitialDownload = IsInitialBlockDownload();
            break;
        }

        // When we reach this point, we switched to a new tip (stored in pindexNewTip).
        // Notifications/callbacks that can run without cs_main
        // Always notify the UI if a new block tip was connected
        if (pindexFork != pindexNewTip) {

            // Notify the UI
            uiInterface.NotifyBlockTip(fInitialDownload, pindexNewTip);

            // Notifications/callbacks that can run without cs_main
            if (!fInitialDownload) {
                uint256 hashNewTip = pindexNewTip->GetBlockHash();
                // Relay inventory, but don't relay old inventory during initial block download.
                int nBlockEstimate = Checkpoints::GetTotalBlocksEstimate();
                {
                    LOCK(cs_vNodes);
                    for (CNode *pnode : vNodes)
                        if (chainActive.Height() >
                            (pnode->nStartingHeight != -1 ? pnode->nStartingHeight - 2000 : nBlockEstimate))
                            pnode->PushInventory(CInv(MSG_BLOCK, hashNewTip));
                }
                // Notify external listeners about the new tip.
                GetMainSignals().UpdatedBlockTip(pindexNewTip);

                unsigned size = 0;
                if (pblock)
                    size = GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION);
                // If the size is over 1 MB notify external listeners, and it is within the last 5 minutes
                if (size > MAX_BLOCK_SIZE_LEGACY && pblock->GetBlockTime() > GetAdjustedTime() - 300) {
                    uiInterface.NotifyBlockSize(static_cast<int>(size), hashNewTip);
                }
            }

        }
    } while (pindexMostWork != chainActive.Tip());
    CheckBlockIndex();

    // Write changes periodically to disk, after relay.
    if (!FlushStateToDisk(state, FLUSH_STATE_PERIODIC)) {
        return false;
    }

    return true;
}

bool InvalidateBlock(CValidationState& state, CBlockIndex* pindex)
{
    AssertLockHeld(cs_main);

    // Mark the block itself as invalid.
    pindex->nStatus |= BLOCK_FAILED_VALID;
    setDirtyBlockIndex.insert(pindex);
    setBlockIndexCandidates.erase(pindex);

    while (chainActive.Contains(pindex)) {
        CBlockIndex* pindexWalk = chainActive.Tip();
        pindexWalk->nStatus |= BLOCK_FAILED_CHILD;
        setDirtyBlockIndex.insert(pindexWalk);
        setBlockIndexCandidates.erase(pindexWalk);
        // ActivateBestChain considers blocks already in chainActive
        // unconditionally valid already, so force disconnect away from it.
        if (!DisconnectTip(state)) {
            return false;
        }
    }

    // The resulting new best tip may not be in setBlockIndexCandidates anymore, so
    // add them again.
    BlockMap::iterator it = mapBlockIndex.begin();
    while (it != mapBlockIndex.end()) {
        if (it->second->IsValid(BLOCK_VALID_TRANSACTIONS) && it->second->nChainTx &&
            !setBlockIndexCandidates.value_comp()(it->second, chainActive.Tip())) {
            setBlockIndexCandidates.insert(it->second);
        }
        it++;
    }

    InvalidChainFound(pindex);
    return true;
}

bool ReconsiderBlock(CValidationState& state, CBlockIndex* pindex)
{
    AssertLockHeld(cs_main);

    int nHeight = pindex->nHeight;

    // Remove the invalidity flag from this block and all its descendants.
    BlockMap::iterator it = mapBlockIndex.begin();
    while (it != mapBlockIndex.end()) {
        if (!it->second->IsValid() && it->second->GetAncestor(nHeight) == pindex) {
            it->second->nStatus &= ~BLOCK_FAILED_MASK;
            setDirtyBlockIndex.insert(it->second);
            if (it->second->IsValid(BLOCK_VALID_TRANSACTIONS) && it->second->nChainTx &&
                setBlockIndexCandidates.value_comp()(chainActive.Tip(), it->second)) {
                setBlockIndexCandidates.insert(it->second);
            }
            if (it->second == pindexBestInvalid) {
                // Reset invalid block marker if it was pointing to one of those.
                pindexBestInvalid = NULL;
            }
        }
        it++;
    }

    // Remove the invalidity flag from all ancestors too.
    while (pindex != NULL) {
        if (pindex->nStatus & BLOCK_FAILED_MASK) {
            pindex->nStatus &= ~BLOCK_FAILED_MASK;
            setDirtyBlockIndex.insert(pindex);
        }
        pindex = pindex->pprev;
    }
    return true;
}

CBlockIndex* AddToBlockIndex(const CBlock& block)
{
    // Check for duplicate
    uint256 hash = block.GetHash();
    BlockMap::iterator it = mapBlockIndex.find(hash);
    if (it != mapBlockIndex.end())
        return it->second;

    // Construct new block index object
    CBlockIndex* pindexNew = new CBlockIndex(block);
    assert(pindexNew);
    // We assign the sequence id to blocks only when the full data is available,
    // to avoid miners withholding blocks but broadcasting headers, to get a
    // competitive advantage.
    pindexNew->nSequenceId = 0;
    BlockMap::iterator mi = mapBlockIndex.insert(std::make_pair(hash, pindexNew)).first;

    pindexNew->phashBlock = &((*mi).first);
    BlockMap::iterator miPrev = mapBlockIndex.find(block.hashPrevBlock);
    if (miPrev != mapBlockIndex.end()) {
        pindexNew->pprev = (*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
        pindexNew->BuildSkip();

        //update previous block pointer
        pindexNew->pprev->pnext = pindexNew;

        // ppcoin: compute chain trust score
        pindexNew->bnChainTrust = (pindexNew->pprev ? pindexNew->pprev->bnChainTrust : 0) + pindexNew->GetBlockTrust();

        // ppcoin: compute stake entropy bit for stake modifier
        if (!block.IsPoABlockByVersion() && !pindexNew->SetStakeEntropyBit(pindexNew->GetStakeEntropyBit()))
            LogPrintf("AddToBlockIndex() : SetStakeEntropyBit() failed \n");

        // ppcoin: record proof-of-stake hash value
        if (pindexNew->IsProofOfStake()) {
            if (!mapProofOfStake.count(hash))
                LogPrintf("AddToBlockIndex() : hashProofOfStake not found in map \n");
            pindexNew->hashProofOfStake = mapProofOfStake[hash];
        }

        // ppcoin: compute stake modifier
        uint64_t nStakeModifier = 0;
        bool fGeneratedStakeModifier = false;
        if (!block.IsPoABlockByVersion() && !ComputeNextStakeModifier(pindexNew->pprev, nStakeModifier, fGeneratedStakeModifier))
            LogPrintf("AddToBlockIndex() : ComputeNextStakeModifier() failed \n");
        pindexNew->SetStakeModifier(nStakeModifier, fGeneratedStakeModifier);
        pindexNew->nStakeModifierChecksum = GetStakeModifierChecksum(pindexNew);
        if (!block.IsPoABlockByVersion() && !CheckStakeModifierCheckpoints(pindexNew->nHeight, pindexNew->nStakeModifierChecksum))
            LogPrintf("AddToBlockIndex() : Rejected by stake modifier checkpoint height=%d, modifier=%s \n",
                pindexNew->nHeight, std::to_string(nStakeModifier));
    }
    pindexNew->nChainWork = (pindexNew->pprev ? pindexNew->pprev->nChainWork : 0) + GetBlockProof(*pindexNew);
    pindexNew->RaiseValidity(BLOCK_VALID_TREE);
    if (pindexBestHeader == NULL || pindexBestHeader->nChainWork < pindexNew->nChainWork)
        pindexBestHeader = pindexNew;

    //update previous block pointer
    if (pindexNew->nHeight)
        pindexNew->pprev->pnext = pindexNew;

    setDirtyBlockIndex.insert(pindexNew);

    return pindexNew;
}

/** Mark a block as having its data received and checked (up to BLOCK_VALID_TRANSACTIONS). */
bool ReceivedBlockTransactions(const CBlock& block, CValidationState& state, CBlockIndex* pindexNew, const CDiskBlockPos& pos)
{
    if (block.IsProofOfStake())
        pindexNew->SetProofOfStake();
    pindexNew->nTx = block.vtx.size();
    pindexNew->nChainTx = 0;
    pindexNew->nFile = pos.nFile;
    pindexNew->nDataPos = pos.nPos;
    pindexNew->nUndoPos = 0;
    pindexNew->nStatus |= BLOCK_HAVE_DATA;
    pindexNew->RaiseValidity(BLOCK_VALID_TRANSACTIONS);
    setDirtyBlockIndex.insert(pindexNew);

    if (pindexNew->pprev == NULL || pindexNew->pprev->nChainTx) {
        // If pindexNew is the genesis block or all parents are BLOCK_VALID_TRANSACTIONS.
        std::deque<CBlockIndex*> queue;
        queue.push_back(pindexNew);

        // Recursively process any descendant blocks that now may be eligible to be connected.
        while (!queue.empty()) {
            CBlockIndex* pindex = queue.front();
            queue.pop_front();
            pindex->nChainTx = (pindex->pprev ? pindex->pprev->nChainTx : 0) + pindex->nTx;
            {
                LOCK(cs_nBlockSequenceId);
                pindex->nSequenceId = nBlockSequenceId++;
            }
            if (chainActive.Tip() == NULL || !setBlockIndexCandidates.value_comp()(pindex, chainActive.Tip())) {
                setBlockIndexCandidates.insert(pindex);
            }
            std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> range = mapBlocksUnlinked.equal_range(
                pindex);
            while (range.first != range.second) {
                std::multimap<CBlockIndex*, CBlockIndex*>::iterator it = range.first;
                queue.push_back(it->second);
                range.first++;
                mapBlocksUnlinked.erase(it);
            }
        }
    } else {
        if (pindexNew->pprev && pindexNew->pprev->IsValid(BLOCK_VALID_TREE)) {
            mapBlocksUnlinked.insert(std::make_pair(pindexNew->pprev, pindexNew));
        }
    }

    return true;
}

bool FindBlockPos(CValidationState& state, CDiskBlockPos& pos, unsigned int nAddSize, unsigned int nHeight, uint64_t nTime, bool fKnown = false)
{
    LOCK(cs_LastBlockFile);

    unsigned int nFile = fKnown ? pos.nFile : nLastBlockFile;
    if (vinfoBlockFile.size() <= nFile) {
        vinfoBlockFile.resize(nFile + 1);
    }

    if (!fKnown) {
        while (vinfoBlockFile[nFile].nSize + nAddSize >= MAX_BLOCKFILE_SIZE) {
            LogPrintf("Leaving block file %i: %s\n", nFile, vinfoBlockFile[nFile].ToString());
            FlushBlockFile(true);
            nFile++;
            if (vinfoBlockFile.size() <= nFile) {
                vinfoBlockFile.resize(nFile + 1);
            }
        }
        pos.nFile = nFile;
        pos.nPos = vinfoBlockFile[nFile].nSize;
    }

    nLastBlockFile = nFile;
    vinfoBlockFile[nFile].AddBlock(nHeight, nTime);
    if (fKnown)
        vinfoBlockFile[nFile].nSize = std::max(pos.nPos + nAddSize, vinfoBlockFile[nFile].nSize);
    else
        vinfoBlockFile[nFile].nSize += nAddSize;

    if (!fKnown) {
        unsigned int nOldChunks = (pos.nPos + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        unsigned int nNewChunks = (vinfoBlockFile[nFile].nSize + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        if (nNewChunks > nOldChunks) {
            if (CheckDiskSpace(nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos)) {
                FILE* file = OpenBlockFile(pos);
                if (file) {
                    LogPrintf("Pre-allocating up to position 0x%x in blk%05u.dat\n", nNewChunks * BLOCKFILE_CHUNK_SIZE,
                        pos.nFile);
                    AllocateFileRange(file, pos.nPos, nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos);
                    fclose(file);
                }
            } else
                return state.Error("out of disk space");
        }
    }

    setDirtyFileInfo.insert(nFile);
    return true;
}

bool FindUndoPos(CValidationState& state, int nFile, CDiskBlockPos& pos, unsigned int nAddSize)
{
    pos.nFile = nFile;

    LOCK(cs_LastBlockFile);

    unsigned int nNewSize;
    pos.nPos = vinfoBlockFile[nFile].nUndoSize;
    nNewSize = vinfoBlockFile[nFile].nUndoSize += nAddSize;
    setDirtyFileInfo.insert(nFile);

    unsigned int nOldChunks = (pos.nPos + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
    unsigned int nNewChunks = (nNewSize + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
    if (nNewChunks > nOldChunks) {
        if (CheckDiskSpace(nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos)) {
            FILE* file = OpenUndoFile(pos);
            if (file) {
                LogPrintf("Pre-allocating up to position 0x%x in rev%05u.dat\n", nNewChunks * UNDOFILE_CHUNK_SIZE,
                    pos.nFile);
                AllocateFileRange(file, pos.nPos, nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos);
                fclose(file);
            }
        } else
            return state.Error("out of disk space");
    }

    return true;
}

bool CheckBlockHeader(const CBlockHeader& block, CValidationState& state, bool fCheckPOW)
{
    // Check proof of work matches claimed amount
    if (fCheckPOW && !CheckProofOfWork(block.GetHash(), block.nBits))
        return state.DoS(50, error("CheckBlockHeader() : proof of work failed"),
            REJECT_INVALID, "high-hash");

    if (Params().IsRegTestNet()) return true;

    //PoA specific header checks
    //Check that the header is valid for PoA mining, this check will use a PoA consensus rule
    if (block.IsPoABlockByVersion() && !CheckPoABlockMinedHash(block)) {
        return state.DoS(50, error("CheckBlockHeader() : proof of work PoA failed"),
            REJECT_INVALID, "high-hash");
    }

    //Check that the PoA header contains valid for PoA previous block hash, this check will use a PoA consensus rule
    if (block.IsPoABlockByVersion() && !CheckPrevPoABlockHash(block)) {
        return state.DoS(50, error("CheckBlockHeader() : Previous PoA block hash is not matched => failed"),
            REJECT_INVALID, "high-hash");
    }

    return true;
}


bool CheckBlock(const CBlock& block, CValidationState& state, bool fCheckPOW, bool fCheckMerkleRoot, bool fCheckSig)
{
    if (block.fChecked)
        return true;

    // Check that the header is valid (particularly PoW).  This is mostly
    // redundant with the call in AcceptBlockHeader.
    if (!CheckBlockHeader(block, state, block.IsProofOfWork()))
        return state.DoS(100, error("CheckBlock() : CheckBlockHeader failed"),
            REJECT_INVALID, "bad-header", true);
    // Check timestamp
    //LogPrint("debug", "%s: block=%s  is proof of stake=%d, is proof of audit=%d\n", __func__, block.GetHash().ToString().c_str(),
        //block.IsProofOfStake(), block.IsProofOfAudit());
    if (!Params().IsRegTestNet() && !block.IsPoABlockByVersion() && block.GetBlockTime() >
                                            GetAdjustedTime() + (block.IsProofOfStake() ? 180 : 7200)) // 3 minute future drift for PoS
        return state.Invalid(error("CheckBlock() : block timestamp too far in the future"),
            REJECT_INVALID, "time-too-new");

    //check duplicate key image in blocks
    std::set<CKeyImage> keyimages;
    for(size_t i = 0; i < block.vtx.size(); i++) {
        for (const CTxIn& txin : block.vtx[i].vin) {
            if (!txin.keyImage.IsValid()) continue;
            if (keyimages.count(txin.keyImage)) {
                return state.DoS(100, error("CheckBlock() : duplicate inputs"),
                        REJECT_INVALID, "bad-txns-inputs-duplicate");
            }
            keyimages.insert(txin.keyImage);
        }
    }

    // Check the merkle root.
    if (fCheckMerkleRoot) {
        bool mutated;
        uint256 hashMerkleRoot2 = BlockMerkleRoot(block, &mutated);
        if (block.hashMerkleRoot != hashMerkleRoot2)
            return state.DoS(100, error("CheckBlock() : hashMerkleRoot mismatch"),
                REJECT_INVALID, "bad-txnmrklroot", true);

        // Check for merkle tree malleability (CVE-2012-2459): repeating sequences
        // of transactions in a block without affecting the merkle root of a block,
        // while still invalidating it.
        if (mutated)
            return state.DoS(100, error("CheckBlock() : duplicate transaction"),
                REJECT_INVALID, "bad-txns-duplicate", true);
    }

    //Proof of Audit: Check audited PoS blocks infor merkle root
    {
        bool fMutated;
        if (!CheckPoAMerkleRoot(block, &fMutated)) {
            return state.DoS(100, error("CheckBlock() : hashPoAMerkleRoot mismatch"),
                REJECT_INVALID, "bad-txnmrklroot", true);
        }
        // Check for PoA merkle tree malleability (CVE-2012-2459): repeating sequences
        // of transactions in a block without affecting the PoA merkle root of a block,
        // while still invalidating it.
        if (fMutated)
            return state.DoS(100, error("CheckBlock() : duplicate PoS block info"),
                REJECT_INVALID, "bad-txns-duplicate", true);
    }

    // All potential-corruption validation must be done before we do any
    // transaction validation, as otherwise we may mark the header as invalid
    // because we receive the wrong transactions for it.

    // Size limits
    unsigned int nMaxBlockSize = MAX_BLOCK_SIZE_CURRENT;
    if (block.vtx.empty() || block.vtx.size() > nMaxBlockSize ||
        ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION) > nMaxBlockSize)
        return state.DoS(100, error("CheckBlock() : size limits failed"),
            REJECT_INVALID, "bad-blk-length");

    // First transaction must be coinbase, the rest must not be
    if ((!block.IsPoABlockByVersion()) && (block.vtx.empty() || !block.vtx[0].IsCoinBase()))
        return state.DoS(100, error("CheckBlock() : first tx is not coinbase"),
            REJECT_INVALID, "bad-cb-missing");
    for (unsigned int i = 1; i < block.vtx.size(); i++)
        if (block.vtx[i].IsCoinBase())
            return state.DoS(100, error("CheckBlock() : more than one coinbase"),
                REJECT_INVALID, "bad-cb-multiple");

    if (block.IsProofOfStake()) {
        // Coinbase output should be empty if proof-of-stake block
        if (block.vtx[0].vout.size() != 1 || !block.vtx[0].vout[0].IsEmpty())
            return state.DoS(100, error("CheckBlock() : coinbase output not empty for proof-of-stake block"));
        // Second transaction must be coinstake, the rest must not be
        if (block.vtx.empty() || !block.vtx[1].IsCoinStake())
            return state.DoS(100, error("CheckBlock() : second tx is not coinstake"));
        for (unsigned int i = 2; i < block.vtx.size(); i++)
            if (block.vtx[i].IsCoinStake())
                return state.DoS(100, error("CheckBlock() : more than one coinstake"));

        const CTransaction& coinstake = block.vtx[1];
        int numUTXO = coinstake.vout.size();

        //verify shnorr signature
        if (!VerifyShnorrKeyImageTx(coinstake)) {
            return state.DoS(100, error("CheckBlock() : Failed to verify shnorr signature"));
        }

        //verify commitments for all UTXOs
        for (int i = 1; i < numUTXO; i++) {
            if (!VerifyZeroBlindCommitment(coinstake.vout[i]))
                return state.DoS(100, error("CheckBlock() : PoS rewards commitment not correct"));
        }

        std::set<COutPoint> vInOutPoints;
        for (const CTxIn& txin : block.vtx[1].vin) {
            if (vInOutPoints.count(txin.prevout) && chainActive.Tip()->nHeight > Params().SyncFix())
                return state.DoS(100, error("CheckBlock() : duplicate inputs"),
                    REJECT_INVALID, "bad-txns-inputs-duplicate");
                vInOutPoints.insert(txin.prevout);
        }

        if (coinstake.vin.size() > 1 && chainActive.Tip()->nHeight > Params().FixChecks())
            return state.DoS(100, error("%s : multiple stake inputs not allowed", __func__), REJECT_INVALID, "bad-txns-stake");
    }

    if (block.IsProofOfAudit() || block.IsProofOfWork()) {
        //verify commitment
        if (!VerifyZeroBlindCommitment(block.vtx[0].vout[0]))
            return state.DoS(100, error("CheckBlock() : PoS rewards commitment not correct"));
    }

    /**
     * @todo Audit checkblock
     */
    if (block.IsProofOfAudit()) {
        if (chainActive.Tip()->nHeight < Params().START_POA_BLOCK()) {
            return state.DoS(100, error("CheckBlock() : PoA block should only start at block height=%d", Params().START_POA_BLOCK()));
        }
    }

    // masternode payments / budgets
    CBlockIndex* pindexPrev = chainActive.Tip();
    int nHeight = 0;
    if (pindexPrev != NULL) {
        if (pindexPrev->GetBlockHash() == block.hashPrevBlock) {
            nHeight = pindexPrev->nHeight + 1;
        } else { //out of order
            BlockMap::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
            if (mi != mapBlockIndex.end() && (*mi).second)
                nHeight = (*mi).second->nHeight + 1;
        }

        // PRCYcoin
        // It is entierly possible that we don't have enough data and this could fail
        // (i.e. the block could indeed be valid). Store the block for later consideration
        // but issue an initial reject message.
        // The case also exists that the sending peer could not have enough data to see
        // that this block is invalid, so don't issue an outright ban.
        if (block.IsProofOfStake() && nHeight != 0 && !IsInitialBlockDownload()) {
            if (!IsBlockPayeeValid(block, nHeight)) {
                mapRejectedBlocks.insert(std::make_pair(block.GetHash(), GetTime()));
                return state.DoS(0, error("CheckBlock() : Couldn't find masternode/budget payment"),
                    REJECT_INVALID, "bad-cb-payee");
            }
        } else {
            LogPrint(BCLog::MASTERNODE, "%s: Masternode payment check skipped on sync - skipping IsBlockPayeeValid()\n", __func__);
        }
    }

    unsigned int nSigOps = 0;
    for (const CTransaction& tx : block.vtx) {
        nSigOps += GetLegacySigOpCount(tx);
    }
    unsigned int nMaxBlockSigOps = MAX_BLOCK_SIGOPS_LEGACY;
    if (nSigOps > nMaxBlockSigOps)
        return state.DoS(100, error("ConnectBlock() : too many sigops"), REJECT_INVALID, "bad-blk-sigops");

    return true;
}

bool CheckWork(const CBlock block, CBlockIndex* const pindexPrev)
{
    if (pindexPrev == NULL)
        return error("%s : null pindexPrev for block %s", __func__, block.GetHash().ToString().c_str());
    unsigned int nBitsRequired = GetNextWorkRequired(pindexPrev, &block);
    if (!Params().IsRegTestNet() && block.IsProofOfWork() && (pindexPrev->nHeight + 1 <= 68589)) {
        double n1 = ConvertBitsToDouble(block.nBits);
        double n2 = ConvertBitsToDouble(nBitsRequired);

        if (std::abs(n1 - n2) > n1 * 0.5)
            return error("%s : incorrect proof of work (DGW pre-fork) - %f %f %f at %d", __func__, std::abs(n1 - n2), n1, n2, pindexPrev->nHeight + 1);

        return true;
    }

    if (block.nBits != nBitsRequired)
        return error("%s : incorrect proof of work at %d", __func__, pindexPrev->nHeight + 1);

    return true;
}

bool ContextualCheckBlockHeader(const CBlockHeader& block, CValidationState& state, CBlockIndex* const pindexPrev)
{
    uint256 hash = block.GetHash();

    if (hash == Params().HashGenesisBlock())
        return true;

    assert(pindexPrev);

    const int nHeight = pindexPrev->nHeight + 1;
    const int chainHeight = chainActive.Height();

    if ((Params().IsRegTestNet() && block.nBits != GetNextWorkRequired(pindexPrev, &block)))
        return state.DoS(100, error("%s : incorrect proof of work", __func__),
                REJECT_INVALID, "bad-diffbits");


    //If this is a reorg, check that it is not too deep
    int nMaxReorgDepth = GetArg("-maxreorg", Params().MaxReorganizationDepth());
    if (chainHeight - nHeight >= nMaxReorgDepth)
        return state.DoS(1, error("%s: forked chain older than max reorganization depth (height %d)", __func__, chainHeight - nHeight));

    // Check timestamp against prev
    if (!block.IsPoABlockByVersion() && block.GetBlockTime() <= pindexPrev->GetMedianTimePast() && !Params().IsRegTestNet()) {
        LogPrintf("Block time = %d , GetMedianTimePast = %d \n", block.GetBlockTime(), pindexPrev->GetMedianTimePast());
        return state.Invalid(error("%s : block's timestamp is too early", __func__),
            REJECT_INVALID, "time-too-old");
    }

    // Check that the block chain matches the known block chain up to a checkpoint
    if (!Checkpoints::CheckBlock(nHeight, hash))
        return state.DoS(100, error("%s : rejected by checkpoint lock-in at %d", __func__, nHeight),
            REJECT_CHECKPOINT, "checkpoint mismatch");

    // Don't accept any forks from the main chain prior to last checkpoint
    CBlockIndex* pcheckpoint = Checkpoints::GetLastCheckpoint();
    if (pcheckpoint && nHeight < pcheckpoint->nHeight)
        return state.DoS(0, error("%s : forked chain older than last checkpoint (height %d)", __func__, nHeight));

    // Reject block.nVersion=1 blocks when 95% (75% on testnet) of the network has upgraded:
    if (block.nVersion < 2 &&
        CBlockIndex::IsSuperMajority(2, pindexPrev, Params().RejectBlockOutdatedMajority())) {
        return state.Invalid(error("%s : rejected nVersion=1 block", __func__),
            REJECT_OBSOLETE, "bad-version");
    }

    // Reject block.nVersion=2 blocks when 95% (75% on testnet) of the network has upgraded:
    if (block.nVersion < 3 && CBlockIndex::IsSuperMajority(3, pindexPrev, Params().RejectBlockOutdatedMajority())) {
        return state.Invalid(error("%s : rejected nVersion=2 block", __func__),
            REJECT_OBSOLETE, "bad-version");
    }

    // Reject block.nVersion=4 blocks when 95% (75% on testnet) of the network has upgraded:
    if (block.nVersion < 5 && CBlockIndex::IsSuperMajority(5, pindexPrev, Params().RejectBlockOutdatedMajority())) {
        return state.Invalid(error("%s : rejected nVersion=4 block", __func__),
                             REJECT_OBSOLETE, "bad-version");
    }

    return true;
}

bool IsBlockHashInChain(const uint256& hashBlock)
{
    if (hashBlock.IsNull() || !mapBlockIndex.count(hashBlock))
        return false;

    return chainActive.Contains(mapBlockIndex[hashBlock]);
}

bool IsTransactionInChain(const uint256& txId, int& nHeightTx, CTransaction& tx)
{
    uint256 hashBlock;
    if (!GetTransaction(txId, tx, hashBlock, true))
        return false;
    if (!IsBlockHashInChain(hashBlock))
        return false;

    nHeightTx = mapBlockIndex.at(hashBlock)->nHeight;
    return true;
}

bool IsTransactionInChain(const uint256& txId, int& nHeightTx)
{
    CTransaction tx;
    return IsTransactionInChain(txId, nHeightTx, tx);
}

bool ContextualCheckBlock(const CBlock& block, CValidationState& state, CBlockIndex* const pindexPrev)
{
    const int nHeight = pindexPrev == NULL ? 0 : pindexPrev->nHeight + 1;

    // Check that all transactions are finalized
    for (const CTransaction& tx : block.vtx)
        if (!block.IsProofOfAudit() && !IsFinalTx(tx, nHeight, block.GetBlockTime())) {
            return state.DoS(10, error("%s : contains a non-final transaction", __func__), REJECT_INVALID,
                "bad-txns-nonfinal");
        }

    // Enforce block.nVersion=2 rule that the coinbase starts with serialized block height
    // if 750 of the last 1,000 blocks are version 2 or greater (51/100 if testnet):
    if (!block.IsProofOfAudit() && block.nVersion >= 2 &&
        CBlockIndex::IsSuperMajority(2, pindexPrev, Params().EnforceBlockUpgradeMajority())) {
        CScript expect = CScript() << nHeight;
        if (block.vtx[0].vin[0].scriptSig.size() < expect.size() ||
            !std::equal(expect.begin(), expect.end(), block.vtx[0].vin[0].scriptSig.begin())) {
            return state.DoS(100, error("%s : block height mismatch in coinbase", __func__), REJECT_INVALID,
                "bad-cb-height");
        }
    }

    return true;
}

bool AcceptBlockHeader(const CBlock& block, CValidationState& state, CBlockIndex** ppindex)
{
    AssertLockHeld(cs_main);
    // Check for duplicate
    uint256 hash = block.GetHash();
    BlockMap::iterator miSelf = mapBlockIndex.find(hash);
    CBlockIndex* pindex = NULL;
    // TODO : ENABLE BLOCK CACHE IN SPECIFIC CASES
    if (miSelf != mapBlockIndex.end()) {
        // Block header is already known.
        pindex = miSelf->second;
        if (ppindex)
            *ppindex = pindex;
        if (!pindex) {
            mapBlockIndex.erase(hash);
            return state.Invalid(error("%s : block is not found", __func__), 0, "not-found");
        }
        if (pindex->nStatus & BLOCK_FAILED_MASK)
            return state.Invalid(error("%s : block is marked invalid", __func__), 0, "duplicate");
        return true;
    }
    if (!CheckBlockHeader(block, state, false)) {
        LogPrintf("AcceptBlockHeader(): CheckBlockHeader failed \n");
        return false;
    }
    // Get prev block index
    CBlockIndex* pindexPrev = NULL;
    if (hash != Params().HashGenesisBlock()) {
        BlockMap::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
        if (mi == mapBlockIndex.end())
            return state.DoS(0, error("%s : prev block %s not found", __func__, block.hashPrevBlock.ToString().c_str()),
                0, "bad-prevblk");
        pindexPrev = (*mi).second;
        if (pindexPrev->nStatus & BLOCK_FAILED_MASK) {
            //If this "invalid" block is an exact match from the checkpoints, then reconsider it
            if (pindex && Checkpoints::CheckBlock(pindex->nHeight - 1, block.hashPrevBlock, true)) {
                LogPrintf("%s : Reconsidering block %s height %d\n", __func__, pindexPrev->GetBlockHash().GetHex(),
                    pindexPrev->nHeight);
                CValidationState statePrev;
                ReconsiderBlock(statePrev, pindexPrev);
                if (statePrev.IsValid()) {
                    ActivateBestChain(statePrev);
                    return true;
                }
            }

            return state.DoS(100,
                error("%s : prev block height=%d hash=%s is invalid, unable to add block %s", __func__,
                    pindexPrev->nHeight, block.hashPrevBlock.GetHex(), block.GetHash().GetHex()),
                REJECT_INVALID, "bad-prevblk");
        }
    }

    if (!pindexPrev)
        return state.DoS(0, error("%s : prev block %s not found", __func__, block.hashPrevBlock.ToString().c_str()),
            0, "bad-prevblk");
    if (!ContextualCheckBlockHeader(block, state, pindexPrev))
        return false;

    if (pindex == NULL)
        pindex = AddToBlockIndex(block);

    if (ppindex)
        *ppindex = pindex;

    return true;
}

bool AcceptBlock(CBlock& block, CValidationState& state, CBlockIndex** ppindex, CDiskBlockPos* dbp, bool fAlreadyCheckedBlock)
{
    AssertLockHeld(cs_main);
    CBlockIndex*& pindex = *ppindex;
    // Get prev block index
    CBlockIndex* pindexPrev = NULL;
    if (block.GetHash() != Params().HashGenesisBlock()) {
        BlockMap::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
        if (mi == mapBlockIndex.end() || mi->second == NULL)
            return state.DoS(0, error("%s : prev block %s not found", __func__, block.hashPrevBlock.ToString().c_str()),
                0, "bad-prevblk");
        pindexPrev = (*mi).second;
        if (pindexPrev->nStatus & BLOCK_FAILED_MASK) {
            //If this "invalid" block is an exact match from the checkpoints or if it is a PoA block rejected previously due not synced with other nodes before, then reconsider it
            if (Checkpoints::CheckBlock(pindexPrev->nHeight, block.hashPrevBlock, true) || (pindexPrev->IsProofOfAudit() && chainActive.Height() - pindexPrev->nHeight < Params().MaxReorganizationDepth())) {
                LogPrintf("%s : Reconsidering block %s height %d\n", __func__, pindexPrev->GetBlockHash().GetHex(),
                    pindexPrev->nHeight);
                CValidationState statePrev;
                ReconsiderBlock(statePrev, pindexPrev);
                if (statePrev.IsValid()) {
                    ActivateBestChain(statePrev);
                    return true;
                }
            }
            return state.DoS(100, error("%s : prev block %s is invalid, unable to add block %s", __func__, block.hashPrevBlock.GetHex(), block.GetHash().GetHex()),
                REJECT_INVALID, "bad-prevblk");
        }
    }
    if (block.GetHash() != Params().HashGenesisBlock() && !CheckWork(block, pindexPrev)) {
        return false;
    }

    bool isPoS = false;
    if (block.IsProofOfStake()) {
        isPoS = true;
        uint256 hashProofOfStake = 0;
        std::unique_ptr<CStakeInput> stake;

        if (!CheckProofOfStake(block, hashProofOfStake, stake, pindexPrev->nHeight))
            return state.DoS(100, error("%s: proof of stake check failed", __func__));

        if (!stake)
            return error("%s: null stake ptr", __func__);

        uint256 hash = block.GetHash();
        if(!mapProofOfStake.count(hash)) // add to mapProofOfStake
            mapProofOfStake.insert(std::make_pair(hash, hashProofOfStake));
    }

    if (!AcceptBlockHeader(block, state, &pindex))
        return false;

    if (pindex->nStatus & BLOCK_HAVE_DATA) {
        // TODO: deal better with duplicate blocks.
        LogPrintf("AcceptBlock() : already have block %d %s", pindex->nHeight, pindex->GetBlockHash().ToString());
        return true;
    }

    if ((!fAlreadyCheckedBlock && !CheckBlock(block, state)) || !ContextualCheckBlock(block, state, pindex->pprev)) {
        if (state.IsInvalid() && !state.CorruptionPossible()) {
            pindex->nStatus |= BLOCK_FAILED_VALID;
            setDirtyBlockIndex.insert(pindex);
        }
        return false;
    }

    int nHeight = pindex->nHeight;
    int splitHeight = -1;

    if (isPoS) {
        LOCK(cs_main);

        // Blocks arrives in order, so if prev block is not the tip then we are on a fork.
        // Extra info: duplicated blocks are skipping this checks, so we don't have to worry about those here.
        bool isBlockFromFork = pindexPrev != nullptr && chainActive.Tip() != pindexPrev;

        // Coin stake
        CTransaction &stakeTxIn = block.vtx[1];

        // Only one input is allowed
        if (stakeTxIn.vin.size() > 1)
            return state.DoS(100, error("%s : multiple stake inputs not allowed", __func__), REJECT_INVALID, "bad-txns-stake");

        // Inputs
        std::vector<CTxIn> prcyInputs;

        for (const CTxIn& stakeIn : stakeTxIn.vin) {
            prcyInputs.push_back(stakeIn);
        }
        const bool hasPRCYInputs = !prcyInputs.empty();

        for (const CTransaction& tx : block.vtx) {
            for (const CTxIn& in: tx.vin) {
                if(tx.IsCoinStake()) continue;
                if(hasPRCYInputs)
                    // Check if coinstake input is double spent inside the same block
                    for (const CTxIn& prcyIn : prcyInputs){
                        if (IsSpentKeyImage(prcyIn.keyImage.GetHex(), block.GetHash())) {
                            // double spent coinstake input inside block
                            return error("%s: double spent coinstake input: %s, KeyImage: %s inside block: %s", __func__, prcyIn.prevout.hash.GetHex(), prcyIn.keyImage.GetHex(), block.GetHash().GetHex());
                        }
                    }
            }
        }

        // Check whether is a fork or not
        if (isBlockFromFork) {

            // Start at the block we're adding on to
            CBlockIndex *prev = pindexPrev;

            int readBlock = 0;
            CBlock bl;
            // Go backwards on the forked chain up to the split
            do {
                // Check if the forked chain is longer than the max reorg limit
                if(readBlock == Params().MaxReorganizationDepth()){
                    // TODO: Remove this chain from disk.
                    return error("%s: forked chain longer than maximum reorg limit", __func__);
                }

                if(!ReadBlockFromDisk(bl, prev))
                    // Previous block not on disk
                    return error("%s: previous block %s not on disk", __func__, prev->GetBlockHash().GetHex());
                // Increase amount of read blocks
                readBlock++;
                // Loop through every input from said block
                for (const CTransaction& t : bl.vtx) {
                    for (const CTxIn& in: t.vin) {
                        // Loop through every input of the staking tx
                        for (const CTxIn& stakeIn : prcyInputs) {
                            // if it's already spent

                            // First regular staking check
                            if(hasPRCYInputs) {
                                if (IsSpentKeyImage(stakeIn.keyImage.GetHex(), bl.GetHash())) {
                                    return state.DoS(100, error("%s: input: %s already spent on a previous block: %s", __func__, stakeIn.keyImage.GetHex(), bl.GetHash().GetHex()));
                                }
                            }
                        }
                    }
                }

                prev = prev->pprev;

            } while (!chainActive.Contains(prev));
        }


        // Let's check if the inputs were spent on the main chain
        const CCoinsViewCache coins(pcoinsTip);
        for (const CTxIn& in: stakeTxIn.vin) {
            const CCoins* coin = coins.AccessCoins(in.prevout.hash);

            if(!coin && !isBlockFromFork){
                // No coins on the main chain
                return error("%s: coin stake inputs not available on main chain, received height %d vs current %d", __func__, nHeight, chainActive.Height());
            }
            if(coin && !coin->IsAvailable(in.prevout.n)){
                // If this is not available get the height of the spent and validate it with the forked height
                // Check if this occurred before the chain split
                if(!(isBlockFromFork && coin->nHeight > splitHeight)){
                    // Coins not available
                    return error("%s: coin stake inputs already spent in main chain", __func__);
                }
            }
        }

    }

    // Write block to history file
    try {
        unsigned int nBlockSize = ::GetSerializeSize(block, SER_DISK, CLIENT_VERSION);
        CDiskBlockPos blockPos;
        if (dbp != NULL)
            blockPos = *dbp;
        if (!FindBlockPos(state, blockPos, nBlockSize + 8, nHeight, block.GetBlockTime(), dbp != NULL))
            return error("AcceptBlock() : FindBlockPos failed");
        if (dbp == NULL)
            if (!WriteBlockToDisk(block, blockPos))
                return AbortNode(state, "Failed to write block");
        if (!ReceivedBlockTransactions(block, state, pindex, blockPos))
            return error("AcceptBlock() : ReceivedBlockTransactions failed");
    } catch (const std::runtime_error& e) {
        return AbortNode(state, std::string("System error: ") + e.what());
    }

    return true;
}

bool CBlockIndex::IsSuperMajority(int minVersion, const CBlockIndex* pstart, unsigned int nRequired)
{
    unsigned int nToCheck = Params().ToCheckBlockUpgradeMajority();
    unsigned int nFound = 0;
    for (unsigned int i = 0; i < nToCheck && nFound < nRequired && pstart != NULL; i++) {
        if (pstart->nVersion >= minVersion)
            ++nFound;
        pstart = pstart->pprev;
    }
    return (nFound >= nRequired);
}

/** Turn the lowest '1' bit in the binary representation of a number into a '0'. */
int static inline InvertLowestOne(int n) { return n & (n - 1); }

/** Compute what height to jump back to with the CBlockIndex::pskip pointer. */
int static inline GetSkipHeight(int height)
{
    if (height < 2)
        return 0;

    // Determine which height to jump back to. Any number strictly lower than height is acceptable,
    // but the following expression seems to perform well in simulations (max 110 steps to go back
    // up to 2**18 blocks).
    return (height & 1) ? InvertLowestOne(InvertLowestOne(height - 1)) + 1 : InvertLowestOne(height);
}

CBlockIndex* CBlockIndex::GetAncestor(int height)
{
    if (height > nHeight || height < 0)
        return NULL;

    CBlockIndex* pindexWalk = this;
    int heightWalk = nHeight;
    while (heightWalk > height) {
        int heightSkip = GetSkipHeight(heightWalk);
        int heightSkipPrev = GetSkipHeight(heightWalk - 1);
        if (heightSkip == height ||
            (heightSkip > height && !(heightSkipPrev < heightSkip - 2 && heightSkipPrev >= height))) {
            // Only follow pskip if pprev->pskip isn't better than pskip->pprev.
            pindexWalk = pindexWalk->pskip;
            heightWalk = heightSkip;
        } else {
            pindexWalk = pindexWalk->pprev;
            heightWalk--;
        }
    }
    return pindexWalk;
}

const CBlockIndex* CBlockIndex::GetAncestor(int height) const
{
    return const_cast<CBlockIndex*>(this)->GetAncestor(height);
}

void CBlockIndex::BuildSkip()
{
    if (pprev)
        pskip = pprev->GetAncestor(GetSkipHeight(nHeight));
}

bool ProcessNewBlock(CValidationState& state, CNode* pfrom, CBlock* pblock, CDiskBlockPos* dbp)
{
    AssertLockNotHeld(cs_main);

    // Preliminary checks
    int64_t nStartTime = GetTimeMillis();
    bool checked = CheckBlock(*pblock, state);

    if (!pblock->IsPoABlockByVersion() && !CheckBlockSignature(*pblock))
        return error("ProcessNewBlock() : bad proof-of-stake block signature");

    if (pblock->GetHash() != Params().HashGenesisBlock() && pfrom != NULL) {
        //if we get this far, check if the prev block is our prev block, if not then request sync and return false
        BlockMap::iterator mi = mapBlockIndex.find(pblock->hashPrevBlock);
        if (mi == mapBlockIndex.end() || (mi != mapBlockIndex.end() && mi->second == NULL)) {
            mapBlockIndex.erase(pblock->hashPrevBlock);
            pfrom->PushMessage(NetMsgType::GETBLOCKS, chainActive.GetLocator(), UINT256_ZERO);
            return false;
        } else {
            CBlock r;
            if (!ReadBlockFromDisk(r, mapBlockIndex[pblock->hashPrevBlock])) {
                pfrom->PushMessage(NetMsgType::GETBLOCKS, chainActive.GetLocator(), UINT256_ZERO);
                return false;
            }
        }
    }
    {
        LOCK(cs_main); // Replaces the former TRY_LOCK loop because busy waiting wastes too much resources

        MarkBlockAsReceived(pblock->GetHash());
        if (!checked) {
            return error("%s : CheckBlock FAILED for block %s", __func__, pblock->GetHash().GetHex());
        }

        // Store to disk
        CBlockIndex* pindex = nullptr;
        bool ret = AcceptBlock(*pblock, state, &pindex, dbp, checked);
        if (pindex && pfrom) {
            mapBlockSource[pindex->GetBlockHash()] = pfrom->GetId();
        }
        CheckBlockIndex();
        if (!ret) {
            if (pfrom) {
                pfrom->PushMessage(NetMsgType::GETBLOCKS, chainActive.GetLocator(pindexBestForkTip), pblock->GetHash());
            }
            if (pwalletMain)
            {
                LOCK(pwalletMain->cs_wallet);
                if (pblock->IsProofOfStake()) {
                    if (pwalletMain->IsMine(pblock->vtx[1].vin[0])) {
                        pwalletMain->mapWallet.erase(pblock->vtx[1].GetHash());
                    }
                }
            }
            // Check spamming
            if(pindex && pfrom && GetBoolArg("-blockspamfilter", DEFAULT_BLOCK_SPAM_FILTER)) {
                CNodeState *nodestate = State(pfrom->GetId());
                if(nodestate != nullptr) {
                    nodestate->nodeBlocks.onBlockReceived(pindex->nHeight);
                    bool nodeStatus = true;
                    // UpdateState will return false if the node is attacking us or update the score and return true.
                    nodeStatus = nodestate->nodeBlocks.updateState(state, nodeStatus);
                    int nDoS = 0;
                    if (state.IsInvalid(nDoS)) {
                        if (nDoS > 0)
                            Misbehaving(pfrom->GetId(), nDoS);
                        nodeStatus = false;
                    }
                    if (!nodeStatus)
                        return error("%s : AcceptBlock FAILED - block spam protection", __func__);
                }
            }
            return error("%s : AcceptBlock FAILED", __func__);
        }
        bool initialDownloadCheck = IsInitialBlockDownload();
        if (!initialDownloadCheck &&
            pblock->IsPoABlockByVersion()) // Run DeleteWalletTransactions on PoA blocks
        {
            pwalletMain->DeleteWalletTransactions(pindex);
        } else {
            if (initialDownloadCheck && pindex->nHeight % fDeleteInterval == 0) {
                pwalletMain->DeleteWalletTransactions(pindex);
            }
        }
    }

    if (!ActivateBestChain(state, pblock, checked))
        return error("%s : ActivateBestChain failed", __func__);
    if (!fLiteMode) {
        if (masternodeSync.RequestedMasternodeAssets > MASTERNODE_SYNC_LIST) {
            masternodePayments.ProcessBlock(GetHeight() + 10);
            budget.NewBlock();
        }
    }

    if (pwalletMain) {
        LOCK2(cs_main, pwalletMain->cs_wallet);
        /*// If turned on MultiSend will send a transaction (or more) on the after maturity of a stake
        if (pwalletMain->isMultiSendEnabled())
            pwalletMain->MultiSend();*/

        // If turned on Auto Combine will scan wallet for dust to combine
        if (pwalletMain->fCombineDust && chainActive.Height() % 15 == 0)
            pwalletMain->AutoCombineDust();

        if (chainActive.Height() % 15 == 0) {
            RemoveInvalidTransactionsFromMempool();
        }
        pwalletMain->resetPendingOutPoints();
    }

    //Block is accepted, let's update decoys pool
    //First, update user decoy pool
    int userTxStartIdx = 1;
    int coinbaseIdx = 0;
    if (pwalletMain) {
        LOCK2(cs_main, pwalletMain->cs_wallet);
        {
            if (pblock->IsProofOfStake()) {
                userTxStartIdx = 2;
                coinbaseIdx = 1;
            }

            if (pblock->IsProofOfStake()) {
                if (pwalletMain->userDecoysPool.count(pblock->vtx[1].vin[0].prevout) == 1) {
                    pwalletMain->userDecoysPool.erase(pblock->vtx[1].vin[0].prevout);
                }

                if (pwalletMain->coinbaseDecoysPool.count(pblock->vtx[1].vin[0].prevout) == 1) {
                    pwalletMain->coinbaseDecoysPool.erase(pblock->vtx[1].vin[0].prevout);
                }
            }

            if ((int)pblock->vtx.size() > userTxStartIdx) {
                for (int i = userTxStartIdx; i < (int)pblock->vtx.size(); i++) {
                    for (int j = 0; j < (int)pblock->vtx[i].vout.size(); j++) {
                        if (!pblock->vtx[i].vout[j].commitment.empty() && (secp256k1_rand32() % 100) <= CWallet::PROBABILITY_NEW_COIN_SELECTED) {
                            COutPoint newOutPoint(pblock->vtx[i].GetHash(), j);
                            if (pwalletMain->userDecoysPool.count(newOutPoint) == 1) {
                                continue;
                            }
                            //add new user transaction to the pool
                            if ((int32_t)pwalletMain->userDecoysPool.size() >= CWallet::MAX_DECOY_POOL) {
                                int selected = secp256k1_rand32() % CWallet::MAX_DECOY_POOL;
                                std::map<COutPoint, uint256>::const_iterator it = std::next(pwalletMain->userDecoysPool.begin(), selected);
                                pwalletMain->userDecoysPool.erase(it->first);
                                pwalletMain->userDecoysPool[newOutPoint] = pblock->GetHash();
                            } else {
                                pwalletMain->userDecoysPool[newOutPoint] = pblock->GetHash();
                            }
                        }
                    }
                }
            }

            if (chainActive.Height() > Params().COINBASE_MATURITY()) {
                //read block chainActive.Height() - Params().COINBASE_MATURITY()
                CBlockIndex* p = chainActive[chainActive.Height() - Params().COINBASE_MATURITY()];
                CBlock b;
                if (ReadBlockFromDisk(b, p)) {
                    coinbaseIdx = 0;
                    if (p->IsProofOfStake()) {
                        coinbaseIdx = 1;
                    }
                    CTransaction& coinbase = b.vtx[coinbaseIdx];
                    if (b.posBlocksAudited.size() == 0) {
                        for (int i = 0; i < (int)coinbase.vout.size(); i++) {
                            if (!coinbase.vout[i].IsNull() && !coinbase.vout[i].commitment.empty() && coinbase.vout[i].nValue > 0 && !coinbase.vout[i].IsEmpty()) {
                                if ((secp256k1_rand32() % 100) <= CWallet::PROBABILITY_NEW_COIN_SELECTED) {
                                    COutPoint newOutPoint(coinbase.GetHash(), i);
                                    if (pwalletMain->coinbaseDecoysPool.count(newOutPoint) == 1) {
                                        continue;
                                    }
                                    //add new coinbase transaction to the pool
                                    if ((int)pwalletMain->coinbaseDecoysPool.size() >= CWallet::MAX_DECOY_POOL) {
                                        int selected = secp256k1_rand32() % CWallet::MAX_DECOY_POOL;
                                        std::map<COutPoint, uint256>::const_iterator it = std::next(pwalletMain->coinbaseDecoysPool.begin(), selected);
                                        pwalletMain->coinbaseDecoysPool.erase(it->first);
                                        pwalletMain->coinbaseDecoysPool[newOutPoint] = pblock->GetHash();
                                    } else {
                                        pwalletMain->coinbaseDecoysPool[newOutPoint] = pblock->GetHash();
                                    }
                                }
                            }
                        }
                    }
                }
            }
            LogPrintf("%s: Coinbase decoys = %d, user decoys = %d\n", __func__, pwalletMain->coinbaseDecoysPool.size(), pwalletMain->userDecoysPool.size());
        }
    }

    LogPrintf("%s: ACCEPTED in %ld milliseconds with size=%d, height=%d\n", __func__, GetTimeMillis() - nStartTime,
        pblock->GetSerializeSize(SER_DISK, CLIENT_VERSION), chainActive.Height());

    return true;
}

bool TestBlockValidity(CValidationState& state, const CBlock& block, CBlockIndex* const pindexPrev, bool fCheckPOW, bool fCheckMerkleRoot)
{
    AssertLockHeld(cs_main);
    assert(pindexPrev && pindexPrev == chainActive.Tip());
    if (pindexPrev != chainActive.Tip()) {
        LogPrintf("%s : No longer working on chain tip\n", __func__);
        return false;
    }

    CCoinsViewCache viewNew(pcoinsTip);
    CBlockIndex indexDummy(block);
    indexDummy.pprev = pindexPrev;
    indexDummy.nHeight = pindexPrev->nHeight + 1;

    // NOTE: CheckBlockHeader is called by CheckBlock
    if (!ContextualCheckBlockHeader(block, state, pindexPrev))
        return false;
    if (!CheckBlock(block, state, fCheckPOW, fCheckMerkleRoot))
        return false;
    if (!ContextualCheckBlock(block, state, pindexPrev))
        return false;
    if (!ConnectBlock(block, state, &indexDummy, viewNew, true))
        return false;
    assert(state.IsValid());

    return true;
}

bool CheckDiskSpace(uint64_t nAdditionalBytes)
{
    uint64_t nFreeBytesAvailable = fs::space(GetDataDir()).available;

    // Check for nMinDiskSpace bytes (currently 50MB)
    if (nFreeBytesAvailable < nMinDiskSpace + nAdditionalBytes)
        return AbortNode("Disk space is low!", _("Error: Disk space is low!"));

    return true;
}

FILE* OpenDiskFile(const CDiskBlockPos& pos, const char* prefix, bool fReadOnly)
{
    if (pos.IsNull())
        return NULL;
    fs::path path = GetBlockPosFilename(pos, prefix);
    fs::create_directories(path.parent_path());
    FILE* file = fsbridge::fopen(path, "rb+");
    if (!file && !fReadOnly)
        file = fsbridge::fopen(path, "wb+");
    if (!file) {
        LogPrintf("Unable to open file %s\n", path.string());
        return NULL;
    }
    if (pos.nPos) {
        if (fseek(file, pos.nPos, SEEK_SET)) {
            LogPrintf("Unable to seek to position %u of %s\n", pos.nPos, path.string());
            fclose(file);
            return NULL;
        }
    }
    return file;
}

FILE* OpenBlockFile(const CDiskBlockPos& pos, bool fReadOnly)
{
    return OpenDiskFile(pos, "blk", fReadOnly);
}

FILE* OpenUndoFile(const CDiskBlockPos& pos, bool fReadOnly)
{
    return OpenDiskFile(pos, "rev", fReadOnly);
}

fs::path GetBlockPosFilename(const CDiskBlockPos& pos, const char* prefix)
{
    return GetDataDir() / "blocks" / strprintf("%s%05u.dat", prefix, pos.nFile);
}

CBlockIndex* InsertBlockIndex(uint256 hash)
{
    if (hash.IsNull())
        return NULL;

    // Return existing
    BlockMap::iterator mi = mapBlockIndex.find(hash);
    if (mi != mapBlockIndex.end())
        return (*mi).second;

    // Create new
    CBlockIndex* pindexNew = new CBlockIndex();
    if (!pindexNew)
        throw std::runtime_error("LoadBlockIndex() : new CBlockIndex failed");
    mi = mapBlockIndex.insert(std::make_pair(hash, pindexNew)).first;

    pindexNew->phashBlock = &((*mi).first);

    return pindexNew;
}

bool static LoadBlockIndexDB(std::string& strError)
{
    if (!pblocktree->LoadBlockIndexGuts())
        return false;

    boost::this_thread::interruption_point();

    // Calculate nChainWork
    std::vector<std::pair<int, CBlockIndex*> > vSortedByHeight;
    vSortedByHeight.reserve(mapBlockIndex.size());
    for (const std::pair<const uint256, CBlockIndex*>& item : mapBlockIndex) {
        CBlockIndex* pindex = item.second;
        vSortedByHeight.push_back(std::make_pair(pindex->nHeight, pindex));
    }
    std::sort(vSortedByHeight.begin(), vSortedByHeight.end());
    for (const PAIRTYPE(int, CBlockIndex*) & item : vSortedByHeight) {
        // Stop if shutdown was requested
        if (ShutdownRequested()) return false;

        CBlockIndex* pindex = item.second;
        pindex->nChainWork = (pindex->pprev ? pindex->pprev->nChainWork : 0) + GetBlockProof(*pindex);
        if (pindex->nStatus & BLOCK_HAVE_DATA) {
            if (pindex->pprev) {
                if (pindex->pprev->nChainTx) {
                    pindex->nChainTx = pindex->pprev->nChainTx + pindex->nTx;
                } else {
                    pindex->nChainTx = 0;
                    mapBlocksUnlinked.insert(std::make_pair(pindex->pprev, pindex));
                }
            } else {
                pindex->nChainTx = pindex->nTx;
            }
        }
        if (pindex->IsValid(BLOCK_VALID_TRANSACTIONS) && (pindex->nChainTx || pindex->pprev == NULL))
            setBlockIndexCandidates.insert(pindex);
        if (pindex->nStatus & BLOCK_FAILED_MASK &&
            (!pindexBestInvalid || pindex->nChainWork > pindexBestInvalid->nChainWork))
            pindexBestInvalid = pindex;
        if (pindex->pprev)
            pindex->BuildSkip();
        if (pindex->IsValid(BLOCK_VALID_TREE) &&
            (pindexBestHeader == NULL || CBlockIndexWorkComparator()(pindexBestHeader, pindex)))
            pindexBestHeader = pindex;
    }

    // Load block file info
    pblocktree->ReadLastBlockFile(nLastBlockFile);
    vinfoBlockFile.resize(nLastBlockFile + 1);
    LogPrintf("%s: last block file = %i\n", __func__, nLastBlockFile);
    for (int nFile = 0; nFile <= nLastBlockFile; nFile++) {
        pblocktree->ReadBlockFileInfo(nFile, vinfoBlockFile[nFile]);
    }
    LogPrintf("%s: last block file info: %s\n", __func__, vinfoBlockFile[nLastBlockFile].ToString());
    for (int nFile = nLastBlockFile + 1; true; nFile++) {
        CBlockFileInfo info;
        if (pblocktree->ReadBlockFileInfo(nFile, info)) {
            vinfoBlockFile.push_back(info);
        } else {
            break;
        }
    }

    // Check presence of blk files
    LogPrintf("Checking all blk files are present...\n");
    std::set<int> setBlkDataFiles;
    for (const std::pair<const uint256, CBlockIndex*>& item : mapBlockIndex) {
        CBlockIndex* pindex = item.second;
        if (pindex->nStatus & BLOCK_HAVE_DATA) {
            setBlkDataFiles.insert(pindex->nFile);
        }
    }
    for (std::set<int>::iterator it = setBlkDataFiles.begin(); it != setBlkDataFiles.end(); it++) {
        CDiskBlockPos pos(*it, 0);
        if (CAutoFile(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION).IsNull()) {
            return false;
        }
    }

    //Check if the shutdown procedure was followed on last client exit
    bool fLastShutdownWasPrepared = true;
    pblocktree->ReadFlag("shutdown", fLastShutdownWasPrepared);
    LogPrintf("%s: Last shutdown was prepared: %s\n", __func__, fLastShutdownWasPrepared);

    // Check whether we need to continue reindexing
    bool fReindexing = false;
    pblocktree->ReadReindexing(fReindexing);
    if(fReindexing) fReindex = true;

    // Check whether we have a transaction index
    pblocktree->ReadFlag("txindex", fTxIndex);
    LogPrintf("LoadBlockIndexDB(): transaction index %s\n", fTxIndex ? "enabled" : "disabled");

    // If this is written true before the next client init, then we know the shutdown process failed
    pblocktree->WriteFlag("shutdown", false);

    // Load pointer to end of best chain
    BlockMap::iterator it = mapBlockIndex.find(pcoinsTip->GetBestBlock());
    if (it == mapBlockIndex.end())
        return true;
    chainActive.SetTip(it->second);

    PruneBlockIndexCandidates();

    const CBlockIndex* pChainTip = chainActive.Tip();
    LogPrintf("LoadBlockIndexDB(): hashBestChain=%s height=%d date=%s progress=%f\n",
              pChainTip->GetBlockHash().GetHex(), pChainTip->nHeight,
              DateTimeStrFormat("%Y-%m-%d %H:%M:%S", pChainTip->GetBlockTime()),
              Checkpoints::GuessVerificationProgress(pChainTip));

    return true;
}

CVerifyDB::CVerifyDB()
{
    uiInterface.ShowProgress(_("Verifying blocks..."), 0);
}

CVerifyDB::~CVerifyDB()
{
    uiInterface.ShowProgress("", 100);
}

bool CVerifyDB::VerifyDB(CCoinsView* coinsview, int nCheckLevel, int nCheckDepth)
{
    LOCK(cs_main);
    if (chainActive.Tip() == NULL || chainActive.Tip()->pprev == NULL)
        return true;

    const int chainHeight = chainActive.Height();
    // Verify blocks in the best chain
    if (nCheckDepth <= 0)
        nCheckDepth = 1000000000; // suffices until the year 19000
    if (nCheckDepth > chainHeight)
        nCheckDepth = chainHeight;
    nCheckLevel = std::max(0, std::min(4, nCheckLevel));
    LogPrintf("Verifying last %i blocks at level %i\n", nCheckDepth, nCheckLevel);
    CCoinsViewCache coins(coinsview);
    CBlockIndex* pindexState = chainActive.Tip();
    CBlockIndex* pindexFailure = NULL;
    int nGoodTransactions = 0;
    CValidationState state;
    for (CBlockIndex* pindex = chainActive.Tip(); pindex && pindex->pprev; pindex = pindex->pprev) {
        boost::this_thread::interruption_point();
        uiInterface.ShowProgress(_("Verifying blocks..."), std::max(1, std::min(99, (int)(((double)(chainHeight - pindex->nHeight)) / (double)nCheckDepth * (nCheckLevel >= 4 ? 50 : 100)))));
        if (pindex->nHeight < chainHeight - nCheckDepth)
            break;
        CBlock block;
        // check level 0: read from disk
        if (!ReadBlockFromDisk(block, pindex))
            return error("VerifyDB() : *** ReadBlockFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
        // check level 1: verify block validity
        if (nCheckLevel >= 1 && !CheckBlock(block, state))
            return error("VerifyDB() : *** found bad block at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
        // check level 2: verify undo validity
        if (nCheckLevel >= 2 && pindex) {
            CBlockUndo undo;
            CDiskBlockPos pos = pindex->GetUndoPos();
            if (!pos.IsNull()) {
                if (!undo.ReadFromDisk(pos, pindex->pprev->GetBlockHash()))
                    return error("VerifyDB() : *** found bad undo data at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
            }
        }
        // check level 3: check for inconsistencies during memory-only disconnect of tip blocks
        if (nCheckLevel >= 3 && pindex == pindexState && (coins.DynamicMemoryUsage() + pcoinsTip->DynamicMemoryUsage()) <= nCoinCacheUsage) {
            bool fClean = true;
            if (!DisconnectBlock(block, state, pindex, coins, &fClean))
                return error("VerifyDB() : *** irrecoverable inconsistency in block data at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
            pindexState = pindex->pprev;
            if (!fClean) {
                nGoodTransactions = 0;
                pindexFailure = pindex;
            } else
                nGoodTransactions += block.vtx.size();
        }
        if (ShutdownRequested())
            return true;
    }
    if (pindexFailure)
        return error("VerifyDB() : *** coin database inconsistencies found (last %i blocks, %i good transactions before that)\n", chainHeight - pindexFailure->nHeight + 1, nGoodTransactions);

    // check level 4: try reconnecting blocks
    if (nCheckLevel >= 4) {
        CBlockIndex* pindex = pindexState;
        while (pindex != chainActive.Tip()) {
            boost::this_thread::interruption_point();
            uiInterface.ShowProgress(_("Verifying blocks..."), std::max(1, std::min(99, 100 - (int)(((double)(chainHeight - pindex->nHeight)) / (double)nCheckDepth * 50))));
            pindex = chainActive.Next(pindex);
            CBlock block;
            if (!ReadBlockFromDisk(block, pindex))
                return error("VerifyDB() : *** ReadBlockFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
            if (!ConnectBlock(block, state, pindex, coins, false))
                return error("VerifyDB() : *** found unconnectable block at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
        }
    }

    LogPrintf("No coin database inconsistencies in last %i blocks (%i transactions)\n", chainHeight - pindexState->nHeight, nGoodTransactions);

    return true;
}

void UnloadBlockIndex()
{
    LOCK(cs_main);
    setBlockIndexCandidates.clear();
    chainActive.SetTip(NULL);
    pindexBestInvalid = NULL;
    pindexBestHeader = NULL;
    mempool.clear();
    mapOrphanTransactions.clear();
    mapOrphanTransactionsByPrev.clear();
    nSyncStarted = 0;
    mapBlocksUnlinked.clear();
    vinfoBlockFile.clear();
    nLastBlockFile = 0;
    nBlockSequenceId = 1;
    mapBlockSource.clear();
    mapBlocksInFlight.clear();
    nQueuedValidatedHeaders = 0;
    nPreferredDownload = 0;
    setDirtyBlockIndex.clear();
    setDirtyFileInfo.clear();
    mapNodeState.clear();
    recentRejects.reset(nullptr);

    for (BlockMap::value_type& entry : mapBlockIndex) {
        delete entry.second;
    }
    mapBlockIndex.clear();
}

bool LoadBlockIndex(std::string& strError)
{
    // Load block index from databases
    if (!fReindex && !LoadBlockIndexDB(strError))
        return false;
    return true;
}


bool InitBlockIndex()
{
    LOCK(cs_main);

    // Initialize global variables that cannot be constructed at startup.
    recentRejects.reset(new CRollingBloomFilter(120000, 0.000001));

    // Check whether we're already initialized
    if (chainActive.Genesis() != NULL)
        return true;

    // Use the provided setting for -txindex in the new database
    fTxIndex = GetBoolArg("-txindex", true);
    pblocktree->WriteFlag("txindex", fTxIndex);
    LogPrintf("Initializing databases...\n");

    // Only add the genesis block if not reindexing (in which case we reuse the one already on disk)
    if (!fReindex) {
        try {
            CBlock& block = const_cast<CBlock&>(Params().GenesisBlock());
            // Start new block file
            unsigned int nBlockSize = ::GetSerializeSize(block, SER_DISK, CLIENT_VERSION);
            CDiskBlockPos blockPos;
            CValidationState state;
            if (!FindBlockPos(state, blockPos, nBlockSize + 8, 0, block.GetBlockTime()))
                return error("LoadBlockIndex() : FindBlockPos failed");
            if (!WriteBlockToDisk(block, blockPos))
                return error("LoadBlockIndex() : writing genesis block to disk failed");
            CBlockIndex* pindex = AddToBlockIndex(block);
            if (!ReceivedBlockTransactions(block, state, pindex, blockPos))
                return error("LoadBlockIndex() : genesis block not accepted");
            // Force a chainstate write so that when we VerifyDB in a moment, it doesnt check stale data
            return FlushStateToDisk(state, FLUSH_STATE_ALWAYS);
        } catch (const std::runtime_error& e) {
            return error("LoadBlockIndex() : failed to initialize block database: %s", e.what());
        }
    }

    return true;
}


bool LoadExternalBlockFile(FILE* fileIn, CDiskBlockPos* dbp)
{
    // Map of disk positions for blocks with unknown parent (only used for reindex)
    static std::multimap<uint256, CDiskBlockPos> mapBlocksUnknownParent;
    int64_t nStart = GetTimeMillis();

    int nLoaded = 0;
    try {
        // This takes over fileIn and calls fclose() on it in the CBufferedFile destructor
        CBufferedFile blkdat(fileIn, 2 * MAX_BLOCK_SIZE_CURRENT, MAX_BLOCK_SIZE_CURRENT + 8, SER_DISK, CLIENT_VERSION);
        uint64_t nRewind = blkdat.GetPos();
        while (!blkdat.eof()) {
            boost::this_thread::interruption_point();

            blkdat.SetPos(nRewind);
            nRewind++;         // start one byte further next time, in case of failure
            blkdat.SetLimit(); // remove former limit
            unsigned int nSize = 0;
            try {
                // locate a header
                unsigned char buf[MESSAGE_START_SIZE];
                blkdat.FindByte(Params().MessageStart()[0]);
                nRewind = blkdat.GetPos() + 1;
                blkdat >> FLATDATA(buf);
                if (memcmp(buf, Params().MessageStart(), MESSAGE_START_SIZE))
                    continue;
                // read size
                blkdat >> nSize;
                if (nSize < 80 || nSize > MAX_BLOCK_SIZE_CURRENT)
                    continue;
            } catch (const std::exception&) {
                // no valid block header found; don't complain
                break;
            }
            try {
                // read block
                uint64_t nBlockPos = blkdat.GetPos();
                if (dbp)
                    dbp->nPos = nBlockPos;
                blkdat.SetLimit(nBlockPos + nSize);
                blkdat.SetPos(nBlockPos);
                CBlock block;
                blkdat >> block;
                nRewind = blkdat.GetPos();

                // detect out of order blocks, and store them for later
                uint256 hash = block.GetHash();
                if (hash != Params().HashGenesisBlock() &&
                    mapBlockIndex.find(block.hashPrevBlock) == mapBlockIndex.end()) {
                    LogPrint(BCLog::REINDEX, "%s: Out of order block %s, parent %s not known\n", __func__,
                            hash.GetHex(), block.hashPrevBlock.GetHex());
                    if (dbp)
                        mapBlocksUnknownParent.insert(std::make_pair(block.hashPrevBlock, *dbp));
                    continue;
                }

                // process in case the block isn't known yet
                if (mapBlockIndex.count(hash) == 0 || (mapBlockIndex[hash] == NULL) || (mapBlockIndex[hash]->nStatus & BLOCK_HAVE_DATA) == 0) {
                    CValidationState state;
                    if (ProcessNewBlock(state, NULL, &block, dbp))
                        nLoaded++;
                    if (state.IsError())
                        break;
                } else if (hash != Params().HashGenesisBlock() && mapBlockIndex[hash]->nHeight % 1000 == 0) {
                    LogPrintf("Block Import: already had block %s at height %d\n", hash.ToString(),
                        mapBlockIndex[hash]->nHeight);
                }

                // Recursively process earlier encountered successors of this block
                std::deque<uint256> queue;
                queue.push_back(hash);
                while (!queue.empty()) {
                    uint256 head = queue.front();
                    queue.pop_front();
                    std::pair<std::multimap<uint256, CDiskBlockPos>::iterator, std::multimap<uint256, CDiskBlockPos>::iterator> range = mapBlocksUnknownParent.equal_range(
                        head);
                    while (range.first != range.second) {
                        std::multimap<uint256, CDiskBlockPos>::iterator it = range.first;
                        if (ReadBlockFromDisk(block, it->second)) {
                            LogPrintf("%s: Processing out of order child %s of %s\n", __func__,
                                block.GetHash().ToString(),
                                head.ToString());
                            CValidationState dummy;
                            if (ProcessNewBlock(dummy, NULL, &block, &it->second)) {
                                nLoaded++;
                                queue.push_back(block.GetHash());
                            }
                        }
                        range.first++;
                        mapBlocksUnknownParent.erase(it);
                    }
                }
            } catch (const std::exception& e) {
                LogPrintf("%s : Deserialize or I/O error - %s\n", __func__, e.what());
            }
        }
    } catch (const std::runtime_error& e) {
        AbortNode(std::string("System error: ") + e.what());
    }
    if (nLoaded > 0)
        LogPrintf("Loaded %i blocks from external file in %dms\n", nLoaded, GetTimeMillis() - nStart);
    return nLoaded > 0;
}

void static CheckBlockIndex()
{
    if (!fCheckBlockIndex) {
        return;
    }

    LOCK(cs_main);

    // During a reindex, we read the genesis block and call CheckBlockIndex before ActivateBestChain,
    // so we have the genesis block in mapBlockIndex but no active chain.  (A few of the tests when
    // iterating the block tree require that chainActive has been initialized.)
    if (chainActive.Height() < 0) {
        assert(mapBlockIndex.size() <= 1);
        return;
    }

    // Build forward-pointing map of the entire block tree.
    std::multimap<CBlockIndex*, CBlockIndex*> forward;
    for (BlockMap::iterator it = mapBlockIndex.begin(); it != mapBlockIndex.end(); it++) {
        forward.insert(std::make_pair(it->second->pprev, it->second));
    }

    assert(forward.size() == mapBlockIndex.size());

    std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> rangeGenesis = forward.equal_range(
        NULL);
    CBlockIndex* pindex = rangeGenesis.first->second;
    rangeGenesis.first++;
    assert(rangeGenesis.first == rangeGenesis.second); // There is only one index entry with parent NULL.

    // Iterate over the entire block tree, using depth-first search.
    // Along the way, remember whether there are blocks on the path from genesis
    // block being explored which are the first to have certain properties.
    size_t nNodes = 0;
    int nHeight = 0;
    CBlockIndex* pindexFirstInvalid = NULL;         // Oldest ancestor of pindex which is invalid.
    CBlockIndex* pindexFirstMissing = NULL;         // Oldest ancestor of pindex which does not have BLOCK_HAVE_DATA.
    CBlockIndex* pindexFirstNotTreeValid = NULL;    // Oldest ancestor of pindex which does not have BLOCK_VALID_TREE (regardless of being valid or not).
    CBlockIndex* pindexFirstNotChainValid = NULL;   // Oldest ancestor of pindex which does not have BLOCK_VALID_CHAIN (regardless of being valid or not).
    CBlockIndex* pindexFirstNotScriptsValid = NULL; // Oldest ancestor of pindex which does not have BLOCK_VALID_SCRIPTS (regardless of being valid or not).
    while (pindex != NULL) {
        nNodes++;
        if (pindexFirstInvalid == NULL && pindex->nStatus & BLOCK_FAILED_VALID) pindexFirstInvalid = pindex;
        if (pindexFirstMissing == NULL && !(pindex->nStatus & BLOCK_HAVE_DATA)) pindexFirstMissing = pindex;
        if (pindex->pprev != NULL && pindexFirstNotTreeValid == NULL &&
            (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TREE)
            pindexFirstNotTreeValid = pindex;
        if (pindex->pprev != NULL && pindexFirstNotChainValid == NULL &&
            (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_CHAIN)
            pindexFirstNotChainValid = pindex;
        if (pindex->pprev != NULL && pindexFirstNotScriptsValid == NULL &&
            (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_SCRIPTS)
            pindexFirstNotScriptsValid = pindex;

        // Begin: actual consistency checks.
        if (pindex->pprev == NULL) {
            // Genesis block checks.
            assert(pindex->GetBlockHash() == Params().HashGenesisBlock()); // Genesis block's hash must match.
            assert(pindex ==
                   chainActive.Genesis()); // The current active chain's genesis block must be this block.
        }
        // HAVE_DATA is equivalent to VALID_TRANSACTIONS and equivalent to nTx > 0 (we stored the number of transactions in the block)
        assert(!(pindex->nStatus & BLOCK_HAVE_DATA) == (pindex->nTx == 0));
        assert(((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TRANSACTIONS) == (pindex->nTx > 0));
        if (pindex->nChainTx == 0)
            assert(pindex->nSequenceId == 0); // nSequenceId can't be set for blocks that aren't linked
        // All parents having data is equivalent to all parents being VALID_TRANSACTIONS, which is equivalent to nChainTx being set.
        assert((pindexFirstMissing != NULL) == (pindex->nChainTx ==
                                                   0)); // nChainTx == 0 is used to signal that all parent block's transaction data is available.
        assert(pindex->nHeight ==
               nHeight); // nHeight must be consistent.
        assert(pindex->pprev == NULL || pindex->nChainWork >=
                                            pindex->pprev->nChainWork); // For every block except the genesis block, the chainwork must be larger than the parent's.
        assert(nHeight < 2 || (pindex->pskip && (pindex->pskip->nHeight <
                                                    nHeight))); // The pskip pointer must point back for all but the first 2 blocks.
        assert(pindexFirstNotTreeValid ==
               NULL); // All mapBlockIndex entries must at least be TREE valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TREE)
            assert(pindexFirstNotTreeValid == NULL); // TREE valid implies all parents are TREE valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_CHAIN)
            assert(pindexFirstNotChainValid == NULL); // CHAIN valid implies all parents are CHAIN valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_SCRIPTS)
            assert(pindexFirstNotScriptsValid == NULL); // SCRIPTS valid implies all parents are SCRIPTS valid
        if (pindexFirstInvalid == NULL) {
            // Checks for not-invalid blocks.
            assert((pindex->nStatus & BLOCK_FAILED_MASK) ==
                   0); // The failed mask cannot be set for blocks without invalid parents.
        }
        if (!CBlockIndexWorkComparator()(pindex, chainActive.Tip()) && pindexFirstMissing == NULL) {
            if (pindexFirstInvalid ==
                NULL) { // If this block sorts at least as good as the current tip and is valid, it must be in setBlockIndexCandidates.
                assert(setBlockIndexCandidates.count(pindex));
            }
        } else { // If this block sorts worse than the current tip, it cannot be in setBlockIndexCandidates.
            assert(setBlockIndexCandidates.count(pindex) == 0);
        }
        // Check whether this block is in mapBlocksUnlinked.
        std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> rangeUnlinked = mapBlocksUnlinked.equal_range(
            pindex->pprev);
        bool foundInUnlinked = false;
        while (rangeUnlinked.first != rangeUnlinked.second) {
            assert(rangeUnlinked.first->first == pindex->pprev);
            if (rangeUnlinked.first->second == pindex) {
                foundInUnlinked = true;
                break;
            }
            rangeUnlinked.first++;
        }
        if (pindex->pprev && pindex->nStatus & BLOCK_HAVE_DATA && pindexFirstMissing != NULL) {
            if (pindexFirstInvalid ==
                NULL) { // If this block has block data available, some parent doesn't, and has no invalid parents, it must be in mapBlocksUnlinked.
                assert(foundInUnlinked);
            }
        } else { // If this block does not have block data available, or all parents do, it cannot be in mapBlocksUnlinked.
            assert(!foundInUnlinked);
        }
        // assert(pindex->GetBlockHash() == pindex->GetBlockHeader().GetHash()); // Perhaps too slow
        // End: actual consistency checks.

        // Try descending into the first subnode.
        std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> range = forward.equal_range(
            pindex);
        if (range.first != range.second) {
            // A subnode was found.
            pindex = range.first->second;
            nHeight++;
            continue;
        }
        // This is a leaf node.
        // Move upwards until we reach a node of which we have not yet visited the last child.
        while (pindex) {
            // We are going to either move to a parent or a sibling of pindex.
            // If pindex was the first with a certain property, unset the corresponding variable.
            if (pindex == pindexFirstInvalid) pindexFirstInvalid = NULL;
            if (pindex == pindexFirstMissing) pindexFirstMissing = NULL;
            if (pindex == pindexFirstNotTreeValid) pindexFirstNotTreeValid = NULL;
            if (pindex == pindexFirstNotChainValid) pindexFirstNotChainValid = NULL;
            if (pindex == pindexFirstNotScriptsValid) pindexFirstNotScriptsValid = NULL;
            // Find our parent.
            CBlockIndex* pindexPar = pindex->pprev;
            // Find which child we just visited.
            std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> rangePar = forward.equal_range(
                pindexPar);
            while (rangePar.first->second != pindex) {
                assert(rangePar.first !=
                       rangePar.second); // Our parent must have at least the node we're coming from as child.
                rangePar.first++;
            }
            // Proceed to the next one.
            rangePar.first++;
            if (rangePar.first != rangePar.second) {
                // Move to the sibling.
                pindex = rangePar.first->second;
                break;
            } else {
                // Move up further.
                pindex = pindexPar;
                nHeight--;
                continue;
            }
        }
    }

    // Check that we actually traversed the entire map.
    assert(nNodes == forward.size());
}


std::string GetWarnings(std::string strFor)
{
    std::string strStatusBar;
    std::string strRPC;

    if (!CLIENT_VERSION_IS_RELEASE)
        strStatusBar = _(
            "This is a pre-release test build - use at your own risk - do not use for staking or merchant applications!");

    if (GetBoolArg("-testsafemode", false))
        strStatusBar = strRPC = "testsafemode enabled";

    // Misc warnings like out of disk space and clock is wrong
    if (strMiscWarning != "") {
        strStatusBar = strMiscWarning;
    }

    if (fLargeWorkForkFound) {
        strStatusBar = strRPC = _(
            "Warning: The network does not appear to fully agree! Some miners appear to be experiencing issues.");
    } else if (fLargeWorkInvalidChainFound) {
        strStatusBar = strRPC = _(
            "Warning: We do not appear to fully agree with our peers! You may need to upgrade, or other nodes may need to upgrade.");
    }

    if (strFor == "statusbar")
        return strStatusBar;
    else if (strFor == "rpc")
        return strRPC;
    assert(!"GetWarnings() : invalid parameter");
    return "error";
}


//////////////////////////////////////////////////////////////////////////////
//
// Messages
//


bool static AlreadyHave(const CInv& inv)
{
    switch (inv.type) {
    case MSG_TX: {
        assert(recentRejects);
        if (chainActive.Tip()->GetBlockHash() != hashRecentRejectsChainTip) {
            // If the chain tip has changed previously rejected transactions
            // might be now valid, e.g. due to a nLockTime'd tx becoming valid,
            // or a double-spend. Reset the rejects filter and give those
            // txs a second chance.
            hashRecentRejectsChainTip = chainActive.Tip()->GetBlockHash();
            recentRejects->reset();
        }

        return recentRejects->contains(inv.hash) ||
               mempool.exists(inv.hash) ||
               mapOrphanTransactions.count(inv.hash) ||
               pcoinsTip->HaveCoins(inv.hash);
    }
    case MSG_BLOCK:
        return mapBlockIndex.count(inv.hash);
    case MSG_TXLOCK_REQUEST:
        return mapTxLockReq.count(inv.hash) ||
               mapTxLockReqRejected.count(inv.hash);
    case MSG_TXLOCK_VOTE:
        return mapTxLockVote.count(inv.hash);
    case MSG_MASTERNODE_WINNER:
        if (masternodePayments.mapMasternodePayeeVotes.count(inv.hash)) {
            masternodeSync.AddedMasternodeWinner(inv.hash);
            return true;
        }
        return false;
    case MSG_BUDGET_VOTE:
        if (budget.mapSeenMasternodeBudgetVotes.count(inv.hash)) {
            masternodeSync.AddedBudgetItem(inv.hash);
            return true;
        }
        return false;
    case MSG_BUDGET_PROPOSAL:
        if (budget.mapSeenMasternodeBudgetProposals.count(inv.hash)) {
            masternodeSync.AddedBudgetItem(inv.hash);
            return true;
        }
        return false;
    case MSG_BUDGET_FINALIZED_VOTE:
        if (budget.mapSeenFinalizedBudgetVotes.count(inv.hash)) {
            masternodeSync.AddedBudgetItem(inv.hash);
            return true;
        }
        return false;
    case MSG_BUDGET_FINALIZED:
        if (budget.mapSeenFinalizedBudgets.count(inv.hash)) {
            masternodeSync.AddedBudgetItem(inv.hash);
            return true;
        }
        return false;
    case MSG_MASTERNODE_ANNOUNCE:
        if (mnodeman.mapSeenMasternodeBroadcast.count(inv.hash)) {
            masternodeSync.AddedMasternodeList(inv.hash);
            return true;
        }
        return false;
    case MSG_MASTERNODE_PING:
        return mnodeman.mapSeenMasternodePing.count(inv.hash);
    }
    // Don't know what it is, just say we already got one
    return true;
}

void static ProcessGetData(CNode* pfrom)
{
    AssertLockNotHeld(cs_main);

    std::deque<CInv>::iterator it = pfrom->vRecvGetData.begin();
    std::vector<CInv> vNotFound;

    LOCK(cs_main);

    while (it != pfrom->vRecvGetData.end()) {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        const CInv& inv = *it;
        {
            boost::this_thread::interruption_point();
            it++;

            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK) {
                bool send = false;
                BlockMap::iterator mi = mapBlockIndex.find(inv.hash);
                if (mi != mapBlockIndex.end()) {
                    if (chainActive.Contains(mi->second)) {
                        send = true;
                    } else {
                        // To prevent fingerprinting attacks, only send blocks outside of the active
                        // chain if they are valid, and no more than a max reorg depth than the best header
                        // chain we know about.
                        send = mi->second->IsValid(BLOCK_VALID_SCRIPTS) && (pindexBestHeader != NULL) &&
                               (chainActive.Height() - mi->second->nHeight < Params().MaxReorganizationDepth());
                        if (!send) {
                            std::string remoteAddr;
                            if (fLogIPs)
                                remoteAddr = ", peeraddr=" + pfrom->addr.ToString();

                            LogPrintf(
                                "ProcessGetData(): ignoring request from peer=%i%s for old block that is not in the main chain\n",
                                pfrom->GetId(), remoteAddr);
                        }
                    }
                }
                // Don't send not-validated blocks
                if (send && (mi->second->nStatus & BLOCK_HAVE_DATA)) {
                    // Send block from disk
                    CBlock block;
                    if (!ReadBlockFromDisk(block, (*mi).second))
                        assert(!"cannot load block from disk");
                    if (inv.type == MSG_BLOCK)
                        pfrom->PushMessage(NetMsgType::BLOCK, block);
                    else // MSG_FILTERED_BLOCK)
                    {
                        LOCK(pfrom->cs_filter);
                        if (pfrom->pfilter) {
                            CMerkleBlock merkleBlock(block, *pfrom->pfilter);
                            pfrom->PushMessage(NetMsgType::MERKLEBLOCK, merkleBlock);
                            // CMerkleBlock just contains hashes, so also push any transactions in the block the client did not see
                            // This avoids hurting performance by pointlessly requiring a round-trip
                            // Note that there is currently no way for a node to request any single transactions we didnt send here -
                            // they must either disconnect and retry or request the full block.
                            // Thus, the protocol spec specified allows for us to provide duplicate txn here,
                            // however we MUST always provide at least what the remote peer needs
                            typedef std::pair<unsigned int, uint256> PairType;
                            for (PairType& pair : merkleBlock.vMatchedTxn)
                                pfrom->PushMessage(NetMsgType::TX, block.vtx[pair.first]);
                        }
                        // else
                        // no response
                    }

                    // Trigger them to send a getblocks request for the next batch of inventory
                    if (inv.hash == pfrom->hashContinue) {
                        // Bypass PushInventory, this must send even if redundant,
                        // and we want it right after the last block so they don't
                        // wait for other stuff first.
                        std::vector<CInv> vInv;
                        vInv.push_back(CInv(MSG_BLOCK, chainActive.Tip()->GetBlockHash()));
                        pfrom->PushMessage(NetMsgType::INV, vInv);
                        pfrom->hashContinue.SetNull();
                    }
                }
            } else if (inv.IsKnownType()) {
                // Send stream from relay memory
                bool pushed = false;
                {
                    LOCK(cs_mapRelay);
                    std::map<CInv, CDataStream>::iterator mi = mapRelay.find(inv);
                    if (mi != mapRelay.end()) {
                        pfrom->PushMessage(inv.GetCommand(), (*mi).second);
                        pushed = true;
                    }
                }

                if (!pushed && inv.type == MSG_TX) {
                    CTransaction tx;
                    if (mempool.lookup(inv.hash, tx)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << tx;
                        pfrom->PushMessage(NetMsgType::TX, ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_TXLOCK_VOTE) {
                    if (mapTxLockVote.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mapTxLockVote[inv.hash];
                        pfrom->PushMessage(NetMsgType::IXLOCKVOTE, ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_TXLOCK_REQUEST) {
                    if (mapTxLockReq.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mapTxLockReq[inv.hash];
                        pfrom->PushMessage(NetMsgType::IX, ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_MASTERNODE_WINNER) {
                    if (masternodePayments.mapMasternodePayeeVotes.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << masternodePayments.mapMasternodePayeeVotes[inv.hash];
                        pfrom->PushMessage(NetMsgType::MNWINNER, ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_BUDGET_VOTE) {
                    if (budget.mapSeenMasternodeBudgetVotes.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << budget.mapSeenMasternodeBudgetVotes[inv.hash];
                        pfrom->PushMessage(NetMsgType::BUDGETVOTE, ss);
                        pushed = true;
                    }
                }

                if (!pushed && inv.type == MSG_BUDGET_PROPOSAL) {
                    if (budget.mapSeenMasternodeBudgetProposals.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << budget.mapSeenMasternodeBudgetProposals[inv.hash];
                        pfrom->PushMessage(NetMsgType::BUDGETPROPOSAL, ss);
                        pushed = true;
                    }
                }

                if (!pushed && inv.type == MSG_BUDGET_FINALIZED_VOTE) {
                    if (budget.mapSeenFinalizedBudgetVotes.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << budget.mapSeenFinalizedBudgetVotes[inv.hash];
                        pfrom->PushMessage(NetMsgType::FINALBUDGETVOTE, ss);
                        pushed = true;
                    }
                }

                if (!pushed && inv.type == MSG_BUDGET_FINALIZED) {
                    if (budget.mapSeenFinalizedBudgets.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << budget.mapSeenFinalizedBudgets[inv.hash];
                        pfrom->PushMessage(NetMsgType::FINALBUDGET, ss);
                        pushed = true;
                    }
                }

                if (!pushed && inv.type == MSG_MASTERNODE_ANNOUNCE) {
                    if (mnodeman.mapSeenMasternodeBroadcast.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mnodeman.mapSeenMasternodeBroadcast[inv.hash];
                        pfrom->PushMessage(NetMsgType::MNBROADCAST, ss);
                        pushed = true;
                    }
                }

                if (!pushed && inv.type == MSG_MASTERNODE_PING) {
                    if (mnodeman.mapSeenMasternodePing.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mnodeman.mapSeenMasternodePing[inv.hash];
                        pfrom->PushMessage(NetMsgType::MNPING, ss);
                        pushed = true;
                    }
                }

                if (!pushed) {
                    vNotFound.push_back(inv);
                }
            }

            // Track requests for our stuff.
            GetMainSignals().Inventory(inv.hash);

            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
                break;
        }
    }

    pfrom->vRecvGetData.erase(pfrom->vRecvGetData.begin(), it);

    if (!vNotFound.empty()) {
        // Let the peer know that we didn't find what it asked for, so it doesn't
        // have to wait around forever. Currently only SPV clients actually care
        // about this message: it's needed when they are recursively walking the
        // dependencies of relevant unconfirmed transactions. SPV clients want to
        // do that because they want to know about (and store and rebroadcast and
        // risk analyze) the dependencies of transactions relevant to them, without
        // having to download the entire memory pool.
        pfrom->PushMessage(NetMsgType::NOTFOUND, vNotFound);
    }
}

bool static ProcessMessage(CNode* pfrom, std::string strCommand, CDataStream& vRecv, int64_t nTimeReceived)
{
    LogPrint(BCLog::NET, "received: %s (%u bytes) peer=%d, chainheight=%d\n", SanitizeString(strCommand), vRecv.size(), pfrom->id, chainActive.Height());
    if (mapArgs.count("-dropmessagestest") && GetRand(atoi(mapArgs["-dropmessagestest"])) == 0) {
        LogPrintf("dropmessagestest DROPPING RECV MESSAGE\n");
        return true;
    }
    if (strCommand == NetMsgType::VERSION) {
        // Feeler connections exist only to verify if address is online.
        if (pfrom->fFeeler) {
            assert(pfrom->fInbound == false);
            pfrom->fDisconnect = true;
        }

        // Each connection can only send one version message
        if (pfrom->nVersion != 0) {
            pfrom->PushMessage(NetMsgType::REJECT, strCommand, REJECT_DUPLICATE, std::string("Duplicate version message"));
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 1);
            return false;
        }

        int64_t nTime;
        CAddress addrMe;
        CAddress addrFrom;
        uint64_t nNonce = 1;
        uint64_t nServiceInt;
        vRecv >> pfrom->nVersion >> nServiceInt >> nTime >> addrMe;
        pfrom->nServices = ServiceFlags(nServiceInt);
        if (!pfrom->fInbound) {
            addrman.SetServices(pfrom->addr, pfrom->nServices);
        }
        if (pfrom->nServicesExpected & ~pfrom->nServices) {
            LogPrint(BCLog::NET, "peer=%d does not offer the expected services (%08x offered, %08x expected); disconnecting\n", pfrom->id, pfrom->nServices, pfrom->nServicesExpected);
            pfrom->PushMessage(NetMsgType::REJECT, strCommand, REJECT_NONSTANDARD,
                               strprintf("Expected to offer services %08x", pfrom->nServicesExpected));
            pfrom->fDisconnect = true;
            return false;
        }

        if (pfrom->DisconnectOldProtocol(ActiveProtocol(), strCommand))
            return false;

        if (pfrom->DisconnectOldVersion(pfrom->strSubVer, chainActive.Height(), strCommand))
            return false;

        if (pfrom->nVersion == 10300)
            pfrom->nVersion = 300;
        if (!vRecv.empty())
            vRecv >> addrFrom >> nNonce;
        if (!vRecv.empty()) {
            vRecv >> LIMITED_STRING(pfrom->strSubVer, MAX_SUBVERSION_LENGTH);
            pfrom->cleanSubVer = SanitizeString(pfrom->strSubVer);
        }
        if (!vRecv.empty())
            vRecv >> pfrom->nStartingHeight;
        if (!vRecv.empty())
            vRecv >> pfrom->fRelayTxes; // set to true after we get the first filter* message
        else
            pfrom->fRelayTxes = true;

        // Disconnect if we connected to ourself
        if (nNonce == nLocalHostNonce && nNonce > 1) {
            LogPrintf("connected to self at %s, disconnecting\n", pfrom->addr.ToString());
            pfrom->fDisconnect = true;
            return true;
        }

        pfrom->addrLocal = addrMe;
        if (pfrom->fInbound && addrMe.IsRoutable()) {
            SeenLocal(addrMe);
        }

        // Be shy and don't send version until we hear
        if (pfrom->fInbound)
            pfrom->PushVersion();

        pfrom->fClient = !(pfrom->nServices & NODE_NETWORK);

        {
            LOCK(cs_main);
            // Potentially mark this peer as a preferred download peer.
            UpdatePreferredDownload(pfrom, State(pfrom->GetId()));
        }
        // Change version
        pfrom->PushMessage(NetMsgType::VERACK);
        pfrom->ssSend.SetVersion(std::min(pfrom->nVersion, PROTOCOL_VERSION));

        if (!pfrom->fInbound) {
            // Advertise our address
            if (fListen && !IsInitialBlockDownload()) {
                CAddress addr = GetLocalAddress(&pfrom->addr);
                FastRandomContext insecure_rand;
                if (addr.IsRoutable()) {
                    pfrom->PushAddress(addr, insecure_rand);
                } else if (IsPeerAddrLocalGood(pfrom)) {
                    addr.SetIP(pfrom->addrLocal);
                    pfrom->PushAddress(addr, insecure_rand);
                }
            }

            // Get recent addresses
            if (pfrom->fOneShot || pfrom->nVersion >= CADDR_TIME_VERSION || addrman.size() < 1000) {
                pfrom->PushMessage(NetMsgType::GETADDR);
                pfrom->fGetAddr = true;
            }
            addrman.Good(pfrom->addr);
        }

        pfrom->fSuccessfullyConnected = true;

        std::string remoteAddr;
        if (fLogIPs)
            remoteAddr = ", peeraddr=" + pfrom->addr.ToString();

        LogPrintf("receive version message: %s: version %d, blocks=%d, us=%s, peer=%d%s\n",
            pfrom->cleanSubVer, pfrom->nVersion,
            pfrom->nStartingHeight, addrMe.ToString(), pfrom->id,
            remoteAddr);

        int64_t nTimeOffset = nTime - GetTime();
        pfrom->nTimeOffset = nTimeOffset;
        AddTimeData(pfrom->addr, nTimeOffset);
    } else if (pfrom->nVersion == 0) {
        // Must have a version message before anything else
        LOCK(cs_main);
        Misbehaving(pfrom->GetId(), 1);
        return false;
    } else if (strCommand == NetMsgType::VERACK) {
        pfrom->SetRecvVersion(std::min(pfrom->nVersion, PROTOCOL_VERSION));

        // Mark this node as currently connected, so we update its timestamp later.
        if (pfrom->fNetworkNode) {
            LOCK(cs_main);
            State(pfrom->GetId())->fCurrentlyConnected = true;
        }
    } else if (strCommand == NetMsgType::ADDR) {
        std::vector<CAddress> vAddr;
        vRecv >> vAddr;

        // Don't want addr from older versions unless seeding
        if (pfrom->nVersion < CADDR_TIME_VERSION && addrman.size() > 1000)
            return true;
        if (vAddr.size() > 1000) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 20);
            return error("message addr size() = %u", vAddr.size());
        }

        // Store the new addresses
        std::vector<CAddress> vAddrOk;
        int64_t nNow = GetAdjustedTime();
        int64_t nSince = nNow - 10 * 60;
        for (CAddress& addr : vAddr) {
            boost::this_thread::interruption_point();

            if ((addr.nServices & REQUIRED_SERVICES) != REQUIRED_SERVICES)
                continue;

            if (addr.nTime <= 100000000 || addr.nTime > nNow + 10 * 60)
                addr.nTime = nNow - 5 * 24 * 60 * 60;
            pfrom->AddAddressKnown(addr);
            bool fReachable = IsReachable(addr);
            if (addr.nTime > nSince && !pfrom->fGetAddr && vAddr.size() <= 10 && addr.IsRoutable()) {
                // Relay to a limited number of other nodes
                {
                    LOCK(cs_vNodes);
                    // Use deterministic randomness to send to the same nodes for 24 hours
                    // at a time so the addrKnowns of the chosen nodes prevent repeats
                    static uint256 hashSalt;
                    if (hashSalt.IsNull())
                        hashSalt = GetRandHash();
                    uint64_t hashAddr = addr.GetHash();
                    uint256 hashRand = hashSalt ^ (hashAddr << 32) ^ ((GetTime() + hashAddr) / (24 * 60 * 60));
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    std::multimap<uint256, CNode*> mapMix;
                    for (CNode* pnode : vNodes) {
                        if (pnode->nVersion < CADDR_TIME_VERSION)
                            continue;
                        unsigned int nPointer;
                        memcpy(&nPointer, &pnode, sizeof(nPointer));
                        uint256 hashKey = hashRand ^ nPointer;
                        hashKey = Hash(BEGIN(hashKey), END(hashKey));
                        mapMix.insert(std::make_pair(hashKey, pnode));
                    }
                    int nRelayNodes = fReachable ? 2 : 1; // limited relaying of addresses outside our network(s)
                    FastRandomContext insecure_rand;
                    for (std::multimap<uint256, CNode*>::iterator mi = mapMix.begin();
                        mi != mapMix.end() && nRelayNodes-- > 0; ++mi)
                        ((*mi).second)->PushAddress(addr, insecure_rand);
                }
            }
            // Do not store addresses outside our network
            if (fReachable)
                vAddrOk.push_back(addr);
        }
        addrman.Add(vAddrOk, pfrom->addr, 2 * 60 * 60);
        if (vAddr.size() < 1000)
            pfrom->fGetAddr = false;
        if (pfrom->fOneShot)
            pfrom->fDisconnect = true;
    } else if (strCommand == NetMsgType::INV) {
        std::vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 20);
            return error("message inv size() = %u", vInv.size());
        }

        // find last block in inv vector
        unsigned int nLastBlock = (unsigned int)(-1);
        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++) {
            if (vInv[vInv.size() - 1 - nInv].type == MSG_BLOCK) {
                nLastBlock = vInv.size() - 1 - nInv;
                break;
            }
        }

        LOCK(cs_main);

        std::vector<CInv> vToFetch;
        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++) {
            const CInv& inv = vInv[nInv];

            boost::this_thread::interruption_point();
            pfrom->AddInventoryKnown(inv);

            bool fAlreadyHave = AlreadyHave(inv);
            LogPrint(BCLog::NET, "got inv: %s  %s peer=%d, inv.type=%d, mapBlocksInFlight.count(inv.hash)=%d\n", inv.ToString(), fAlreadyHave ? "have" : "new", pfrom->id, inv.type, mapBlocksInFlight.count(inv.hash));

            if (!fAlreadyHave && pfrom)
                pfrom->AskFor(inv, IsInitialBlockDownload()); // peershares: immediate retry during initial download
            if (inv.type == MSG_BLOCK) {
                UpdateBlockAvailability(pfrom->GetId(), inv.hash);
                if (!fAlreadyHave && !fImporting && !fReindex && !mapBlocksInFlight.count(inv.hash)) {
                    // Add this to the list of blocks to request
                    vToFetch.push_back(inv);
                    LogPrint(BCLog::NET, "getblocks (%d) %s to peer=%d\n", pindexBestHeader->nHeight, inv.hash.ToString(),
                        pfrom->id);
                }
            } else if (nInv == nLastBlock) {
                // In case we are on a very long side-chain, it is possible that we already have
                // the last block in an inv bundle sent in response to getblocks. Try to detect
                // this situation and push another getblocks to continue.
                std::vector<CInv> vGetData(1,inv);

                if (pfrom) {
                    pfrom->PushMessage(NetMsgType::GETBLOCKS, chainActive.GetLocator(mapBlockIndex[inv.hash]), UINT256_ZERO);
                }
                printf("force request: %s\n", inv.ToString().c_str());
            }

            // Track requests for our stuff
            GetMainSignals().Inventory(inv.hash);
            if (pfrom->nSendSize > (SendBufferSize() * 2)) {
                Misbehaving(pfrom->GetId(), 50);
                return error("send buffer size() = %u", pfrom->nSendSize);
            }
        }

        if (!vToFetch.empty())
            pfrom->PushMessage(NetMsgType::GETDATA, vToFetch);
    } else if (strCommand == NetMsgType::GETDATA) {
        std::vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 20);
            return error("message getdata size() = %u", vInv.size());
        }

        if ((vInv.size() != 1))
            LogPrint(BCLog::NET, "received getdata (%u invsz) peer=%d\n", vInv.size(), pfrom->id);

        if (vInv.size() > 0)
            LogPrint(BCLog::NET, "received getdata for: %s peer=%d\n", vInv[0].ToString(), pfrom->id);

        pfrom->vRecvGetData.insert(pfrom->vRecvGetData.end(), vInv.begin(), vInv.end());
        ProcessGetData(pfrom);
    } else if (strCommand == NetMsgType::GETBLOCKS || strCommand == NetMsgType::GETHEADERS) {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        if (locator.vHave.size() > MAX_LOCATOR_SZ) {
            LogPrint(BCLog::NET, "getblocks locator size %lld > %d, disconnect peer=%d\n", locator.vHave.size(), MAX_LOCATOR_SZ, pfrom->GetId());
            pfrom->fDisconnect = true;
            return true;
        }

        LOCK(cs_main);

        // Find the last block the caller has in the main chain
        CBlockIndex* pindex = FindForkInGlobalIndex(chainActive, locator);

        // Send the rest of the chain
        if (pindex)
            pindex = chainActive.Next(pindex);
        int nLimit = 500;
        LogPrint(BCLog::NET, "getblocks %d to %s limit %d from peer=%d\n", (pindex ? pindex->nHeight : -1),
            hashStop.IsNull() ? "end" : hashStop.ToString(), nLimit, pfrom->id);
        for (; pindex; pindex = chainActive.Next(pindex)) {
            if (pindex->GetBlockHash() == hashStop) {
                LogPrint(BCLog::NET, "  getblocks stopping at %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                break;
            }
            pfrom->PushInventory(CInv(MSG_BLOCK, pindex->GetBlockHash()));
            if (--nLimit <= 0) {
                // When this block is requested, we'll send an inv that'll make them
                // getblocks the next batch of inventory.
                LogPrint(BCLog::NET, "  getblocks stopping at limit %d %s\n", pindex->nHeight,
                    pindex->GetBlockHash().ToString());
                pfrom->hashContinue = pindex->GetBlockHash();
                break;
            }
        }
    } else if (strCommand == NetMsgType::HEADERS && Params().HeadersFirstSyncingActive()) {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        if (locator.vHave.size() > MAX_LOCATOR_SZ) {
            LogPrint(BCLog::NET, "getblocks locator size %lld > %d, disconnect peer=%d\n", locator.vHave.size(), MAX_LOCATOR_SZ, pfrom->GetId());
            pfrom->fDisconnect = true;
            return true;
        }

        LOCK(cs_main);

        if (IsInitialBlockDownload())
            return true;

        CBlockIndex* pindex = NULL;
        if (locator.IsNull()) {
            // If locator is null, return the hashStop block
            BlockMap::iterator mi = mapBlockIndex.find(hashStop);
            if (mi == mapBlockIndex.end())
                return true;
            pindex = (*mi).second;
        } else {
            // Find the last block the caller has in the main chain
            pindex = FindForkInGlobalIndex(chainActive, locator);
            if (pindex)
                pindex = chainActive.Next(pindex);
        }

        // we must use CBlocks, as CBlockHeaders won't include the 0x00 nTx count at the end
        std::vector<CBlock> vHeaders;
        int nLimit = MAX_HEADERS_RESULTS;
        LogPrintf("getheaders %d to %s from peer=%d\n", (pindex ? pindex->nHeight : -1), hashStop.ToString(), pfrom->id);
        for (; pindex; pindex = chainActive.Next(pindex)) {
            vHeaders.push_back(pindex->GetBlockHeader());
            if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop)
                break;
        }
        pfrom->PushMessage(NetMsgType::HEADERS, vHeaders);
    } else if (strCommand == NetMsgType::TX) {
        std::vector<uint256> vWorkQueue;
        std::vector<uint256> vEraseQueue;
        CTransaction tx;

        //masternode signed transaction
        bool ignoreFees = false;
        CTxIn vin;
        std::vector<unsigned char> vchSig;

        vRecv >> tx;

        CInv inv(MSG_TX, tx.GetHash());
        pfrom->AddInventoryKnown(inv);

        LOCK(cs_main);

        bool fMissingInputs = false;
        CValidationState state;

        mapAlreadyAskedFor.erase(inv);

        if (AcceptToMemoryPool(mempool, state, tx, true, &fMissingInputs, false, ignoreFees)) {
            mempool.check(pcoinsTip);
            RelayTransaction(tx);
            vWorkQueue.push_back(inv.hash);

            LogPrint(BCLog::MEMPOOL, "AcceptToMemoryPool: peer=%d %s : accepted %s (poolsz %u)\n",
                pfrom->id, pfrom->cleanSubVer,
                tx.GetHash().ToString(),
                mempool.mapTx.size());

            // Recursively process any orphan transactions that depended on this one
            std::set<NodeId> setMisbehaving;
            for (unsigned int i = 0; i < vWorkQueue.size(); i++) {
                std::map<uint256, std::set<uint256> >::iterator itByPrev = mapOrphanTransactionsByPrev.find(vWorkQueue[i]);
                if (itByPrev == mapOrphanTransactionsByPrev.end())
                    continue;
                for(std::set<uint256>::iterator mi = itByPrev->second.begin();
                     mi != itByPrev->second.end();
                     ++mi) {
                    const uint256& orphanHash = *mi;
                    const CTransaction& orphanTx = mapOrphanTransactions[orphanHash].tx;
                    NodeId fromPeer = mapOrphanTransactions[orphanHash].fromPeer;
                    bool fMissingInputs2 = false;
                    // Use a dummy CValidationState so someone can't setup nodes to counter-DoS based on orphan
                    // resolution (that is, feeding people an invalid transaction based on LegitTxX in order to get
                    // anyone relaying LegitTxX banned)
                    CValidationState stateDummy;


                    if (setMisbehaving.count(fromPeer))
                        continue;
                    if (AcceptToMemoryPool(mempool, stateDummy, orphanTx, true, &fMissingInputs2)) {
                        LogPrint(BCLog::MEMPOOL, "   accepted orphan tx %s\n", orphanHash.ToString());
                        RelayTransaction(orphanTx);
                        vWorkQueue.push_back(orphanHash);
                        vEraseQueue.push_back(orphanHash);
                    } else if (!fMissingInputs2) {
                        int nDos = 0;
                        if (stateDummy.IsInvalid(nDos) && nDos > 0) {
                            // Punish peer that gave us an invalid orphan tx
                            Misbehaving(fromPeer, nDos);
                            setMisbehaving.insert(fromPeer);
                            LogPrint(BCLog::MEMPOOL, "   invalid orphan tx %s\n", orphanHash.ToString());
                        }
                        // Has inputs but not accepted to mempool
                        // Probably non-standard or insufficient fee/priority
                        LogPrint(BCLog::MEMPOOL, "   removed orphan tx %s\n", orphanHash.ToString());
                        vEraseQueue.push_back(orphanHash);
                        assert(recentRejects);
                        recentRejects->insert(orphanHash);
                    }
                    mempool.check(pcoinsTip);
                }
            }

            for (uint256 hash : vEraseQueue)
                EraseOrphanTx(hash);
        } else if (fMissingInputs) {
            AddOrphanTx(tx, pfrom->GetId());

            // DoS prevention: do not allow mapOrphanTransactions to grow unbounded
            unsigned int nMaxOrphanTx = (unsigned int)std::max((int64_t)0, GetArg("-maxorphantx",
                                                                               DEFAULT_MAX_ORPHAN_TRANSACTIONS));
            unsigned int nEvicted = LimitOrphanTxSize(nMaxOrphanTx);
            if (nEvicted > 0)
                LogPrint(BCLog::MEMPOOL, "mapOrphan overflow, removed %u tx\n", nEvicted);
        } else {
            // AcceptToMemoryPool() returned false, possibly because the tx is
            // already in the mempool; if the tx isn't in the mempool that
            // means it was rejected and we shouldn't ask for it again.
            if (!mempool.exists(tx.GetHash())) {
                assert(recentRejects);
                recentRejects->insert(tx.GetHash());
            }
            if (pfrom->fWhitelisted) {
                // Always relay transactions received from whitelisted peers, even
                // if they were rejected from the mempool, allowing the node to
                // function as a gateway for nodes hidden behind it.
                //
                // FIXME: This includes invalid transactions, which means a
                // whitelisted peer could get us banned! We may want to change
                // that.
                RelayTransaction(tx);
            }
        }

        int nDoS = 0;
        if (state.IsInvalid(nDoS)) {
            LogPrint(BCLog::MEMPOOL, "%s from peer=%d %s was not accepted into the memory pool: %s\n",
                tx.GetHash().ToString(),
                pfrom->id, pfrom->cleanSubVer,
                state.GetRejectReason());
            pfrom->PushMessage(NetMsgType::REJECT, strCommand, state.GetRejectCode(),
                state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH), inv.hash);
            if (nDoS > 0)
                Misbehaving(pfrom->GetId(), nDoS);
        }
    } else if (strCommand == NetMsgType::HEADERS && Params().HeadersFirstSyncingActive() && !fImporting &&
               !fReindex) // Ignore headers received while importing
    {
        std::vector<CBlockHeader> headers;

        // Bypass the normal CBlock deserialization, as we don't want to risk deserializing 2000 full blocks.
        unsigned int nCount = ReadCompactSize(vRecv);
        if (nCount > MAX_HEADERS_RESULTS) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 20);
            return error("headers message size = %u", nCount);
        }
        headers.resize(nCount);
        for (unsigned int n = 0; n < nCount; n++) {
            vRecv >> headers[n];
            ReadCompactSize(vRecv); // ignore tx count; assume it is 0.
        }

        LOCK(cs_main);

        if (nCount == 0) {
            // Nothing interesting. Stop asking this peers for more headers.
            return true;
        }
        CBlockIndex* pindexLast = NULL;
        for (const CBlockHeader& header : headers) {
            CValidationState state;
            if (pindexLast != NULL && header.hashPrevBlock != pindexLast->GetBlockHash()) {
                Misbehaving(pfrom->GetId(), 20);
                return error("non-continuous headers sequence");
            }

            /*TODO: this has a CBlock cast on it so that it will compile. There should be a solution for this
             * before headers are reimplemented on mainnet
             */
            if (!AcceptBlockHeader((CBlock)header, state, &pindexLast)) {
                int nDoS;
                if (state.IsInvalid(nDoS)) {
                    if (nDoS > 0)
                        Misbehaving(pfrom->GetId(), nDoS);
                    std::string strError = "invalid header received " + header.GetHash().ToString();
                    return error(strError.c_str());
                }
            }
        }

        if (pindexLast)
            UpdateBlockAvailability(pfrom->GetId(), pindexLast->GetBlockHash());

        if (nCount == MAX_HEADERS_RESULTS && pindexLast) {
            // Headers message had its maximum size; the peer may have more headers.
            // TODO: optimize: if pindexLast is an ancestor of chainActive.Tip or pindexBestHeader, continue
            // from there instead.
            LogPrintf("more getheaders (%d) to end to peer=%d (startheight:%d)\n", pindexLast->nHeight, pfrom->id,
                pfrom->nStartingHeight);
            pfrom->PushMessage(NetMsgType::GETHEADERS, chainActive.GetLocator(pindexLast), UINT256_ZERO);
        }

        CheckBlockIndex();
    } else if (strCommand == NetMsgType::BLOCK && !fImporting && !fReindex) // Ignore blocks received while importing
    {
        CBlock block;
        vRecv >> block;
        uint256 hashBlock = block.GetHash();
        CInv inv(MSG_BLOCK, hashBlock);
        LogPrint(BCLog::NET, "received block %s peer=%d, height=%d\n", inv.hash.ToString(), pfrom->id, chainActive.Height());

        //sometimes we will be sent their most recent block and its not the one we want, in that case tell where we are
        if (!mapBlockIndex.count(block.hashPrevBlock)) {
            if (find(pfrom->vBlockRequested.begin(), pfrom->vBlockRequested.end(), hashBlock) != pfrom->vBlockRequested.end()) {
                //we already asked for this block, so lets work backwards and ask for the previous block
                pfrom->PushMessage(NetMsgType::GETBLOCKS, chainActive.GetLocator(), block.hashPrevBlock);
                pfrom->vBlockRequested.push_back(block.hashPrevBlock);
            } else {
                //ask to sync to this block
                pfrom->PushMessage(NetMsgType::GETBLOCKS, chainActive.GetLocator(), hashBlock);
                pfrom->vBlockRequested.push_back(hashBlock);
            }
        } else {
            pfrom->AddInventoryKnown(inv);
            CValidationState state;
            if (!mapBlockIndex.count(block.GetHash())) {
                ProcessNewBlock(state, pfrom, &block);
                int nDoS;
                if (state.IsInvalid(nDoS)) {
                    pfrom->PushMessage(NetMsgType::REJECT, strCommand, state.GetRejectCode(),
                        state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH), inv.hash);
                    if (nDoS > 0) {
                        TRY_LOCK(cs_main, lockMain);
                        if (lockMain) Misbehaving(pfrom->GetId(), nDoS);
                    }
                }
                //disconnect this node if its old protocol version
                pfrom->DisconnectOldProtocol(ActiveProtocol(), strCommand);
                pfrom->DisconnectOldVersion(pfrom->strSubVer, chainActive.Height(), strCommand);
                if (mapBlockIndex.count(block.GetHash())) {
                    LogPrint(BCLog::NET, "Added block %s to block index map\n", block.GetHash().GetHex());
                }
            } else {
                LogPrint(BCLog::NET, "%s : Already processed block %s, skipping ProcessNewBlock()\n", __func__,
                    block.GetHash().GetHex());
            }
        }
    }


    // This asymmetric behavior for inbound and outbound connections was introduced
    // to prevent a fingerprinting attack: an attacker can send specific fake addresses
    // to users' AddrMan and later request them by sending getaddr messages.
    // Making users (which are behind NAT and can only make outgoing connections) ignore
    // getaddr message mitigates the attack.
    else if ((strCommand == NetMsgType::GETADDR) && (pfrom->fInbound)) {
        pfrom->vAddrToSend.clear();
        std::vector<CAddress> vAddr = addrman.GetAddr();
        FastRandomContext insecure_rand;
        for (const CAddress& addr : vAddr)
            pfrom->PushAddress(addr, insecure_rand);
    } else if (strCommand == NetMsgType::MEMPOOL) {
        LOCK2(cs_main, pfrom->cs_filter);

        std::vector<uint256> vtxid;
        mempool.queryHashes(vtxid);
        std::vector<CInv> vInv;
        for (uint256& hash : vtxid) {
            CInv inv(MSG_TX, hash);
            CTransaction tx;
            bool fInMemPool = mempool.lookup(hash, tx);
            if (!fInMemPool) continue; // another thread removed since queryHashes, maybe...
            if ((pfrom->pfilter && pfrom->pfilter->IsRelevantAndUpdate(tx)) ||
                (!pfrom->pfilter))
                vInv.push_back(inv);
            if (vInv.size() == MAX_INV_SZ) {
                pfrom->PushMessage(NetMsgType::INV, vInv);
                vInv.clear();
            }
        }
        if (vInv.size() > 0)
            pfrom->PushMessage(NetMsgType::INV, vInv);
    } else if (strCommand == NetMsgType::PING) {
        if (pfrom->nVersion > BIP0031_VERSION) {
            uint64_t nonce = 0;
            vRecv >> nonce;
            // Echo the message back with the nonce. This allows for two useful features:
            //
            // 1) A remote node can quickly check if the connection is operational
            // 2) Remote nodes can measure the latency of the network thread. If this node
            //    is overloaded it won't respond to pings quickly and the remote node can
            //    avoid sending us more work, like chain download requests.
            //
            // The nonce stops the remote getting confused between different pings: without
            // it, if the remote node sends a ping once per second and this node takes 5
            // seconds to respond to each, the 5th ping the remote sends would appear to
            // return very quickly.
            pfrom->PushMessage(NetMsgType::PONG, nonce);
        }
    } else if (strCommand == NetMsgType::PONG) {
        int64_t pingUsecEnd = nTimeReceived;
        uint64_t nonce = 0;
        size_t nAvail = vRecv.in_avail();
        bool bPingFinished = false;
        std::string sProblem;

        if (nAvail >= sizeof(nonce)) {
            vRecv >> nonce;

            // Only process pong message if there is an outstanding ping (old ping without nonce should never pong)
            if (pfrom->nPingNonceSent != 0) {
                if (nonce == pfrom->nPingNonceSent) {
                    // Matching pong received, this ping is no longer outstanding
                    bPingFinished = true;
                    int64_t pingUsecTime = pingUsecEnd - pfrom->nPingUsecStart;
                    if (pingUsecTime > 0) {
                        // Successful ping time measurement, replace previous
                        pfrom->nPingUsecTime = pingUsecTime;
                        pfrom->nMinPingUsecTime = std::min(pfrom->nMinPingUsecTime, pingUsecTime);
                    } else {
                        // This should never happen
                        sProblem = "Timing mishap";
                    }
                } else {
                    // Nonce mismatches are normal when pings are overlapping
                    sProblem = "Nonce mismatch";
                    if (nonce == 0) {
                        // This is most likely a bug in another implementation somewhere, cancel this ping
                        bPingFinished = true;
                        sProblem = "Nonce zero";
                    }
                }
            } else {
                sProblem = "Unsolicited pong without ping";
            }
        } else {
            // This is most likely a bug in another implementation somewhere, cancel this ping
            bPingFinished = true;
            sProblem = "Short payload";
        }

        if (!(sProblem.empty())) {
            LogPrint(BCLog::NET, "pong peer=%d %s: %s, %x expected, %x received, %u bytes\n",
                pfrom->id,
                pfrom->cleanSubVer,
                sProblem,
                pfrom->nPingNonceSent,
                nonce,
                nAvail);
        }
        if (bPingFinished) {
            pfrom->nPingNonceSent = 0;
        }
    } else if (!(nLocalServices & NODE_BLOOM) &&
               (strCommand == NetMsgType::FILTERLOAD ||
                   strCommand == NetMsgType::FILTERADD ||
                   strCommand == NetMsgType::FILTERCLEAR)) {
        LogPrintf("bloom message=%s\n", strCommand);
        LOCK(cs_main);
        Misbehaving(pfrom->GetId(), 100);
    } else if (strCommand == NetMsgType::FILTERLOAD) {
        CBloomFilter filter;
        vRecv >> filter;

        if (!filter.IsWithinSizeConstraints()) {
            // There is no excuse for sending a too-large filter
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 100);
        } else {
            LOCK(pfrom->cs_filter);
            delete pfrom->pfilter;
            pfrom->pfilter = new CBloomFilter(filter);
            pfrom->pfilter->UpdateEmptyFull();
        }
        pfrom->fRelayTxes = true;
    } else if (strCommand == NetMsgType::FILTERADD) {
        std::vector<unsigned char> vData;
        vRecv >> vData;

        // Nodes must NEVER send a data item > 520 bytes (the max size for a script data object,
        // and thus, the maximum size any matched object can have) in a filteradd message
        if (vData.size() > MAX_SCRIPT_ELEMENT_SIZE) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 100);
        } else {
            LOCK(pfrom->cs_filter);
            if (pfrom->pfilter)
                pfrom->pfilter->insert(vData);
            else {
                LOCK(cs_main);
                Misbehaving(pfrom->GetId(), 100);
            }
        }
    } else if (strCommand == NetMsgType::FILTERCLEAR) {
        LOCK(pfrom->cs_filter);
        delete pfrom->pfilter;
        pfrom->pfilter = new CBloomFilter();
        pfrom->fRelayTxes = true;
    } else if (strCommand == NetMsgType::REJECT) {
        try {
            std::string strMsg;
            unsigned char ccode;
            std::string strReason;
            vRecv >> LIMITED_STRING(strMsg, CMessageHeader::COMMAND_SIZE) >> ccode >> LIMITED_STRING(strReason, MAX_REJECT_MESSAGE_LENGTH);

            std::ostringstream ss;
            ss << strMsg << " code " << itostr(ccode) << ": " << strReason;

            if (strMsg == NetMsgType::BLOCK || strMsg == NetMsgType::TX) {
                uint256 hash;
                vRecv >> hash;
                ss << ": hash " << hash.ToString();
            }
                LogPrint(BCLog::NET, "Reject %s\n", SanitizeString(ss.str()));
            } catch (const std::ios_base::failure& e) {
                // Avoid feedback loops by preventing reject messages from triggering a new reject message.
                LogPrint(BCLog::NET, "Unparseable reject message received\n");
        }
    } else {
        bool found = false;
        const std::vector<std::string> &allMessages = getAllNetMessageTypes();
        for (const std::string msg : allMessages) {
            if(msg == strCommand) {
                found = true;
                break;
            }
        }

        if (found) {
            //probably one the extensions
            mnodeman.ProcessMessage(pfrom, strCommand, vRecv);
            budget.ProcessMessage(pfrom, strCommand, vRecv);
            masternodePayments.ProcessMessageMasternodePayments(pfrom, strCommand, vRecv);
            ProcessMessageSwiftTX(pfrom, strCommand, vRecv);
            masternodeSync.ProcessMessage(pfrom, strCommand, vRecv);
        } else {
            // Ignore unknown commands for extensibility
            LogPrint(BCLog::NET, "Unknown command \"%s\" from peer=%d\n", SanitizeString(strCommand), pfrom->id);
        }
    }


    return true;
}

// Note: whenever a protocol update is needed toggle between both implementations (comment out the formerly active one)
//       it was the one which was commented out
int ActiveProtocol()
{
    return MIN_PEER_PROTO_VERSION_BEFORE_ENFORCEMENT;
}

bool ProcessMessages(CNode* pfrom)
{
    // Message format
    //  (4) message start
    //  (12) command
    //  (4) size
    //  (4) checksum
    //  (x) data
    //
    bool fOk = true;

    if (!pfrom->vRecvGetData.empty())
        ProcessGetData(pfrom);

    // this maintains the order of responses
    if (!pfrom->vRecvGetData.empty()) return fOk;

    std::deque<CNetMessage>::iterator it = pfrom->vRecvMsg.begin();
    while (!pfrom->fDisconnect && it != pfrom->vRecvMsg.end()) {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        // get next message
        CNetMessage& msg = *it;

        // end, if an incomplete message is found
        if (!msg.complete())
            break;

        // at this point, any failure means we can delete the current message
        it++;

        // Scan for message start
        if (memcmp(msg.hdr.pchMessageStart, Params().MessageStart(), MESSAGE_START_SIZE) != 0) {
            LogPrintf("PROCESSMESSAGE: INVALID MESSAGESTART %s peer=%d\n", SanitizeString(msg.hdr.GetCommand()), pfrom->id);
            fOk = false;
            break;
        }

        // Read header
        CMessageHeader& hdr = msg.hdr;
        if (!hdr.IsValid()) {
            LogPrintf("PROCESSMESSAGE: ERRORS IN HEADER %s peer=%d\n", SanitizeString(hdr.GetCommand()), pfrom->id);
            continue;
        }
        std::string strCommand = hdr.GetCommand();

        // Message size
        unsigned int nMessageSize = hdr.nMessageSize;

        // Checksum
        CDataStream& vRecv = msg.vRecv;
        uint256 hash = Hash(vRecv.begin(), vRecv.begin() + nMessageSize);
        unsigned int nChecksum = ReadLE32((unsigned char*)&hash);
        if (nChecksum != hdr.nChecksum) {
            LogPrintf("ProcessMessages(%s, %u bytes): CHECKSUM ERROR nChecksum=%08x hdr.nChecksum=%08x\n",
                SanitizeString(strCommand), nMessageSize, nChecksum, hdr.nChecksum);
            continue;
        }

        // Process message
        bool fRet = false;
        try {
            fRet = ProcessMessage(pfrom, strCommand, vRecv, msg.nTime);
            boost::this_thread::interruption_point();
        } catch (const std::ios_base::failure& e) {
            pfrom->PushMessage(NetMsgType::REJECT, strCommand, REJECT_MALFORMED, std::string("error parsing message"));
            if (strstr(e.what(), "end of data")) {
                // Allow exceptions from under-length message on vRecv
                LogPrintf("ProcessMessages(%s, %u bytes): Exception '%s' caught, normally caused by a message being shorter than its stated length\n", SanitizeString(strCommand), nMessageSize, e.what());
            } else if (strstr(e.what(), "size too large")) {
                // Allow exceptions from over-long size
                LogPrintf("ProcessMessages(%s, %u bytes): Exception '%s' caught\n", SanitizeString(strCommand), nMessageSize, e.what());
            } else {
                PrintExceptionContinue(&e, "ProcessMessages()");
            }
        } catch (const boost::thread_interrupted&) {
            throw;
        } catch (const std::exception& e) {
            PrintExceptionContinue(&e, "ProcessMessages()");
        } catch (...) {
            PrintExceptionContinue(NULL, "ProcessMessages()");
        }

        if (!fRet)
            LogPrintf("ProcessMessage(%s, %u bytes) FAILED peer=%d\n", SanitizeString(strCommand), nMessageSize, pfrom->id);

        break;
    }

    // In case the connection got shut down, its receive buffer was wiped
    if (!pfrom->fDisconnect)
        pfrom->vRecvMsg.erase(pfrom->vRecvMsg.begin(), it);

    return fOk;
}


bool SendMessages(CNode* pto)
{
    {
        // Don't send anything until we get their version message
        if (pto->nVersion == 0)
            return true;

        //
        // Message: ping
        //
        bool pingSend = false;
        if (pto->fPingQueued) {
            // RPC ping request by user
            pingSend = true;
        }
        if (pto->nPingNonceSent == 0 && pto->nPingUsecStart + PING_INTERVAL * 1000000 < GetTimeMicros()) {
            // Ping automatically sent as a latency probe & keepalive.
            pingSend = true;
        }
        if (pingSend && !pto->fDisconnect) {
            uint64_t nonce = 0;
            while (nonce == 0) {
                GetRandBytes((unsigned char*)&nonce, sizeof(nonce));
            }
            pto->fPingQueued = false;
            pto->nPingUsecStart = GetTimeMicros();
            if (pto->nVersion > BIP0031_VERSION) {
                pto->nPingNonceSent = nonce;
                pto->PushMessage(NetMsgType::PING, nonce);
            } else {
                // Peer is too old to support ping command with nonce, pong will never arrive.
                pto->nPingNonceSent = 0;
                pto->PushMessage(NetMsgType::PING);
            }
        }

        TRY_LOCK(cs_main, lockMain); // Acquire cs_main for IsInitialBlockDownload() and CNodeState()
        if (!lockMain)
            return true;

        // Address refresh broadcast
        int64_t nNow = GetTimeMicros();
        if (!IsInitialBlockDownload() && pto->nNextLocalAddrSend < nNow) {
            AdvertiseLocal(pto);
            pto->nNextLocalAddrSend = PoissonNextSend(nNow, AVG_LOCAL_ADDRESS_BROADCAST_INTERVAL);
        }

        //
        // Message: addr
        //
        if (pto->nNextAddrSend < nNow) {
            pto->nNextAddrSend = PoissonNextSend(nNow, AVG_ADDRESS_BROADCAST_INTERVAL);
            std::vector<CAddress> vAddr;
            vAddr.reserve(pto->vAddrToSend.size());
            for (const CAddress& addr : pto->vAddrToSend) {
                if (!pto->addrKnown.contains(addr.GetKey())) {
                    pto->addrKnown.insert(addr.GetKey());
                    vAddr.push_back(addr);
                    // receiver rejects addr messages larger than 1000
                    if (vAddr.size() >= 1000) {
                        pto->PushMessage(NetMsgType::ADDR, vAddr);
                        vAddr.clear();
                    }
                }
            }
            pto->vAddrToSend.clear();
            if (!vAddr.empty())
                pto->PushMessage(NetMsgType::ADDR, vAddr);
        }

        CNodeState& state = *State(pto->GetId());
        if (state.fShouldBan) {
            if (pto->fWhitelisted)
                LogPrintf("Warning: not punishing whitelisted peer %s!\n", pto->addr.ToString());
            else {
                pto->fDisconnect = true;
                if (pto->addr.IsLocal())
                    LogPrintf("Warning: not banning local peer %s!\n", pto->addr.ToString());
                else {
                    CNode::Ban(pto->addr, BanReasonNodeMisbehaving);
                }
            }
            state.fShouldBan = false;
        }

        for (const CBlockReject& reject : state.rejects)
            pto->PushMessage(NetMsgType::REJECT, (std::string) NetMsgType::BLOCK, reject.chRejectCode, reject.strRejectReason, reject.hashBlock);
        state.rejects.clear();

        // Start block sync
        if (pindexBestHeader == NULL)
            pindexBestHeader = chainActive.Tip();
        bool fFetch = state.fPreferredDownload || (nPreferredDownload == 0 && !pto->fClient &&
                                                      !pto->fOneShot); // Download if this is a nice peer, or we have no nice peers and this one might do.
        if (!state.fSyncStarted && !pto->fClient && !pto->fDisconnect && fFetch /*&& !fImporting*/ && !fReindex) {
            // Only actively request headers from a single peer, unless we're close to end of initial download.
            if (nSyncStarted == 0 || pindexBestHeader->GetBlockTime() >
                                         GetAdjustedTime() - 6 * 60 * 60) { // NOTE: was "close to today" and 24h in Bitcoin
                state.fSyncStarted = true;
                nSyncStarted++;

                pto->PushMessage(NetMsgType::GETBLOCKS, chainActive.GetLocator(chainActive.Tip()), UINT256_ZERO);
            }
        }

        // Resend wallet transactions that haven't gotten in a block yet
        // Except during reindex, importing and IBD, when old wallet
        // transactions become unconfirmed and spams other nodes.
        if (!fReindex && !fImporting && !IsInitialBlockDownload()) {
            GetMainSignals().Broadcast();
        }

        //
        // Message: inventory
        //
        std::vector<CInv> vInv;
        std::vector<CInv> vInvWait;
        {
            bool fSendTrickle = pto->fWhitelisted;
            if (pto->nNextInvSend < nNow) {
                fSendTrickle = true;
                pto->nNextInvSend = PoissonNextSend(nNow, AVG_INVENTORY_BROADCAST_INTERVAL);
            }
            LOCK(pto->cs_inventory);
            vInv.reserve(pto->vInventoryToSend.size());
            vInvWait.reserve(pto->vInventoryToSend.size());
            for (const CInv& inv : pto->vInventoryToSend) {
                if (inv.type == MSG_TX && pto->filterInventoryKnown.contains(inv.hash))
                    continue;

                // trickle out tx inv to protect privacy
                if (inv.type == MSG_TX && !fSendTrickle) {
                    // 1/4 of tx invs blast to all immediately
                    static uint256 hashSalt;
                    if (hashSalt.IsNull())
                        hashSalt = GetRandHash();
                    uint256 hashRand = inv.hash ^ hashSalt;
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    bool fTrickleWait = ((hashRand & 3) != 0);

                    if (fTrickleWait) {
                        vInvWait.push_back(inv);
                        continue;
                    }
                }

                pto->filterInventoryKnown.insert(inv.hash);

                vInv.push_back(inv);
                if (vInv.size() >= 1000) {
                    pto->PushMessage(NetMsgType::INV, vInv);
                    vInv.clear();
                }
            }
            pto->vInventoryToSend = vInvWait;
        }
        if (!vInv.empty())
            pto->PushMessage(NetMsgType::INV, vInv);

        // Detect whether we're stalling
        nNow = GetTimeMicros();
        if (!pto->fDisconnect && state.nStallingSince &&
            state.nStallingSince < nNow - 1000000 * BLOCK_STALLING_TIMEOUT) {
            // Stalling only triggers when the block download window cannot move. During normal steady state,
            // the download window should be much larger than the to-be-downloaded set of blocks, so disconnection
            // should only happen during initial block download.
            LogPrintf("Peer=%d is stalling block download, disconnecting\n", pto->id);
            pto->fDisconnect = true;
        }
        // In case there is a block that has been in flight from this peer for (2 + 0.5 * N) times the block interval
        // (with N the number of validated blocks that were in flight at the time it was requested), disconnect due to
        // timeout. We compensate for in-flight blocks to prevent killing off peers due to our own downstream link
        // being saturated. We only count validated in-flight blocks so peers can't advertise nonexisting block hashes
        // to unreasonably increase our timeout.
        if (!pto->fDisconnect && state.vBlocksInFlight.size() > 0 && state.vBlocksInFlight.front().nTime < nNow - 500000 * Params().TargetSpacing() * (4 + state.vBlocksInFlight.front().nValidatedQueuedBefore)) {
            LogPrintf("Timeout downloading block %s from peer=%d, disconnecting\n",
                state.vBlocksInFlight.front().hash.ToString(), pto->id);
            pto->fDisconnect = true;
        }

        //
        // Message: getdata (blocks)
        //
        std::vector<CInv> vGetData;
        if (!pto->fDisconnect && !pto->fClient && fFetch && state.nBlocksInFlight < MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
            std::vector<CBlockIndex*> vToDownload;
            NodeId staller = -1;
            FindNextBlocksToDownload(pto->GetId(), MAX_BLOCKS_IN_TRANSIT_PER_PEER - state.nBlocksInFlight, vToDownload,
                staller);
            for (CBlockIndex* pindex : vToDownload) {
                vGetData.push_back(CInv(MSG_BLOCK, pindex->GetBlockHash()));
                MarkBlockAsInFlight(pto->GetId(), pindex->GetBlockHash(), pindex);
                LogPrintf("Requesting block %s (%d) peer=%d\n", pindex->GetBlockHash().ToString(),
                    pindex->nHeight, pto->id);
            }
            if (state.nBlocksInFlight == 0 && staller != -1) {
                if (State(staller)->nStallingSince == 0) {
                    State(staller)->nStallingSince = nNow;
                    LogPrint(BCLog::NET, "Stall started peer=%d\n", staller);
                }
            }
        }

        //
        // Message: getdata (non-blocks)
        //
        while (!pto->fDisconnect && !pto->mapAskFor.empty() && (*pto->mapAskFor.begin()).first <= nNow) {
            const CInv& inv = (*pto->mapAskFor.begin()).second;
            if (!AlreadyHave(inv)) {
                LogPrint(BCLog::NET, "Requesting %s peer=%d\n", inv.ToString(), pto->id);
                vGetData.push_back(inv);
                if (vGetData.size() >= 1000) {
                    pto->PushMessage(NetMsgType::GETDATA, vGetData);
                    vGetData.clear();
                }
            }
            pto->mapAskFor.erase(pto->mapAskFor.begin());
        }
        if (!vGetData.empty())
            pto->PushMessage(NetMsgType::GETDATA, vGetData);
    }
    return true;
}


bool CBlockUndo::WriteToDisk(CDiskBlockPos& pos, const uint256& hashBlock)
{
    // Open history file to append
    CAutoFile fileout(OpenUndoFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("CBlockUndo::WriteToDisk : OpenUndoFile failed");

    // Write index header
    unsigned int nSize = fileout.GetSerializeSize(*this);
    fileout << FLATDATA(Params().MessageStart()) << nSize;

    // Write undo data
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0)
        return error("CBlockUndo::WriteToDisk : ftell failed");
    pos.nPos = (unsigned int)fileOutPos;
    fileout << *this;

    // calculate & write checksum
    CHashWriter hasher(SER_GETHASH, PROTOCOL_VERSION);
    hasher << hashBlock;
    hasher << *this;
    fileout << hasher.GetHash();

    return true;
}

bool CBlockUndo::ReadFromDisk(const CDiskBlockPos& pos, const uint256& hashBlock)
{
    // Open history file to read
    CAutoFile filein(OpenUndoFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("CBlockUndo::ReadFromDisk : OpenBlockFile failed");

    // Read block
    uint256 hashChecksum;
    try {
        filein >> *this;
        filein >> hashChecksum;
    } catch (const std::exception& e) {
        return error("%s : Deserialize or I/O error - %s", __func__, e.what());
    }

    // Verify checksum
    CHashWriter hasher(SER_GETHASH, PROTOCOL_VERSION);
    hasher << hashBlock;
    hasher << *this;
    if (hashChecksum != hasher.GetHash())
        return error("CBlockUndo::ReadFromDisk : Checksum mismatch");

    return true;
}

std::string CBlockFileInfo::ToString() const
{
    return strprintf("CBlockFileInfo(blocks=%u, size=%u, heights=%u...%u, time=%s...%s)", nBlocks, nSize, nHeightFirst,
        nHeightLast, DateTimeStrFormat("%Y-%m-%d", nTimeFirst), DateTimeStrFormat("%Y-%m-%d", nTimeLast));
}


class CMainCleanup
{
public:
    CMainCleanup() {}

    ~CMainCleanup()
    {
        // block headers
        BlockMap::iterator it1 = mapBlockIndex.begin();
        for (; it1 != mapBlockIndex.end(); it1++)
            delete (*it1).second;
        mapBlockIndex.clear();

        // orphan transactions
        mapOrphanTransactions.clear();
        mapOrphanTransactionsByPrev.clear();
    }
};
