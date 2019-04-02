// Copyright (c) 2018 The PIVX developers
// Copyright (c) 2019 Zcoin
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/deterministicmint.h>
#include "zerocointracker.h"
#include "util.h"
#include "sync.h"
#include "txdb.h"
#include "wallet/walletdb.h"
#include "zerocoinwallet.h"
#include "libzerocoin/Zerocoin.h"
#include "main.h"
#include "zerocoin.h"
//#include "accumulators.h"

using namespace std;

CZerocoinTracker::CZerocoinTracker(std::string strWalletFile)
{
    this->strWalletFile = strWalletFile;
    mapSerialHashes.clear();
    mapPendingSpends.clear();
    fInitialized = false;
}

CZerocoinTracker::~CZerocoinTracker()
{
    mapSerialHashes.clear();
    mapPendingSpends.clear();
}

void CZerocoinTracker::Init()
{
    //Load all CZerocoinEntries and CDeterministicMints from the database
    if (!fInitialized) {
        ListMints(false, false, true);
        fInitialized = true;
    }
}

bool CZerocoinTracker::Archive(CMintMeta& meta)
{
    uint256 hashPubcoin = GetPubCoinHash(meta.pubcoin);

    if (mapSerialHashes.count(meta.hashSerial))
        mapSerialHashes.at(meta.hashSerial).isArchived = true;

    CWalletDB walletdb(strWalletFile);
    CZerocoinEntry zerocoin;
    // if (walletdb.ReadZerocoinEntry(meta.pubcoin, zerocoin)) {
    //     if (!CWalletDB(strWalletFile).ArchiveMintOrphan(zerocoin))
    //         return error("%s: failed to archive zerocoinmint", __func__);
    // } else {
    //     //failed to read mint from DB, try reading deterministic
    //     CDeterministicMint dMint;
    //     if (!walletdb.ReadDeterministicMint(hashPubcoin, dMint))
    //         return error("%s: could not find pubcoinhash %s in db", __func__, hashPubcoin.GetHex());
    //     if (!walletdb.ArchiveDeterministicOrphan(dMint))
    //         return error("%s: failed to archive deterministic ophaned mint", __func__);
    // }

    LogPrintf("%s: archived pubcoinhash %s\n", __func__, hashPubcoin.GetHex());
    return true;
}

bool CZerocoinTracker::UnArchive(const uint256& hashPubcoin, bool isDeterministic)
{
    CWalletDB walletdb(strWalletFile);
    if (isDeterministic) {
        CDeterministicMint dMint;
        if (!walletdb.UnarchiveDeterministicMint(hashPubcoin, dMint))
            return error("%s: failed to unarchive deterministic mint", __func__);
        Add(dMint, false);
    } else {
        CZerocoinEntry zerocoin;
        if (!walletdb.UnarchiveZerocoinMint(hashPubcoin, zerocoin))
            return error("%s: failed to unarchivezerocoin mint", __func__);
        Add(zerocoin, false);
    }

    LogPrintf("%s: unarchived %s\n", __func__, hashPubcoin.GetHex());
    return true;
}

bool CZerocoinTracker::Get(const uint256 &hashSerial, CMintMeta& mMeta)
{
    if (!mapSerialHashes.count(hashSerial))
        mMeta = CMintMeta();
        return false;

    mMeta = mapSerialHashes.at(hashSerial);
    return true;
}

CMintMeta CZerocoinTracker::GetMetaFromPubcoin(const uint256& hashPubcoin)
{
    for (auto it : mapSerialHashes) {
        CMintMeta meta = it.second;
        if (GetPubCoinHash(meta.pubcoin) == hashPubcoin)
            return meta;
    }

    return CMintMeta();
}

std::vector<uint256> CZerocoinTracker::GetSerialHashes()
{
    vector<uint256> vHashes;
    for (auto it : mapSerialHashes) {
        if (it.second.isArchived)
            continue;

        vHashes.emplace_back(it.first);
    }


    return vHashes;
}

CAmount CZerocoinTracker::GetBalance(bool fConfirmedOnly, bool fUnconfirmedOnly) const
{
    CAmount nTotal = 0;
    //! zerocoin specific fields
    std::map<libzerocoin::CoinDenomination, unsigned int> myZerocoinSupply;
    for (auto& denom : libzerocoin::zerocoinDenomList) {
        myZerocoinSupply.insert(make_pair(denom, 0));
    }

    {
        //LOCK(cs_pivtracker);
        // Get Unused coins
        for (auto& it : mapSerialHashes) {
            CMintMeta meta = it.second;
            if (meta.isUsed || meta.isArchived)
                continue;
            bool fConfirmed = ((meta.nHeight < chainActive.Height() - ZC_MINT_CONFIRMATIONS) && !(meta.nHeight == 0));
            if (fConfirmedOnly && !fConfirmed)
                continue;
            if (fUnconfirmedOnly && fConfirmed)
                continue;

            nTotal += libzerocoin::ZerocoinDenominationToAmount(meta.denom);
            myZerocoinSupply.at(meta.denom)++;
        }
    }

    if (nTotal < 0 ) nTotal = 0; // Sanity never hurts

    return nTotal;
}

CAmount CZerocoinTracker::GetUnconfirmedBalance() const
{
    return GetBalance(false, true);
}

std::vector<CMintMeta> CZerocoinTracker::GetMints(bool fConfirmedOnly) const
{
    vector<CMintMeta> vMints;
    for (auto& it : mapSerialHashes) {
        CMintMeta mint = it.second;
        if (mint.isArchived || mint.isUsed)
            continue;
        bool fConfirmed = (mint.nHeight < chainActive.Height() - ZC_MINT_CONFIRMATIONS);
        if (fConfirmedOnly && !fConfirmed)
            continue;
        vMints.emplace_back(mint);
    }
    return vMints;
}

//Does a mint in the tracker have this txid
bool CZerocoinTracker::HasMintTx(const uint256& txid)
{
    for (auto it : mapSerialHashes) {
        if (it.second.txid == txid)
            return true;
    }

    return false;
}

bool CZerocoinTracker::HasPubcoin(const CBigNum &pubcoin) const
{
    // Check if this mint's pubcoin value belongs to our mapSerialHashes (which includes hashpubcoin values)
    uint256 hash = GetPubCoinHash(pubcoin);
    return HasPubcoinHash(hash);
}

bool CZerocoinTracker::HasPubcoinHash(const uint256& hashPubcoin) const
{
    for (auto it : mapSerialHashes) {
        CMintMeta meta = it.second;
        if (GetPubCoinHash(meta.pubcoin) == hashPubcoin)
            return true;
    }
    return false;
}

bool CZerocoinTracker::HasSerial(const CBigNum& bnSerial) const
{
    uint256 hash = GetSerialHash(bnSerial);
    return HasSerialHash(hash);
}

bool CZerocoinTracker::HasSerialHash(const uint256& hashSerial) const
{
    auto it = mapSerialHashes.find(hashSerial);
    return it != mapSerialHashes.end();
}

bool CZerocoinTracker::UpdateZerocoinEntry(const CZerocoinEntry& zerocoin)
{
    if (!HasSerial(zerocoin.serialNumber))
        return error("%s: zerocoin %s is not known", __func__, zerocoin.value.GetHex());

    uint256 hashSerial = GetSerialHash(zerocoin.serialNumber);

    //Update the meta object
    CMintMeta meta;
    Get(hashSerial, meta);
    meta.isUsed = zerocoin.IsUsed;
    meta.denom = (libzerocoin::CoinDenomination)zerocoin.denomination;
    meta.nHeight = zerocoin.nHeight;
    mapSerialHashes.at(hashSerial) = meta;

    //Write to db
    return CWalletDB(strWalletFile).WriteZerocoinEntry(zerocoin);
}

bool CZerocoinTracker::UpdateState(const CMintMeta& meta)
{
    uint256 hashPubcoin = GetPubCoinHash(meta.pubcoin);
    CWalletDB walletdb(strWalletFile);

    if (meta.isDeterministic) {
        CDeterministicMint dMint;
        if (!walletdb.ReadDeterministicMint(hashPubcoin, dMint)) {
            // Check archive just in case
            if (!meta.isArchived)
                return error("%s: failed to read deterministic mint from database", __func__);

            // Unarchive this mint since it is being requested and updated
            if (!walletdb.UnarchiveDeterministicMint(hashPubcoin, dMint))
                return error("%s: failed to unarchive deterministic mint from database", __func__);
        }

        dMint.SetHeight(meta.nHeight);
        dMint.SetUsed(meta.isUsed);
        dMint.SetDenomination(meta.denom);

        if (!walletdb.WriteDeterministicMint(dMint))
            return error("%s: failed to update deterministic mint when writing to db", __func__);
    } else {
        CZerocoinEntry zerocoin;
        // if (!walletdb.ReadZerocoinEntry(meta.pubcoin, zerocoin))
        //     return error("%s: failed to read mint from database", __func__);

        zerocoin.nHeight = meta.nHeight;
        zerocoin.IsUsed = meta.isUsed;
        zerocoin.denomination = meta.denom;

        if (!walletdb.WriteZerocoinEntry(zerocoin))
            return error("%s: failed to write mint to database", __func__);
    }

    mapSerialHashes[meta.hashSerial] = meta;

    return true;
}

void CZerocoinTracker::Add(const CDeterministicMint& dMint, bool isNew, bool isArchived, CZerocoinWallet* zerocoinWallet)
{
    bool iszerocoinWalletInitialized = (NULL != zerocoinWallet);
    CMintMeta meta;
    meta.pubcoin = dMint.GetPubcoin();
    meta.nHeight = dMint.GetHeight();
    meta.txid = dMint.GetTxHash();
    meta.isUsed = dMint.IsUsed();
    meta.hashSerial = dMint.GetSerialHash();
    meta.denom = dMint.GetDenomination();
    meta.isArchived = isArchived;
    meta.isDeterministic = true;
    if (! iszerocoinWalletInitialized)
        zerocoinWallet = new CZerocoinWallet(strWalletFile);
    meta.isSeedCorrect = zerocoinWallet->CheckSeed(dMint);
    if (! iszerocoinWalletInitialized)
        delete zerocoinWallet;
    mapSerialHashes[meta.hashSerial] = meta;

    if (isNew)
        CWalletDB(strWalletFile).WriteDeterministicMint(dMint);
}

void CZerocoinTracker::Add(const CZerocoinEntry& zerocoin, bool isNew, bool isArchived)
{
    CMintMeta meta;
    meta.pubcoin = zerocoin.value;
    meta.nHeight = zerocoin.nHeight;
    //meta.txid = zerocoin.GetTxHash();
    meta.isUsed = zerocoin.IsUsed;
    meta.hashSerial = GetSerialHash(zerocoin.serialNumber);
    meta.denom = (libzerocoin::CoinDenomination)zerocoin.denomination;
    meta.isArchived = isArchived;
    meta.isDeterministic = false;
    meta.isSeedCorrect = true;
    mapSerialHashes[meta.hashSerial] = meta;

    if (isNew)
        CWalletDB(strWalletFile).WriteZerocoinEntry(zerocoin);
}

void CZerocoinTracker::SetPubcoinUsed(const uint256& hashPubcoin, const uint256& txid)
{
    if (!HasPubcoinHash(hashPubcoin))
        return;
    CMintMeta meta = GetMetaFromPubcoin(hashPubcoin);
    meta.isUsed = true;
    mapPendingSpends.insert(make_pair(meta.hashSerial, txid));
    UpdateState(meta);
}

void CZerocoinTracker::SetPubcoinNotUsed(const uint256& hashPubcoin)
{
    if (!HasPubcoinHash(hashPubcoin))
        return;
    CMintMeta meta = GetMetaFromPubcoin(hashPubcoin);
    meta.isUsed = false;

    if (mapPendingSpends.count(meta.hashSerial))
        mapPendingSpends.erase(meta.hashSerial);

    UpdateState(meta);
}

void CZerocoinTracker::RemovePending(const uint256& txid)
{
    uint256 hashSerial;
    for (auto it : mapPendingSpends) {
        if (it.second == txid) {
            hashSerial = it.first;
            break;
        }
    }
    if (UintToArith256(hashSerial) > 0)
        mapPendingSpends.erase(hashSerial);
}

bool CZerocoinTracker::UpdateStatusInternal(const std::set<uint256>& setMempool, CMintMeta& mint)
{
    uint256 hashPubcoin = GetPubCoinHash(mint.pubcoin);
    //! Check whether this mint has been spent and is considered 'pending' or 'confirmed'
    // If there is not a record of the block height, then look it up and assign it
    uint256 txidMint;
    bool isMintInChain = CZerocoinState::GetZerocoinState()->HasCoin(mint.pubcoin);
    if(isMintInChain)
        txidMint = GetMetaFromPubcoin(hashPubcoin).txid;

    //See if there is internal record of spending this mint (note this is memory only, would reset on restart)
    bool isPendingSpend = static_cast<bool>(mapPendingSpends.count(mint.hashSerial));

    // See if there is a blockchain record of spending this mint
    CBigNum bnSerial;
    bool isConfirmedSpend = CZerocoinState::GetZerocoinState()->IsUsedCoinSerialHash(bnSerial, mint.hashSerial);

    // Double check the mempool for pending spend
    if (isPendingSpend) {
        uint256 txidPendingSpend = mapPendingSpends.at(mint.hashSerial);
        if (!setMempool.count(txidPendingSpend) || isConfirmedSpend) {
            RemovePending(txidPendingSpend);
            isPendingSpend = false;
            LogPrintf("%s : Pending txid %s removed because not in mempool\n", __func__, txidPendingSpend.GetHex());
        }
    }

    bool isUsed = isPendingSpend || isConfirmedSpend;

    if (!mint.nHeight || !isMintInChain || isUsed != mint.isUsed) {
        CTransaction tx;
        uint256 hashBlock;

        // Txid will be marked 0 if there is no knowledge of the final tx hash yet
        if (mint.txid.IsNull()) {
            if (!isMintInChain) {
                LogPrintf("%s : Failed to find mint in zerocoinDB %s\n", __func__, hashPubcoin.GetHex().substr(0, 6));
                mint.isArchived = true;
                Archive(mint);
                return true;
            }
            mint.txid = txidMint;
        }

        if (setMempool.count(mint.txid))
            return true;

        // Check the transaction associated with this mint
        if (!IsInitialBlockDownload() && !GetTransaction(mint.txid, tx, Params().GetConsensus(), hashBlock, true)) {
            LogPrintf("%s : Failed to find tx for mint txid=%s\n", __func__, mint.txid.GetHex());
            mint.isArchived = true;
            Archive(mint);
            return true;
        }

        // An orphan tx if hashblock is in mapBlockIndex but not in chain active
        if (mapBlockIndex.count(hashBlock) && !chainActive.Contains(mapBlockIndex.at(hashBlock))) {
            LogPrintf("%s : Found orphaned mint txid=%s\n", __func__, mint.txid.GetHex());
            mint.isUsed = false;
            mint.nHeight = 0;

            return true;
        }

        // Check that the mint has correct used status
        if (mint.isUsed != isUsed) {
            LogPrintf("%s : Set mint %s isUsed to %d\n", __func__, hashPubcoin.GetHex(), isUsed);
            mint.isUsed = isUsed;
            return true;
        }
    }

    return false;
}

std::set<CMintMeta> CZerocoinTracker::ListMints(bool fUnusedOnly, bool fMatureOnly, bool fUpdateStatus, bool fWrongSeed)
{
    CWalletDB walletdb(strWalletFile);
    if (fUpdateStatus) {
        std::list<CZerocoinEntry> listMintsDB;
        walletdb.ListPubCoin(listMintsDB);
        for (auto& mint : listMintsDB)
            Add(mint);
        LogPrint("zero", "%s: added %d zerocoinmints from DB\n", __func__, listMintsDB.size());

        std::list<CDeterministicMint> listDeterministicDB = walletdb.ListDeterministicMints();

        CZerocoinWallet* zerocoinWallet = new CZerocoinWallet(strWalletFile);
        for (auto& dMint : listDeterministicDB) {
            Add(dMint, false, false, zerocoinWallet);
        }
        delete zerocoinWallet;
        LogPrint("zero", "%s: added %d hdmint from DB\n", __func__, listDeterministicDB.size());
    }

    std::vector<CMintMeta> vOverWrite;
    std::set<CMintMeta> setMints;
    std::set<uint256> setMempool;
    {
        LOCK(mempool.cs);
        mempool.getTransactions(setMempool);
    }

    for (auto& it : mapSerialHashes) {
        CMintMeta mint = it.second;

        //This is only intended for unarchived coins
        if (mint.isArchived)
            continue;

        // Update the metadata of the mints if requested
        if (fUpdateStatus && UpdateStatusInternal(setMempool, mint)) {
            if (mint.isArchived)
                continue;

            // Mint was updated, queue for overwrite
            vOverWrite.emplace_back(mint);
        }

        if (fUnusedOnly && mint.isUsed)
            continue;

        if (fMatureOnly) {
            // Not confirmed
            // TODO DETERMINISTIC
            if (!mint.nHeight || mint.nHeight > chainActive.Height() - ZC_MINT_CONFIRMATIONS)
                continue;
        }

        if (!fWrongSeed && !mint.isSeedCorrect)
            continue;

        setMints.insert(mint);
    }

    //overwrite any updates
    for (CMintMeta& meta : vOverWrite)
        UpdateState(meta);

    return setMints;
}

void CZerocoinTracker::Clear()
{
    mapSerialHashes.clear();
}