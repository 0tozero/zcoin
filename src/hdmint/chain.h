// Copyright (c) 2019 The Zcoin Core Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ZCOIN_HDMINTCHAIN_H
#define ZCOIN_HDMINTCHAIN_H

#include "libzerocoin/Zerocoin.h"
#include "zerocoin.h"
#include <list>
#include <string>

class CBigNum;
struct CMintMeta;
class CTransaction;
class CTxIn;
class CTxOut;
class CValidationState;
class CZerocoinEntry;
class uint256;

bool IsSerialInBlockchain(const Scalar& bnSerial, int& nHeightTx);
bool IsSerialInBlockchain(const uint256& hashSerial, int& nHeightTx, uint256& txidSpend);
bool IsSerialInBlockchain(const uint256& hashSerial, int& nHeightTx, uint256& txidSpend, CTransaction& tx);
bool TxOutToPublicCoin(const CTxOut& txout, sigma::PublicCoinV3& pubCoin, CValidationState& state);

#endif //ZCOIN_HDMINTCHAIN_H