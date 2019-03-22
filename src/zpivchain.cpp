// Copyright (c) 2018 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zpivchain.h"
//#include "invalid.h"
#include "main.h"
#include "txdb.h"
#include "ui_interface.h"
#include "zerocoin.h"
#include "wallet/wallet.h"

// 6 comes from OPCODE (1) + vch.size() (1) + BIGNUM size (4)
#define SCRIPT_OFFSET 6
// For Script size (BIGNUM/Uint256 size)
#define BIGNUM_SIZE   4

bool BlockToMintValueVector(const CBlock& block, const libzerocoin::CoinDenomination denom, vector<CBigNum>& vValues)
{
    for (const CTransaction& tx : block.vtx) {
        if(!tx.IsZerocoinMint())
            continue;

        for (const CTxOut& txOut : tx.vout) {
            if(!txOut.scriptPubKey.IsZerocoinMint())
                continue;

            CValidationState state;
            libzerocoin::PublicCoin coin(ZCParamsV2);
            if(!TxOutToPublicCoin(txOut, coin, state))
                return false;

            if (coin.getDenomination() != denom)
                continue;

            vValues.push_back(coin.getValue());
        }
    }

    return true;
}

bool BlockToPubcoinList(const CBlock& block, std::list<libzerocoin::PublicCoin>& listPubcoins)
{
    for (const CTransaction& tx : block.vtx) {
        if(!tx.IsZerocoinMint())
            continue;

        for (unsigned int i = 0; i < tx.vout.size(); i++) {
            const CTxOut txOut = tx.vout[i];
            if(!txOut.scriptPubKey.IsZerocoinMint())
                continue;

            CValidationState state;
            libzerocoin::PublicCoin pubCoin(ZCParamsV2);
            if(!TxOutToPublicCoin(txOut, pubCoin, state))
                return false;

            listPubcoins.emplace_back(pubCoin);
        }
    }

    return true;
}

//return a list of zerocoin mints contained in a specific block
bool BlockToZerocoinMintList(const CBlock& block, std::list<CZerocoinEntry>& vMints)
{
    for (const CTransaction& tx : block.vtx) {
        if(!tx.IsZerocoinMint())
            continue;

        for (unsigned int i = 0; i < tx.vout.size(); i++) {
            const CTxOut txOut = tx.vout[i];
            if(!txOut.scriptPubKey.IsZerocoinMint())
                continue;

            CValidationState state;
            libzerocoin::PublicCoin pubCoin(ZCParamsV2);
            if(!TxOutToPublicCoin(txOut, pubCoin, state))
                return false;

            Bignum value = const_cast<Bignum&>(pubCoin.getValue());
            Bignum randomness = 0;
            Bignum serialNumber = 0;
            CZerocoinEntry zerocoin = CZerocoinEntry(pubCoin.getDenomination(), value, randomness, serialNumber, false);
            vMints.push_back(zerocoin);
        }
    }

    return true;
}

void FindMints(std::vector<CMintMeta> vMintsToFind, std::vector<CMintMeta>& vMintsToUpdate, std::vector<CMintMeta>& vMissingMints)
{
    // see which mints are in our public zerocoin database. The mint should be here if it exists, unless
    // something went wrong
    for (CMintMeta meta : vMintsToFind) {
        uint256 txHash;
        if (!zerocoinDB->ReadCoinMint(GetPubCoinHash(meta.pubcoin), txHash)) {
            vMissingMints.push_back(meta);
            continue;
        }

        // make sure the txhash and block height meta data are correct for this mint
        CTransaction tx;
        uint256 hashBlock;
        if (!GetTransaction(txHash, tx, Params().GetConsensus(), hashBlock, true)) {
            LogPrintf("%s : cannot find tx %s\n", __func__, txHash.GetHex());
            vMissingMints.push_back(meta);
            continue;
        }

        if (!mapBlockIndex.count(hashBlock)) {
            LogPrintf("%s : cannot find block %s\n", __func__, hashBlock.GetHex());
            vMissingMints.push_back(meta);
            continue;
        }

        //see if this mint is spent
        uint256 hashTxSpend;
        hashTxSpend.SetNull();
        bool fSpent = zerocoinDB->ReadCoinSpend(meta.hashSerial, hashTxSpend);

        //if marked as spent, check that it actually made it into the chain
        CTransaction txSpend;
        uint256 hashBlockSpend;
        if (fSpent && !GetTransaction(hashTxSpend, txSpend, Params().GetConsensus(), hashBlockSpend, true)) {
            LogPrintf("%s : cannot find spend tx %s\n", __func__, hashTxSpend.GetHex());
            meta.isUsed = false;
            vMintsToUpdate.push_back(meta);
            continue;
        }

        //The mint has been incorrectly labelled as spent in zerocoinDB and needs to be undone
        int nHeightTx = 0;
        uint256 hashSerial = meta.hashSerial;
        uint256 txidSpend;
        if (fSpent && !IsSerialInBlockchain(hashSerial, nHeightTx, txidSpend)) {
            LogPrintf("%s : cannot find block %s. Erasing coinspend from zerocoinDB.\n", __func__, hashBlockSpend.GetHex());
            meta.isUsed = false;
            vMintsToUpdate.push_back(meta);
            continue;
        }

        // is the denomination correct?
        for (auto& out : tx.vout) {
            if (!out.scriptPubKey.IsZerocoinMint())
                continue;
            libzerocoin::PublicCoin pubcoin(ZCParamsV2);
            CValidationState state;
            TxOutToPublicCoin(out, pubcoin, state);
            if (pubcoin.getValue() == meta.pubcoin && pubcoin.getDenomination() != meta.denom) {
                LogPrintf("%s: found mismatched denom pubcoinhash = %s\n", __func__, GetPubCoinHash(meta.pubcoin).GetHex());
                meta.denom = pubcoin.getDenomination();
                vMintsToUpdate.emplace_back(meta);
            }
        }

        // if meta data is correct, then no need to update
        if (meta.txid == txHash && meta.nHeight == mapBlockIndex[hashBlock]->nHeight && meta.isUsed == fSpent)
            continue;

        //mark this mint for update
        meta.txid = txHash;
        meta.nHeight = mapBlockIndex[hashBlock]->nHeight;
        meta.isUsed = fSpent;
        LogPrintf("%s: found updates for pubcoinhash = %s\n", __func__, GetPubCoinHash(meta.pubcoin).GetHex());

        vMintsToUpdate.push_back(meta);
    }
}

bool GetZerocoinMint(const CBigNum& bnPubcoin, uint256& txHash)
{
    txHash.SetNull();
    return zerocoinDB->ReadCoinMint(bnPubcoin, txHash);
}

bool IsSerialKnown(const CBigNum& bnSerial)
{
    uint256 txHash;
    txHash.SetNull();
    return zerocoinDB->ReadCoinSpend(bnSerial, txHash);
}

bool IsSerialInBlockchain(const CBigNum& bnSerial, int& nHeightTx)
{
    uint256 txHash;
    txHash.SetNull();
    // if not in zerocoinDB then its not in the blockchain
    if (!zerocoinDB->ReadCoinSpend(bnSerial, txHash))
        return false;

    return IsTransactionInChain(txHash, nHeightTx);
}

bool IsSerialInBlockchain(const uint256& hashSerial, int& nHeightTx, uint256& txidSpend)
{
    CTransaction tx;
    return IsSerialInBlockchain(hashSerial, nHeightTx, txidSpend, tx);
}

bool IsSerialInBlockchain(const uint256& hashSerial, int& nHeightTx, uint256& txidSpend, CTransaction& tx)
{
    txidSpend.SetNull();
    // if not in zerocoinDB then its not in the blockchain
    if (!zerocoinDB->ReadCoinSpend(hashSerial, txidSpend))
        return false;

    return IsTransactionInChain(txidSpend, nHeightTx, tx);
}

std::string ReindexZerocoinDB()
{
    if (!zerocoinDB->WipeCoins("spends") || !zerocoinDB->WipeCoins("mints")) {
        return _("Failed to wipe zerocoinDB");
    }

    uiInterface.ShowProgress(_("Reindexing zerocoin database..."), 0);

    CBlockIndex* pindex = chainActive[0];
    std::vector<std::pair<libzerocoin::CoinSpend, uint256> > vSpendInfo;
    std::vector<std::pair<libzerocoin::PublicCoin, uint256> > vMintInfo;
    while (pindex) {
        uiInterface.ShowProgress(_("Reindexing zerocoin database..."), std::max(1, std::min(99, (int)((double)(pindex->nHeight) / (double)(chainActive.Height()) * 100))));

        if (pindex->nHeight % 1000 == 0)
            LogPrintf("Reindexing zerocoin : block %d...\n", pindex->nHeight);

        CBlock block;
        if (!ReadBlockFromDisk(block, pindex, Params().GetConsensus())) {
            return _("Reindexing zerocoin failed");
        }

        for (const CTransaction& tx : block.vtx) {
            for (unsigned int i = 0; i < tx.vin.size(); i++) {
                if (tx.IsCoinBase())
                    break;

                if (tx.ContainsZerocoins()) {
                    uint256 txid = tx.GetHash();
                    //Record Serials
                    if (tx.IsZerocoinSpend()) {
                        for (auto& in : tx.vin) {
                            if (!in.scriptSig.IsZerocoinSpend())
                                continue;

                            libzerocoin::CoinSpend spend = TxInToZerocoinSpend(in);
                            vSpendInfo.push_back(make_pair(spend, txid));
                        }
                    }

                    //Record mints
                    if (tx.IsZerocoinMint()) {
                        for (auto& out : tx.vout) {
                            if (!out.scriptPubKey.IsZerocoinMint())
                                continue;

                            CValidationState state;
                            libzerocoin::PublicCoin coin(ZCParamsV2);
                            TxOutToPublicCoin(out, coin, state);
                            vMintInfo.push_back(make_pair(coin, txid));
                        }
                    }
                }
            }
        }

        // Flush the zerocoinDB to disk every 100 blocks
        if (pindex->nHeight % 100 == 0) {
            if ((!vSpendInfo.empty() && !zerocoinDB->WriteCoinSpendBatch(vSpendInfo)) || (!vMintInfo.empty() && !zerocoinDB->WriteCoinMintBatch(vMintInfo)))
                return _("Error writing zerocoinDB to disk");
            vSpendInfo.clear();
            vMintInfo.clear();
        }

        pindex = chainActive.Next(pindex);
    }
    uiInterface.ShowProgress("", 100);

    // Final flush to disk in case any remaining information exists
    if ((!vSpendInfo.empty() && !zerocoinDB->WriteCoinSpendBatch(vSpendInfo)) || (!vMintInfo.empty() && !zerocoinDB->WriteCoinMintBatch(vMintInfo)))
        return _("Error writing zerocoinDB to disk");

    uiInterface.ShowProgress("", 100);

    return "";
}

bool RemoveSerialFromDB(const CBigNum& bnSerial)
{
    return zerocoinDB->EraseCoinSpend(bnSerial);
}

libzerocoin::CoinSpend TxInToZerocoinSpend(const CTxIn& txin)
{
    // extract the CoinSpend from the txin
    std::vector<char, zero_after_free_allocator<char> > dataTxIn;
    dataTxIn.insert(dataTxIn.end(), txin.scriptSig.begin() + BIGNUM_SIZE, txin.scriptSig.end());
    CDataStream serializedCoinSpend(dataTxIn, SER_NETWORK, PROTOCOL_VERSION);
    //bool fModulusV2 = (txin.nSequence >= ZC_MODULUS_V2_BASE_ID);
    //libzerocoin::Params* paramsAccumulator = fModulusV2 ? ZCParamsV2 : ZCParams;
    libzerocoin::CoinSpend spend(ZCParamsV2, serializedCoinSpend);

    return spend;
}

bool TxOutToPublicCoin(const CTxOut& txout, libzerocoin::PublicCoin& pubCoin, CValidationState& state)
{
    CBigNum publicZerocoin;
    vector<unsigned char> vchZeroMint;
    vchZeroMint.insert(vchZeroMint.end(), txout.scriptPubKey.begin() + SCRIPT_OFFSET,
                       txout.scriptPubKey.begin() + txout.scriptPubKey.size());
    publicZerocoin.setvch(vchZeroMint);

    libzerocoin::CoinDenomination denomination = libzerocoin::AmountToZerocoinDenomination(txout.nValue);
    LogPrint("zero", "%s ZCPRINT denomination %d pubcoin %s\n", __func__, denomination, publicZerocoin.GetHex());
    if (denomination == libzerocoin::ZQ_ERROR)
        return state.DoS(100, error("TxOutToPublicCoin : txout.nValue is not correct"));

    libzerocoin::PublicCoin checkPubCoin(ZCParamsV2, publicZerocoin, denomination);
    pubCoin = checkPubCoin;

    return true;
}

//return a list of zerocoin spends contained in a specific block, list may have many denominations
std::list<libzerocoin::CoinDenomination> ZerocoinSpendListFromBlock(const CBlock& block)
{
    std::list<libzerocoin::CoinDenomination> vSpends;
    for (const CTransaction& tx : block.vtx) {
        if (!tx.IsZerocoinSpend())
            continue;

        for (const CTxIn& txin : tx.vin) {
            if (!txin.scriptSig.IsZerocoinSpend())
                continue;

            libzerocoin::CoinDenomination c = libzerocoin::IntToZerocoinDenomination(txin.nSequence);
            vSpends.push_back(c);
        }
    }
    return vSpends;
}
