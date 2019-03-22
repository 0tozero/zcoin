// Copyright (c) 2018 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_ZPIVTRACKER_H
#define PIVX_ZPIVTRACKER_H

#include "primitives/zerocoin.h"
#include <list>

class CDeterministicMint;
class CzPIVWallet;

class CzPIVTracker
{
private:
    bool fInitialized;
    std::string strWalletFile;
    std::map<uint256, CMintMeta> mapSerialHashes;
    std::map<uint256, uint256> mapPendingSpends; //serialhash, txid of spend
    bool UpdateStatusInternal(const std::set<uint256>& setMempool, CMintMeta& mint);
public:
    CzPIVTracker(std::string strWalletFile);
    ~CzPIVTracker();
    void Add(const CDeterministicMint& dMint, bool isNew = false, bool isArchived = false, CzPIVWallet* zPIVWallet = NULL);
    void Add(const CZerocoinEntry& zerocoin, bool isNew = false, bool isArchived = false);
    bool Archive(CMintMeta& meta);
    bool HasPubcoin(const CBigNum& pubcoin) const;
    bool HasPubcoinHash(const uint256& hashPubcoin) const;
    bool HasSerial(const CBigNum& bnSerial) const;
    bool HasSerialHash(const uint256& hashSerial) const;
    bool HasMintTx(const uint256& txid);
    bool IsEmpty() const { return mapSerialHashes.empty(); }
    void Init();
    CMintMeta Get(const uint256& hashSerial);
    CAmount GetBalance(bool fConfirmedOnly, bool fUnconfirmedOnly) const;
    std::vector<uint256> GetSerialHashes();
    std::vector<CMintMeta> GetMints(bool fConfirmedOnly) const;
    CAmount GetUnconfirmedBalance() const;
    std::set<CMintMeta> ListMints(bool fUnusedOnly, bool fMatureOnly, bool fUpdateStatus, bool fWrongSeed = false);
    void RemovePending(const uint256& txid);
    bool UnArchive(const uint256& hashPubcoin, bool isDeterministic);
    bool UpdateZerocoinEntry(const CZerocoinEntry& zerocoin);
    bool UpdateState(const CMintMeta& meta);
    void Clear();
};

#endif //PIVX_ZPIVTRACKER_H