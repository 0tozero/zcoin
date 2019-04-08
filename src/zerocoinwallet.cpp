// Copyright (c) 2017-2018 The PIVX developers
// Copyright (c) 2019 Zcoin
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zerocoinwallet.h"
#include "main.h"
#include "txdb.h"
#include "init.h"
#include "primitives/deterministicmint.h"
#include "wallet/walletdb.h"
#include "wallet/wallet.h"
#include "zerocoin.h"
#include "zerocoinchain.h"

CZerocoinWallet::CZerocoinWallet(std::string strWalletFile)
{
    this->strWalletFile = strWalletFile;
    CWalletDB walletdb(strWalletFile);

    uint256 hashSeed;
    bool fFirstRun = !walletdb.ReadCurrentSeedHash(hashSeed);

    //Don't try to do anything if the wallet is locked.
    if (pwalletMain->IsLocked()) {
        seedMaster.SetNull();
        nCountLastUsed = 0;
        this->mintPool = CMintPool();
        return;
    }

    //First time running, generate master seed
    uint256 seed;
    if (fFirstRun) {
        // Borrow random generator from the key class so that we don't have to worry about randomness
        CKey key;
        key.MakeNewKey(true);
        seed = key.GetPrivKey_256();
        seedMaster = seed;
        LogPrintf("%s: first run of zerocoin wallet detected, new seed generated. Seedhash=%s\n", __func__, Hash(seed.begin(), seed.end()).GetHex());
    } else if (!pwalletMain->GetDeterministicSeed(hashSeed, seed)) {
        LogPrintf("%s: failed to get deterministic seed for hashseed %s\n", __func__, hashSeed.GetHex());
        return;
    }

    if (!SetMasterSeed(seed)) {
        LogPrintf("%s: failed to save deterministic seed for hashseed %s\n", __func__, hashSeed.GetHex());
        return;
    }
    this->mintPool = CMintPool(nCountLastUsed);
}

bool CZerocoinWallet::SetMasterSeed(const uint256& seedMaster, bool fResetCount)
{

    CWalletDB walletdb(strWalletFile);
    if (pwalletMain->IsLocked())
        return false;

    if (!seedMaster.IsNull() && !pwalletMain->AddDeterministicSeed(seedMaster)) {
        return error("%s: failed to set master seed.", __func__);
    }

    this->seedMaster = seedMaster;

    nCountLastUsed = 0;

    if (fResetCount)
        walletdb.WriteZerocoinCount(nCountLastUsed);
    else if (!walletdb.ReadZerocoinCount(nCountLastUsed))
        nCountLastUsed = 0;

    mintPool.Reset();

    return true;
}

void CZerocoinWallet::Lock()
{
    seedMaster.SetNull();
}

void CZerocoinWallet::AddToMintPool(const std::pair<uint256, uint32_t>& pMint, bool fVerbose)
{
    mintPool.Add(pMint, fVerbose);
}

// //Add the next 20 mints to the mint pool
void CZerocoinWallet::GenerateMintPool(uint32_t nCountStart, uint32_t nCountEnd)
{

    //Is locked
    if (seedMaster.IsNull())
        return;

    uint32_t n = nCountLastUsed + 1;

    if (nCountStart > 0)
        n = nCountStart;

    uint32_t nStop = n + 20;
    if (nCountEnd > 0)
        nStop = std::max(n, n + nCountEnd);

    bool fFound;

    uint256 hashSeed = Hash(seedMaster.begin(), seedMaster.end());
    LogPrintf("%s : n=%d nStop=%d\n", __func__, n, nStop - 1);
    for (uint32_t i = n; i < nStop; ++i) {
        if (ShutdownRequested())
            return;

        fFound = false;

        // Prevent unnecessary repeated minted
        for (auto& pair : mintPool) {
            if(pair.second == i) {
                fFound = true;
                break;
            }
        }

        if(fFound)
            continue;

        uint512 seedZerocoin = GetZerocoinSeed(i);
        CBigNum bnValue;
        CKey key;
        libzerocoin::PrivateCoin coin(ZCParamsV2);
        SeedToZerocoin(seedZerocoin, bnValue, coin);

        mintPool.Add(bnValue, i);
        CWalletDB(strWalletFile).WriteMintPoolPair(hashSeed, GetPubCoinHash(bnValue), i);
        LogPrintf("%s : %s count=%d\n", __func__, bnValue.GetHex().substr(0, 6), i);
    }
}

// pubcoin hashes are stored to db so that a full accounting of mints belonging to the seed can be tracked without regenerating
bool CZerocoinWallet::LoadMintPoolFromDB()
{
    map<uint256, vector<pair<uint256, uint32_t> > > mapMintPool = CWalletDB(strWalletFile).MapMintPool();

    uint256 hashSeed = Hash(seedMaster.begin(), seedMaster.end());
    for (auto& pair : mapMintPool[hashSeed])
        mintPool.Add(pair);

    return true;
}

void CZerocoinWallet::RemoveMintsFromPool(const std::vector<uint256>& vPubcoinHashes)
{
    for (const uint256& hash : vPubcoinHashes)
        mintPool.Remove(hash);
}

void CZerocoinWallet::GetState(int& nCount, int& nLastGenerated)
{
    nCount = this->nCountLastUsed + 1;
    nLastGenerated = mintPool.CountOfLastGenerated();
}

//Catch the counter up with the chain
void CZerocoinWallet::SyncWithChain(bool fGenerateMintPool)
{
    uint32_t nLastCountUsed = 0;
    bool found = true;
    CWalletDB walletdb(strWalletFile);

    set<uint256> setAddedTx;
    while (found) {
        found = false;
        if (fGenerateMintPool)
            GenerateMintPool();
        LogPrintf("%s: Mintpool size=%d\n", __func__, mintPool.size());

        std::set<uint256> setChecked;
        list<pair<uint256,uint32_t> > listMints = mintPool.List();
        for (pair<uint256, uint32_t> pMint : listMints) {
            LOCK(cs_main);
            if (setChecked.count(pMint.first))
                return;
            setChecked.insert(pMint.first);

            if (ShutdownRequested())
                return;

            if (pwalletMain->zerocoinTracker->HasPubcoinHash(pMint.first)) {
                mintPool.Remove(pMint.first);
                continue;
            }

            uint256 txHash;
            if (ZerocoinGetMintTxHash(txHash, pMint.first)) {
                //this mint has already occurred on the chain, increment counter's state to reflect this
                LogPrintf("%s : Found wallet coin mint=%s count=%d tx=%s\n", __func__, pMint.first.GetHex(), pMint.second, txHash.GetHex());
                found = true;

                uint256 hashBlock;
                CTransaction tx;
                if (!GetTransaction(txHash, tx, Params().GetConsensus(), hashBlock, true)) {
                    LogPrintf("%s : failed to get transaction for mint %s!\n", __func__, pMint.first.GetHex());
                    found = false;
                    nLastCountUsed = std::max(pMint.second, nLastCountUsed);
                    continue;
                }

                //Find the denomination
                libzerocoin::CoinDenomination denomination = libzerocoin::CoinDenomination::ZQ_ERROR;
                bool fFoundMint = false;
                CBigNum bnValue = 0;
                for (const CTxOut& out : tx.vout) {
                    if (!out.scriptPubKey.IsZerocoinMint())
                        continue;

                    libzerocoin::PublicCoin pubcoin(ZCParamsV2);
                    CValidationState state;
                    if (!TxOutToPublicCoin(out, pubcoin, state)) {
                        LogPrintf("%s : failed to get mint from txout for %s!\n", __func__, pMint.first.GetHex());
                        continue;
                    }

                    // See if this is the mint that we are looking for
                    uint256 hashPubcoin = GetPubCoinHash(pubcoin.getValue());
                    if (pMint.first == hashPubcoin) {
                        denomination = pubcoin.getDenomination();
                        bnValue = pubcoin.getValue();
                        fFoundMint = true;
                        break;
                    }
                }

                if (!fFoundMint || denomination == libzerocoin::CoinDenomination::ZQ_ERROR) {
                    LogPrintf("%s : failed to get mint %s from tx %s!\n", __func__, pMint.first.GetHex(), tx.GetHash().GetHex());
                    found = false;
                    break;
                }

                CBlockIndex* pindex = nullptr;
                if (mapBlockIndex.count(hashBlock))
                    pindex = mapBlockIndex.at(hashBlock);

                if (!setAddedTx.count(txHash)) {
                    CBlock block;
                    CWalletTx wtx(pwalletMain, tx);
                    if (pindex && ReadBlockFromDisk(block, pindex, Params().GetConsensus()))
                        wtx.SetMerkleBranch(block);

                    //Fill out wtx so that a transaction record can be created
                    wtx.nTimeReceived = pindex->GetBlockTime();
                    pwalletMain->AddToWallet(wtx, false, &walletdb);
                    setAddedTx.insert(txHash);
                }

                SetMintSeen(bnValue, pindex->nHeight, txHash, denomination);
                nLastCountUsed = std::max(pMint.second, nLastCountUsed);
                nCountLastUsed = std::max(nLastCountUsed, nCountLastUsed);
                LogPrint("zero", "%s: updated count to %d\n", __func__, nCountLastUsed);
            }
        }
    }
}

bool CZerocoinWallet::SetMintSeen(const CBigNum& bnValue, const int& nHeight, const uint256& txid, const libzerocoin::CoinDenomination& denom)
{
    if (!mintPool.Has(bnValue))
        return error("%s: value not in pool", __func__);
    pair<uint256, uint32_t> pMint = mintPool.Get(bnValue);

    // Regenerate the mint
    uint512 seedZerocoin = GetZerocoinSeed(pMint.second);
    CBigNum bnValueGen;
    libzerocoin::PrivateCoin coin(ZCParamsV2, denom, false);
    SeedToZerocoin(seedZerocoin, bnValueGen, coin);
    CWalletDB walletdb(strWalletFile);

    //Sanity check
    if (bnValueGen != bnValue)
        return error("%s: generated pubcoin and expected value do not match!", __func__);

    // Create mint object and database it
    uint256 hashSeed = Hash(seedMaster.begin(), seedMaster.end());
    uint256 hashSerial = GetSerialHash(coin.getSerialNumber());
    CDeterministicMint dMint(pMint.second, hashSeed, hashSerial, bnValue);
    dMint.SetDenomination(denom);
    dMint.SetHeight(nHeight);

    // Check if this is also already spent
    int nHeightTx;
    uint256 txidSpend;
    CTransaction txSpend;
    if (IsSerialInBlockchain(hashSerial, nHeightTx, txidSpend, txSpend)) {
        //Find transaction details and make a wallettx and add to wallet
        dMint.SetUsed(true);
        CWalletTx wtx(pwalletMain, txSpend);
        CBlockIndex* pindex = chainActive[nHeightTx];
        CBlock block;
        if (ReadBlockFromDisk(block, pindex, Params().GetConsensus()))
            wtx.SetMerkleBranch(block);

        wtx.nTimeReceived = pindex->nTime;
        pwalletMain->AddToWallet(wtx, false, &walletdb);
    }

    // Add to zerocoinTracker which also adds to database
    pwalletMain->zerocoinTracker->Add(dMint, true);

    //Update the count if it is less than the mint's count
    if (nCountLastUsed < pMint.second) {
        nCountLastUsed = pMint.second;
        walletdb.WriteZerocoinCount(nCountLastUsed);
    }

    //remove from the pool
    mintPool.Remove(dMint.GetPubcoinHash());

    return true;
}

// Check if the value of the commitment meets requirements
bool IsValidCoinValue(const CBigNum& bnValue)
{
    return bnValue >= ZCParamsV2->accumulatorParams.minCoinValue &&
    bnValue <= ZCParamsV2->accumulatorParams.maxCoinValue &&
    bnValue.isPrime();
}

void CZerocoinWallet::SeedToZerocoin(const uint512& seedZerocoin, CBigNum& bnValue, libzerocoin::PrivateCoin& coin)
{
    CBigNum bnRandomness;

    //convert state seed into a seed for the private key
    uint256 nSeedPrivKey = seedZerocoin.trim256();
    nSeedPrivKey = Hash(nSeedPrivKey.begin(), nSeedPrivKey.end());
    coin.setEcdsaSeckey(nSeedPrivKey);

    // Create a key pair
    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_create(libzerocoin::ctx, &pubkey, coin.getEcdsaSeckey())){
        throw ZerocoinException("Unable to create public key.");
    }

    // Hash the public key in the group to obtain a serial number
    Bignum serialNumber = coin.serialNumberFromSerializedPublicKey(libzerocoin::ctx, &pubkey); 
    coin.setSerialNumber(serialNumber);

    //hash randomness seed with Bottom 256 bits of seedZerocoin & attempts256 which is initially 0
    uint256 randomnessSeed = ArithToUint512(UintToArith512(seedZerocoin) >> 256).trim256();
    uint256 hashRandomness = Hash(randomnessSeed.begin(), randomnessSeed.end());
    bnRandomness.setuint256(UintToArith256(hashRandomness));
    bnRandomness = bnRandomness % ZCParamsV2->coinCommitmentGroup.groupOrder;

    //See if serial and randomness make a valid commitment
    // Generate a Pedersen commitment to the serial number
    CBigNum commitmentValue = ZCParamsV2->coinCommitmentGroup.g.pow_mod(coin.getSerialNumber(), ZCParamsV2->coinCommitmentGroup.modulus).mul_mod(
                        ZCParamsV2->coinCommitmentGroup.h.pow_mod(bnRandomness, ZCParamsV2->coinCommitmentGroup.modulus),
                        ZCParamsV2->coinCommitmentGroup.modulus);

    CBigNum random;
    arith_uint256 attempts256Arith = 0;
    // Iterate on Randomness until a valid commitmentValue is found
    while (true) {
        // Now verify that the commitment is a prime number
        // in the appropriate range. If not, we'll throw this coin
        // away and generate a new one.
        if (IsValidCoinValue(commitmentValue)) {
            coin.setRandomness(bnRandomness);
            bnValue = commitmentValue;

            return;
        }

        //Did not create a valid commitment value.
        //Change randomness to something new and (deterministically) random and try again
        attempts256Arith++;
        uint256 attempts256 = ArithToUint256(attempts256Arith);
        hashRandomness = Hash(randomnessSeed.begin(), randomnessSeed.end(),
                              attempts256.begin(), attempts256.end());
        random.setuint256(UintToArith256(hashRandomness));
        bnRandomness = (bnRandomness + random) % ZCParamsV2->coinCommitmentGroup.groupOrder;
        commitmentValue = commitmentValue.mul_mod(ZCParamsV2->coinCommitmentGroup.h.pow_mod(random, ZCParamsV2->coinCommitmentGroup.modulus), ZCParamsV2->coinCommitmentGroup.modulus);
    }
}

uint512 CZerocoinWallet::GetZerocoinSeed(uint32_t n)
{
    CDataStream ss(SER_GETHASH, 0);
    ss << seedMaster << n;
    uint512 zerocoinSeed = Hash512(ss.begin(), ss.end());
    return zerocoinSeed;
}

uint32_t CZerocoinWallet::GetCount()
{
    return nCountLastUsed;
}

void CZerocoinWallet::SetCount(uint32_t nCount)
{
    nCountLastUsed = nCount;
}

void CZerocoinWallet::UpdateCountLocal()
{
    nCountLastUsed++;
}

void CZerocoinWallet::UpdateCountDB()
{
    CWalletDB walletdb(strWalletFile);
    walletdb.WriteZerocoinCount(nCountLastUsed);
}

void CZerocoinWallet::UpdateCount()
{
    UpdateCountLocal();
    UpdateCountDB();
}

void CZerocoinWallet::GenerateDeterministicZerocoin(libzerocoin::CoinDenomination denom, libzerocoin::PrivateCoin& coin, CDeterministicMint& dMint, bool fGenerateOnly)
{
    GenerateMint(nCountLastUsed + 1, denom, coin, dMint);
    if (fGenerateOnly)
        return;

    //TODO remove this leak of seed from logs before merge to master
    //LogPrintf("%s : Generated new deterministic mint. Count=%d pubcoin=%s seed=%s\n", __func__, nCount, coin.getPublicCoin().getValue().GetHex().substr(0,6), seedZerocoin.GetHex().substr(0, 4));
}

void CZerocoinWallet::GenerateMint(const uint32_t& nCount, const libzerocoin::CoinDenomination denom, libzerocoin::PrivateCoin& coin, CDeterministicMint& dMint)
{
    uint512 seedZerocoin = GetZerocoinSeed(nCount);
    CBigNum commitmentValue;
    SeedToZerocoin(seedZerocoin, commitmentValue, coin);

    coin.setPublicCoin(libzerocoin::PublicCoin(ZCParamsV2, commitmentValue, denom));

    uint256 hashSeed = Hash(seedMaster.begin(), seedMaster.end());
    uint256 hashSerial = GetSerialHash(coin.getSerialNumber());
    dMint = CDeterministicMint(nCount, hashSeed, hashSerial, coin.getPublicCoin().getValue());
    dMint.SetDenomination(denom);
}

bool CZerocoinWallet::CheckSeed(const CDeterministicMint& dMint)
{
    //Check that the seed is correct    todo:handling of incorrect, or multiple seeds
    uint256 hashSeed = Hash(seedMaster.begin(), seedMaster.end());
    return hashSeed == dMint.GetSeedHash();
}

bool CZerocoinWallet::RegenerateMint(const CDeterministicMint& dMint, CZerocoinEntry& zerocoin)
{
    if (!CheckSeed(dMint)) {
        uint256 hashSeed = Hash(seedMaster.begin(), seedMaster.end());
        return error("%s: master seed does not match!\ndmint:\n %s \nhashSeed: %s\nseed: %s", __func__, dMint.ToString(), hashSeed.GetHex(), seedMaster.GetHex());
    }

    //Generate the coin
    libzerocoin::PrivateCoin coin(ZCParamsV2, dMint.GetDenomination(), false);
    CDeterministicMint dMintDummy;
    GenerateMint(dMint.GetCount(), dMint.GetDenomination(), coin, dMintDummy);

    //Fill in the zerocoinmint object's details
    CBigNum bnValue = coin.getPublicCoin().getValue();
    if (GetPubCoinHash(bnValue) != dMint.GetPubcoinHash())
        return error("%s: failed to correctly generate mint, pubcoin hash mismatch", __func__);
    zerocoin.value = bnValue;

    CBigNum bnSerial = coin.getSerialNumber();
    if (GetSerialHash(bnSerial) != dMint.GetSerialHash())
        return error("%s: failed to correctly generate mint, serial hash mismatch", __func__);

    zerocoin.denomination = dMint.GetDenomination();
    zerocoin.randomness = coin.getRandomness();
    zerocoin.serialNumber = bnSerial;
    zerocoin.IsUsed = dMint.IsUsed();
    zerocoin.nHeight = dMint.GetHeight();
    zerocoin.ecdsaSecretKey = std::vector<unsigned char>(&coin.getEcdsaSeckey()[0],&coin.getEcdsaSeckey()[32]);

    return true;
}
