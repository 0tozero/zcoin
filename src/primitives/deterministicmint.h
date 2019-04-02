// Copyright (c) 2018 The PIVX developers
// Copyright (c) 2019 Zcoin
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ZCOIN_DETERMINISTICMINT_H
#define ZCOIN_DETERMINISTICMINT_H

#include <libzerocoin/Zerocoin.h>
#include "primitives/zerocoin.h"

//struct that is safe to store essential mint data, without holding any information that allows for actual spending (serial, randomness, private key)
class CDeterministicMint
{
private:
    uint32_t nCount;
    uint256 hashSeed;
    uint256 hashSerial;
    Bignum pubcoin;
    uint256 txid;
    int nHeight;
    int denom;
    bool isUsed;

public:
    CDeterministicMint();
    CDeterministicMint(const uint32_t& nCount, const uint256& hashSeed, const uint256& hashSerial, const Bignum& pubcoin);

    libzerocoin::CoinDenomination GetDenomination() const { return (libzerocoin::CoinDenomination)denom; }
    uint32_t GetCount() const { return nCount; }
    int GetHeight() const { return nHeight; }
    uint256 GetSeedHash() const { return hashSeed; }
    uint256 GetSerialHash() const { return hashSerial; }
    Bignum GetPubcoin() const { return pubcoin; }
    uint256 GetPubcoinHash() const { return GetPubCoinHash(pubcoin); }
    uint256 GetTxHash() const { return txid; }
    bool IsUsed() const { return isUsed; }
    void SetDenomination(const libzerocoin::CoinDenomination denom) { this->denom = denom; }
    void SetHeight(const int& nHeight) { this->nHeight = nHeight; }
    void SetNull();
    void SetTxHash(const uint256& txid) { this->txid = txid; }
    void SetUsed(const bool isUsed) { this->isUsed = isUsed; }
    std::string ToString() const;

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(nCount);
        READWRITE(hashSeed);
        READWRITE(hashSerial);
        READWRITE(pubcoin);
        READWRITE(txid);
        READWRITE(nHeight);
        READWRITE(denom);
        READWRITE(isUsed);
    };
};

#endif //ZCOIN_DETERMINISTICMINT_H
