// Copyright (c) 2016-2017 The B3Coin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "fundamentalnode.h"
#include "fn-manager.h"
#include "spork.h"
#include "core.h"
#include "util.h"
#include "sync.h"
#include "addrman.h"
#include <boost/lexical_cast.hpp>

CCriticalSection cs_fundamentalnodepayments;

/** Object for who's going to get paid on which blocks */
CFundamentalnodePayments fundamentalnodePayments;
// keep track of Fundamentalnode votes I've seen
map<uint256, CFundamentalnodePaymentWinner> mapSeenFundamentalnodeVotes;
// keep track of the scanning errors I've seen
map<uint256, int> mapSeenFundamentalnodeScanningErrors;
// cache block hashes as we calculate them
std::map<int64_t, uint256> mapCacheBlockHashes;

void ProcessMessageFundamentalnodePayments(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if(IsInitialBlockDownload()) return;

    if (strCommand == "fnget") { //Fundamentalnode Payments Request Sync
        if(fProMode) return; //disable all Fundamentalnode related functionality

        if(pfrom->HasFulfilledRequest("fnget")) {
            LogPrintf("fnget - peer already asked me for the list\n");
            pfrom->Misbehaving( 20);
            return;
        }

        pfrom->FulfilledRequest("fnget");
        fundamentalnodePayments.Sync(pfrom);
        LogPrintf("fnget - Sent Fundamentalnode winners to %s\n", pfrom->addr.ToString().c_str());
    }
    else if (strCommand == "fnw") { //Fundamentalnode Payments Declare Winner

        LOCK(cs_fundamentalnodepayments);

        //this is required in litemode
        CFundamentalnodePaymentWinner winner;
        vRecv >> winner;

        if(pindexBest == NULL) return;

        CTxDestination address1;
        ExtractDestination(winner.payee, address1);
        CBitcoinAddress address2(address1);

        uint256 hash = winner.GetHash();
        if(mapSeenFundamentalnodeVotes.count(hash)) {
            if(fDebug) LogPrintf("fnw - seen vote %s Addr %s Height %d bestHeight %d\n", hash.ToString().c_str(), address2.ToString().c_str(), winner.nBlockHeight, pindexBest->nHeight);
            return;
        }

        if(winner.nBlockHeight < pindexBest->nHeight - 10 || winner.nBlockHeight > pindexBest->nHeight+20){
            LogPrintf("fnw - winner out of range %s Addr %s Height %d bestHeight %d\n", winner.vin.ToString().c_str(), address2.ToString().c_str(), winner.nBlockHeight, pindexBest->nHeight);
            return;
        }

        if(winner.vin.nSequence != std::numeric_limits<unsigned int>::max()){
            LogPrintf("fnw - invalid nSequence\n");
            pfrom->Misbehaving( 100);
            return;
        }

        LogPrintf("fnw - winning vote - Vin %s Addr %s Height %d bestHeight %d\n", winner.vin.ToString().c_str(), address2.ToString().c_str(), winner.nBlockHeight, pindexBest->nHeight);

        if(!fundamentalnodePayments.CheckSignature(winner)){
            LogPrintf("fnw - invalid signature\n");
            pfrom->Misbehaving(100);
            return;
        }

        mapSeenFundamentalnodeVotes.insert(make_pair(hash, winner));

        if(fundamentalnodePayments.AddWinningFundamentalnode(winner)){
            fundamentalnodePayments.Relay(winner);
        }
    }
}

struct CompareValueOnly
{
    bool operator()(const pair<int64_t, CTxIn>& t1,
                    const pair<int64_t, CTxIn>& t2) const
    {
        return t1.first < t2.first;
    }
};

//Get the last hash that matches the modulus given. Processed in reverse order
bool GetBlockHash(uint256& hash, int nBlockHeight)
{
    if (pindexBest == NULL) return false;

    if(nBlockHeight == 0)
        nBlockHeight = pindexBest->nHeight;

    if(mapCacheBlockHashes.count(nBlockHeight)){
        hash = mapCacheBlockHashes[nBlockHeight];
        return true;
    }

    const CBlockIndex *BlockLastSolved = pindexBest;
    const CBlockIndex *BlockReading = pindexBest;

    if (BlockLastSolved == NULL || BlockLastSolved->nHeight == 0 || pindexBest->nHeight+1 < nBlockHeight) return false;

    int nBlocksAgo = 0;
    if(nBlockHeight > 0) nBlocksAgo = (pindexBest->nHeight+1)-nBlockHeight;
    assert(nBlocksAgo >= 0);

    int n = 0;
    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
        if(n >= nBlocksAgo){
            hash = BlockReading->GetBlockHash();
            mapCacheBlockHashes[nBlockHeight] = hash;
            return true;
        }
        n++;

        if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
        BlockReading = BlockReading->pprev;
    }

    return false;
}

CFundamentalnode::CFundamentalnode()
{
    LOCK(cs);
    vin = CTxIn();
    addr = CService();
    pubkey = CPubKey();
    pubkey2 = CPubKey();
    sig = std::vector<unsigned char>();
    activeState = FUNDAMENTALNODE_ENABLED;
    sigTime = GetAdjustedTime();
    lastFnep = 0;
    lastTimeSeen = 0;
    cacheInputAge = 0;
    cacheInputAgeBlock = 0;
    unitTest = false;
    allowFreeTx = true;
    protocolVersion = MIN_PEER_PROTO_VERSION;
    nLastFnq = 0;
    donationAddress = CScript();
    donationPercentage = 0;
    nVote = 0;
    lastVote = 0;
    nScanningErrorCount = 0;
    nLastScanningErrorBlockHeight = 0;
}

CFundamentalnode::CFundamentalnode(const CFundamentalnode& other)
{
    LOCK(cs);
    vin = other.vin;
    addr = other.addr;
    pubkey = other.pubkey;
    pubkey2 = other.pubkey2;
    sig = other.sig;
    activeState = other.activeState;
    sigTime = other.sigTime;
    lastFnep = other.lastFnep;
    lastTimeSeen = other.lastTimeSeen;
    cacheInputAge = other.cacheInputAge;
    cacheInputAgeBlock = other.cacheInputAgeBlock;
    unitTest = other.unitTest;
    allowFreeTx = other.allowFreeTx;
    protocolVersion = other.protocolVersion;
    nLastFnq = other.nLastFnq;
    donationAddress = other.donationAddress;
    donationPercentage = other.donationPercentage;
    nVote = other.nVote;
    lastVote = other.lastVote;
    nScanningErrorCount = other.nScanningErrorCount;
    nLastScanningErrorBlockHeight = other.nLastScanningErrorBlockHeight;
}

CFundamentalnode::CFundamentalnode(CService newAddr, CTxIn newVin, CPubKey newPubkey, std::vector<unsigned char> newSig, int64_t newSigTime, CPubKey newPubkey2, int protocolVersionIn, CScript newDonationAddress, int newDonationPercentage)
{
    LOCK(cs);
    vin = newVin;
    addr = newAddr;
    pubkey = newPubkey;
    pubkey2 = newPubkey2;
    sig = newSig;
    activeState = FUNDAMENTALNODE_ENABLED;
    sigTime = newSigTime;
    lastFnep = 0;
    lastTimeSeen = 0;
    cacheInputAge = 0;
    cacheInputAgeBlock = 0;
    unitTest = false;
    allowFreeTx = true;
    protocolVersion = protocolVersionIn;
    nLastFnq = 0;
    donationAddress = newDonationAddress;
    donationPercentage = newDonationPercentage;
    nVote = 0;
    lastVote = 0;
    nScanningErrorCount = 0;
    nLastScanningErrorBlockHeight = 0;
}

//
// Deterministically calculate a given "score" for a Fundamentalnode depending on how close it's hash is to
// the proof of work for that block. The further away they are the better, the furthest will win the election
// and get paid this block
//
uint256 CFundamentalnode::CalculateScore(int mod, int64_t nBlockHeight)
{
    if(pindexBest == NULL) return 0;

    uint256 hash = 0;
    uint256 aux = vin.prevout.hash + vin.prevout.n;

    if(!GetBlockHash(hash, nBlockHeight)) return 0;

    uint256 hash2 = Hash(BEGIN(hash), END(hash));
    uint256 hash3 = Hash(BEGIN(hash), END(hash), BEGIN(aux), END(aux));

    uint256 r = (hash3 > hash2 ? hash3 - hash2 : hash2 - hash3);

    return r;
}

void CFundamentalnode::Check()
{
    //TODO: Random segfault with this line removed
    TRY_LOCK(cs_main, lockRecv);
    if(!lockRecv) return;

    if(nScanningErrorCount >= FUNDAMENTALNODE_SCANNING_ERROR_THESHOLD)
    {
        activeState = FUNDAMENTALNODE_POS_ERROR; // BBBBB
        return;
    }

    //once spent, stop doing the checks
    if(activeState == FUNDAMENTALNODE_VIN_SPENT) return;


    if(!UpdatedWithin(FUNDAMENTALNODE_REMOVAL_SECONDS)){
        activeState = FUNDAMENTALNODE_REMOVE;
        return;
    }

    if(!UpdatedWithin(FUNDAMENTALNODE_EXPIRATION_SECONDS)){
        activeState = FUNDAMENTALNODE_EXPIRED;
        return;
    }

    if(GetFundamentalnodeInputAge() > BLOCK_AGE_THRESHOLD){
        if(pindexBest->nHeight > FN_AGE_ENFORCE_HEIGHT){
            activeState = FUNDAMENTALNODE_EXPIRED;
            return;
        }
    }

    activeState = FUNDAMENTALNODE_ENABLED; // OK
}

bool CFundamentalnodePayments::CheckSignature(CFundamentalnodePaymentWinner& winner)
{
    //note: need to investigate why this is failing
    std::string strMessage = winner.vin.ToString().c_str() + boost::lexical_cast<std::string>(winner.nBlockHeight) + winner.payee.ToString();
    std::string strPubKey = (Params().NetworkID() == CChainParams::MAIN) ? strMainPubKey : strTestPubKey;
    CPubKey pubkey(ParseHex(strPubKey));

    std::string errorMessage = "";
    if(!fnSigner.VerifyMessage(pubkey, winner.vchSig, strMessage, errorMessage)){
        return false;
    }

    return true;
}

bool CFundamentalnodePayments::Sign(CFundamentalnodePaymentWinner& winner)
{
    std::string strMessage = winner.vin.ToString().c_str() + boost::lexical_cast<std::string>(winner.nBlockHeight) + winner.payee.ToString();

    CKey key2;
    CPubKey pubkey2;
    std::string errorMessage = "";

    if(!fnSigner.SetKey(strMasterPrivKey, errorMessage, key2, pubkey2))
    {
        LogPrintf("CFundamentalnodePayments::Sign - ERROR: Invalid Fundamentalnodeprivkey: '%s'\n", errorMessage.c_str());
        return false;
    }

    if(!fnSigner.SignMessage(strMessage, errorMessage, winner.vchSig, key2)) {
        LogPrintf("CFundamentalnodePayments::Sign - Sign message failed");
        return false;
    }

    if(!fnSigner.VerifyMessage(pubkey2, winner.vchSig, strMessage, errorMessage)) {
        LogPrintf("CFundamentalnodePayments::Sign - Verify message failed");
        return false;
    }

    return true;
}


uint64_t CFundamentalnodePayments::CalculateScore(uint256 blockHash, CTxIn& vin)
{
    //BitSendDev & Joshafest 26-06-2016
    uint256 n1 = blockHash; 
	uint256 n2, n3, n4; 
    
	{
     n2 = Hash(BEGIN(n1), END(n1));
     n3 = Hash(BEGIN(vin.prevout.hash), END(vin.prevout.hash));
	 n4 = n3 > n2 ? (n3 - n2) : (n2 - n3);
     LogPrintf(" -- CFundamentalnodePayments CalculateScore() n2 = %d \n", n2.Get64());
     LogPrintf(" -- CFundamentalnodePayments CalculateScore() n3 = %d \n", n3.Get64());
     LogPrintf(" -- CFundamentalnodePayments CalculateScore() n4 = %d \n", n4.Get64());
	 return n4.Get64();
	}

    //printf(" -- CFundamentalnodePayments CalculateScore() n2 = %d \n", n2.Get64());
    //printf(" -- CFundamentalnodePayments CalculateScore() n3 = %d \n", n3.Get64());
    //printf(" -- CFundamentalnodePayments CalculateScore() n4 = %d \n", n4.Get64());
 
}

bool CFundamentalnodePayments::GetBlockPayee(int nBlockHeight, CScript& payee)
{
    BOOST_FOREACH(CFundamentalnodePaymentWinner& winner, vWinning){
        if(winner.nBlockHeight == nBlockHeight) {
            payee = winner.payee;
            return true;
        }
    }

    return false;
}

bool CFundamentalnodePayments::GetWinningFundamentalnode(int nBlockHeight, CTxIn& vinOut)
{
    BOOST_FOREACH(CFundamentalnodePaymentWinner& winner, vWinning){
        if(winner.nBlockHeight == nBlockHeight) {
            vinOut = winner.vin;
            return true;
        }
    }

    return false;
}

bool CFundamentalnodePayments::AddWinningFundamentalnode(CFundamentalnodePaymentWinner& winnerIn)
{
    uint256 blockHash = 0;
    if(!GetBlockHash(blockHash, winnerIn.nBlockHeight-576)) {
        return false;
    }

    winnerIn.score = CalculateScore(blockHash, winnerIn.vin);

    bool foundBlock = false;
    BOOST_FOREACH(CFundamentalnodePaymentWinner& winner, vWinning){
        if(winner.nBlockHeight == winnerIn.nBlockHeight) {
            foundBlock = true;
            if(winner.score < winnerIn.score){
                winner.score = winnerIn.score;
                winner.vin = winnerIn.vin;
                winner.payee = winnerIn.payee;
                winner.vchSig = winnerIn.vchSig;

                mapSeenFundamentalnodeVotes.insert(make_pair(winnerIn.GetHash(), winnerIn));

                return true;
            }
        }
    }

    // if it's not in the vector
    if(!foundBlock){
        vWinning.push_back(winnerIn);
        mapSeenFundamentalnodeVotes.insert(make_pair(winnerIn.GetHash(), winnerIn));

        return true;
    }

    return false;
}

void CFundamentalnodePayments::CleanPaymentList()
{
    LOCK(cs_fundamentalnodepayments);

    if(pindexBest == NULL) return;

    int nLimit = std::max(((int)fnmanager.size())*2, 1000);

    vector<CFundamentalnodePaymentWinner>::iterator it;
    for(it=vWinning.begin();it<vWinning.end();it++){
        if(pindexBest->nHeight - (*it).nBlockHeight > nLimit){
            if(fDebug) LogPrintf("CFundamentalnodePayments::CleanPaymentList - Removing old Fundamentalnode payment - block %d\n", (*it).nBlockHeight);
            vWinning.erase(it);
            break;
        }
    }
}

bool CFundamentalnodePayments::ProcessBlock(int nBlockHeight)
{
    LOCK(cs_fundamentalnodepayments);

    if(nBlockHeight <= nLastBlockHeight) return false;
    if(!enabled) return false;
    CFundamentalnodePaymentWinner newWinner;
    int nMinimumAge = fnmanager.CountEnabled();
    CScript payeeSource;

    uint256 hash;
    if(!GetBlockHash(hash, nBlockHeight-10)) return false;
    unsigned int nHash;
    memcpy(&nHash, &hash, 2);

    LogPrintf(" ProcessBlock Start nHeight %d. \n", nBlockHeight);

    std::vector<CTxIn> vecLastPayments;
    BOOST_REVERSE_FOREACH(CFundamentalnodePaymentWinner& winner, vWinning)
    {
        //if we already have the same vin - we have one full payment cycle, break
        if(vecLastPayments.size() > nMinimumAge) break;
        vecLastPayments.push_back(winner.vin);
    }

    // pay to the oldest fn that still had no payment but its input is old enough and it was active long enough
    CFundamentalnode *pfn = fnmanager.FindOldestNotInVec(vecLastPayments, nMinimumAge, 0);
    if(pfn != NULL)
    {
        LogPrintf(" Found by FindOldestNotInVec \n");
        
        newWinner.score = 0;
        newWinner.nBlockHeight = nBlockHeight;
        newWinner.vin = pfn->vin;

        if(pfn->donationPercentage > 0 && (nHash % 100) <= (unsigned int)pfn->donationPercentage) {
            newWinner.payee = pfn->donationAddress;
        } else {
            newWinner.payee.SetDestination(pfn->pubkey.GetID());
        }
        
        payeeSource.SetDestination(pfn->pubkey.GetID());
    }

    //if we can't find new fn to get paid, pick first active fn counting back from the end of vecLastPayments list
    if(newWinner.nBlockHeight == 0 && nMinimumAge > 0)
    {
        LogPrintf(" Find by reverse \n");
        BOOST_REVERSE_FOREACH(CTxIn& vinLP, vecLastPayments)
        {
            CFundamentalnode* pfn = fnmanager.Find(vinLP);
            if(pfn != NULL)
            {
                pfn->Check();
                if(!pfn->IsEnabled()) continue;

                newWinner.score = 0;
                newWinner.nBlockHeight = nBlockHeight;
                newWinner.vin = pfn->vin;

                if(pfn->donationPercentage > 0 && (nHash % 100) <= (unsigned int)pfn->donationPercentage) {
                    newWinner.payee = pfn->donationAddress;
                } else {
                    newWinner.payee.SetDestination(pfn->pubkey.GetID());
                }
                payeeSource.SetDestination(pfn->pubkey.GetID());
                
                break; // we found active fn
            }
        }
    }

    if(newWinner.nBlockHeight == 0) return false;

    CTxDestination address1;
    ExtractDestination(newWinner.payee, address1);
    CBitcoinAddress address2(address1);

    CTxDestination address3;

    ExtractDestination(payeeSource, address3);
    CBitcoinAddress address4(address3);
    LogPrintf("Winner payee %s nHeight %d vin source %s. \n", address2.ToString().c_str(), newWinner.nBlockHeight, address4.ToString().c_str());

    if(Sign(newWinner))
    {
        if(AddWinningFundamentalnode(newWinner))
        {
            Relay(newWinner);
            nLastBlockHeight = nBlockHeight;
            return true;
        }
    }

    return false;
}


void CFundamentalnodePayments::Relay(CFundamentalnodePaymentWinner& winner)
{
        CInv inv(MSG_FUNDAMENTALNODE_WINNER, winner.GetHash());

    vector<CInv> vInv;
    vInv.push_back(inv);
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes){
        pnode->PushMessage("inv", vInv);
    }
}

void CFundamentalnodePayments::Sync(CNode* node)
{
    LOCK(cs_fundamentalnodepayments);

    BOOST_FOREACH(CFundamentalnodePaymentWinner& winner, vWinning)
        if(winner.nBlockHeight >= pindexBest->nHeight-10 && winner.nBlockHeight <= pindexBest->nHeight + 20)
            node->PushMessage("fnw", winner);
}


bool CFundamentalnodePayments::SetPrivKey(std::string strPrivKey)
{
    CFundamentalnodePaymentWinner winner;

    // Test signing successful, proceed
    strMasterPrivKey = strPrivKey;

    Sign(winner);

    if(CheckSignature(winner)){
        LogPrintf("CFundamentalnodePayments::SetPrivKey - Successfully initialized as Fundamentalnode payments master\n");
        enabled = true;
        return true;
    } else {
        return false;
    }
}
