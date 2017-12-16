// Copyright (c) 2016-2017 The B3Coin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "fn-manager.h"
#include "fundamentalnode.h"
#include "fn-activity.h"
//#include "signhelper_fn.h"
#include "core.h"
#include "main.h"
#include "util.h"
#include "addrman.h"
#include <boost/lexical_cast.hpp>
#include <boost/filesystem.hpp>

CCriticalSection cs_process_message;

/** Fundamentalnode manager */
CFundamentalnodeMan fnmanager;

CFNSignHelper fnSigner;

struct CompareValueOnly
{
    bool operator()(const pair<int64_t, CTxIn>& t1,
                    const pair<int64_t, CTxIn>& t2) const
    {
        return t1.first < t2.first;
    }
};
struct CompareValueOnlyFN
{
    bool operator()(const pair<int64_t, CFundamentalnode>& t1,
                    const pair<int64_t, CFundamentalnode>& t2) const
    {
        return t1.first < t2.first;
    }
};

//
// CFundamentalnodeDB
//

CFundamentalnodeDB::CFundamentalnodeDB()
{
    pathFN = GetDataDir() / "fncache.dat";
    strMagicMessage = "FundamentalnodeCache";
}

bool CFundamentalnodeDB::Write(const CFundamentalnodeMan& fnmanagerToSave)
{
    int64_t nStart = GetTimeMillis();

    // serialize, checksum data up to that point, then append checksum
    CDataStream ssFundamentalnodes(SER_DISK, CLIENT_VERSION);
    ssFundamentalnodes << strMagicMessage; // fundamentalnode cache file specific magic message
    ssFundamentalnodes << FLATDATA(Params().MessageStart()); // network specific magic number
    ssFundamentalnodes << fnmanagerToSave;
    uint256 hash = Hash(ssFundamentalnodes.begin(), ssFundamentalnodes.end());
    ssFundamentalnodes << hash;

    // open output file, and associate with CAutoFile
    FILE *file = fopen(pathFN.string().c_str(), "wb");
    CAutoFile fileout = CAutoFile(file, SER_DISK, CLIENT_VERSION);
    if (!fileout)
        return error("%s : Failed to open file %s", __func__, pathFN.string());

    // Write and commit header, data
    try {
        fileout << ssFundamentalnodes;
    }
    catch (std::exception &e) {
        return error("%s : Serialize or I/O error - %s", __func__, e.what());
    }
    FileCommit(fileout);
    fileout.fclose();

    LogPrintf("Written info to fncache.dat  %dms\n", GetTimeMillis() - nStart);
    LogPrintf("  %s\n", fnmanagerToSave.ToString());

    return true;
}

CFundamentalnodeDB::ReadResult CFundamentalnodeDB::Read(CFundamentalnodeMan& fnmanagerToLoad)
{
    int64_t nStart = GetTimeMillis();
    // open input file, and associate with CAutoFile
    FILE *file = fopen(pathFN.string().c_str(), "rb");
    CAutoFile filein = CAutoFile(file, SER_DISK, CLIENT_VERSION);
    if (!filein)
    {
        error("%s : Failed to open file %s", __func__, pathFN.string());
        return FileError;
    }

    // use file size to size memory buffer
    int fileSize = boost::filesystem::file_size(pathFN);
    int dataSize = fileSize - sizeof(uint256);
    // Don't try to resize to a negative number if file is small
    if (dataSize < 0)
        dataSize = 0;
    vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // read data and checksum from file
    try {
        filein.read((char *)&vchData[0], dataSize);
        filein >> hashIn;
    }
    catch (std::exception &e) {
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return HashReadError;
    }
    filein.fclose();

    CDataStream ssFundamentalnodes(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssFundamentalnodes.begin(), ssFundamentalnodes.end());
    if (hashIn != hashTmp)
    {
        error("%s : Checksum mismatch, data corrupted", __func__);
        return IncorrectHash;
    }

    unsigned char pchMsgTmp[4];
    std::string strMagicMessageTmp;
    try {
        // de-serialize file header (fundamentalnode cache file specific magic message) and ..

        ssFundamentalnodes >> strMagicMessageTmp;

        // ... verify the message matches predefined one
        if (strMagicMessage != strMagicMessageTmp)
        {
            error("%s : Invalid fundamentalnode cache magic message", __func__);
            return IncorrectMagicMessage;
        }

        // de-serialize file header (network specific magic number) and ..
        ssFundamentalnodes >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp)))
        {
            error("%s : Invalid network magic number", __func__);
            return IncorrectMagicNumber;
        }
        // de-serialize data into CFundamentalnodeMan object
        ssFundamentalnodes >> fnmanagerToLoad;
    }
    catch (std::exception &e) {
        fnmanagerToLoad.Clear();
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return IncorrectFormat;
    }

    fnmanagerToLoad.CheckAndRemove(); // clean out expired
    LogPrintf("Loaded info from fncache.dat  %dms\n", GetTimeMillis() - nStart);
    LogPrintf("  %s\n", fnmanagerToLoad.ToString());

    return Ok;
}

void DumpFundamentalnodes()
{
    int64_t nStart = GetTimeMillis();

    CFundamentalnodeDB fndb;
    CFundamentalnodeMan tempfnmanager;

    LogPrintf("Verifying fncache.dat format...\n");
    CFundamentalnodeDB::ReadResult readResult = fndb.Read(tempfnmanager);
    // there was an error and it was not an error on file openning => do not proceed
    if (readResult == CFundamentalnodeDB::FileError)
        LogPrintf("Missing fundamentalnode cache file - fncache.dat, will try to recreate\n");
    else if (readResult != CFundamentalnodeDB::Ok)
    {
        LogPrintf("Error reading fncache.dat: ");
        if(readResult == CFundamentalnodeDB::IncorrectFormat)
            LogPrintf("magic is ok but data has invalid format, will try to recreate\n");
        else
        {
            LogPrintf("file format is unknown or invalid, please fix it manually\n");
            return;
        }
    }
    LogPrintf("Writting info to fncache.dat...\n");
    fndb.Write(fnmanager);

    LogPrintf("Fundamentalnode dump finished  %dms\n", GetTimeMillis() - nStart);
}

CFundamentalnodeMan::CFundamentalnodeMan() {
    nDsqCount = 0;
}

bool CFundamentalnodeMan::Add(CFundamentalnode &fn)
{
    LOCK(cs);

    if (!fn.IsEnabled())
        return false;

    CFundamentalnode *pfn = Find(fn.vin);

    if (pfn == NULL)
    {
        if(fDebug) LogPrintf("CFundamentalnodeMan: Adding new Fundamentalnode %s - %i now\n", fn.addr.ToString().c_str(), size() + 1);
        vFundamentalnodes.push_back(fn);
        return true;
    }

    return false;
}

void CFundamentalnodeMan::Check()
{
    LOCK(cs);

    BOOST_FOREACH(CFundamentalnode& fn, vFundamentalnodes)
        fn.Check();
}

void CFundamentalnodeMan::CheckAndRemove()
{
    LOCK(cs);

    Check();

    //remove inactive
    vector<CFundamentalnode>::iterator it = vFundamentalnodes.begin();
    while(it != vFundamentalnodes.end()){
        if((*it).activeState == CFundamentalnode::FUNDAMENTALNODE_REMOVE || (*it).activeState == CFundamentalnode::FUNDAMENTALNODE_VIN_SPENT){
            if(fDebug) LogPrintf("CFundamentalnodeMan: Removing inactive Fundamentalnode %s - %i now\n", (*it).addr.ToString().c_str(), size() - 1);
            it = vFundamentalnodes.erase(it);
        } else {
            ++it;
        }
    }

    // check who's asked for the Fundamentalnode list
    map<CNetAddr, int64_t>::iterator it1 = mAskedUsForFundamentalnodeList.begin();
    while(it1 != mAskedUsForFundamentalnodeList.end()){
        if((*it1).second < GetTime()) {
            mAskedUsForFundamentalnodeList.erase(it1++);
        } else {
            ++it1;
        }
    }

    // check who we asked for the Fundamentalnode list
    it1 = mWeAskedForFundamentalnodeList.begin();
    while(it1 != mWeAskedForFundamentalnodeList.end()){
        if((*it1).second < GetTime()){
            mWeAskedForFundamentalnodeList.erase(it1++);
        } else {
            ++it1;
        }
    }

    // check which Fundamentalnodes we've asked for
    map<COutPoint, int64_t>::iterator it2 = mWeAskedForFundamentalnodeListEntry.begin();
    while(it2 != mWeAskedForFundamentalnodeListEntry.end()){
        if((*it2).second < GetTime()){
            mWeAskedForFundamentalnodeListEntry.erase(it2++);
        } else {
            ++it2;
        }
    }

}

void CFundamentalnodeMan::Clear()
{
    LOCK(cs);
    vFundamentalnodes.clear();
    mAskedUsForFundamentalnodeList.clear();
    mWeAskedForFundamentalnodeList.clear();
    mWeAskedForFundamentalnodeListEntry.clear();
    nDsqCount = 0;
}

int CFundamentalnodeMan::CountEnabled()
{
    int i = 0;

    BOOST_FOREACH(CFundamentalnode& fn, vFundamentalnodes) {
        fn.Check();
        if(fn.IsEnabled()) i++;
    }

    return i;
}

int CFundamentalnodeMan::CountFundamentalnodesAboveProtocol(int protocolVersion)
{
    int i = 0;

    BOOST_FOREACH(CFundamentalnode& fn, vFundamentalnodes) {
        fn.Check();
        if(fn.protocolVersion < protocolVersion || !fn.IsEnabled()) continue;
        i++;
    }

    return i;
}

void CFundamentalnodeMan::FnlUpdate(CNode* pnode)
{
    LOCK(cs);

    std::map<CNetAddr, int64_t>::iterator it = mWeAskedForFundamentalnodeList.find(pnode->addr);
    if (it != mWeAskedForFundamentalnodeList.end())
    {
        if (GetTime() < (*it).second) {
            LogPrintf("fnl - we already asked %s for the list; skipping...\n", pnode->addr.ToString());
            return;
        }
    }
    pnode->PushMessage("fnl", CTxIn());
    int64_t askAgain = GetTime() + FUNDAMENTALNODES_DSEG_SECONDS;
    mWeAskedForFundamentalnodeList[pnode->addr] = askAgain;
}

CFundamentalnode *CFundamentalnodeMan::Find(const CTxIn &vin)
{
    LOCK(cs);

    BOOST_FOREACH(CFundamentalnode& fn, vFundamentalnodes)
    {
        if(fn.vin.prevout == vin.prevout)
            return &fn;
    }
    return NULL;
}

CFundamentalnode *CFundamentalnodeMan::Find(const CPubKey &pubKeyFundamentalnode)
{
LOCK(cs);

BOOST_FOREACH(CFundamentalnode& fn, vFundamentalnodes)
{
if(fn.pubkey2 == pubKeyFundamentalnode)
return &fn;
}
return NULL;
}


CFundamentalnode* CFundamentalnodeMan::FindOldestNotInVec(const std::vector<CTxIn> &vVins, int nMinimumAge, int nMinimumActiveSeconds)
{
    LOCK(cs);

    CFundamentalnode *pOldestFundamentalnode = NULL;

    BOOST_FOREACH(CFundamentalnode &fn, vFundamentalnodes)
    {
        fn.Check();
        if(!fn.IsEnabled()) continue;

        /* if(!RegTest()) */{
            if(fn.GetFundamentalnodeInputAge() < nMinimumAge || fn.lastTimeSeen - fn.sigTime < nMinimumActiveSeconds) continue;
        }

        bool found = false;
        BOOST_FOREACH(const CTxIn& vin, vVins)
            if(fn.vin.prevout == vin.prevout)
            {
                found = true;
                break;
            }

        if(found) continue;

        if(pOldestFundamentalnode == NULL || pOldestFundamentalnode->GetFundamentalnodeInputAge() < fn.GetFundamentalnodeInputAge()){
            pOldestFundamentalnode = &fn;
        }
    }

    return pOldestFundamentalnode;
}

CFundamentalnode *CFundamentalnodeMan::FindRandom()
{
    LOCK(cs);

    if(size() == 0) return NULL;

    return &vFundamentalnodes[GetRandInt(vFundamentalnodes.size())];
}

CFundamentalnode* CFundamentalnodeMan::GetCurrentFundamentalNode(int mod, int64_t nBlockHeight, int minProtocol)
{
    unsigned int score = 0;
    CFundamentalnode* winner = NULL;

    // scan for winner
    BOOST_FOREACH(CFundamentalnode& fn, vFundamentalnodes) {
        fn.Check();
        if(fn.protocolVersion < minProtocol || !fn.IsEnabled()) continue;

        // calculate the score for each Fundamentalnode
        uint256 n = fn.CalculateScore(mod, nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        // determine the winner
        if(n2 > score){
            score = n2;
            winner = &fn;
        }
    }

    return winner;
}

int CFundamentalnodeMan::GetFundamentalnodeRank(const CTxIn& vin, int64_t nBlockHeight, int minProtocol, bool fOnlyActive)
{
    std::vector<pair<unsigned int, CTxIn> > vecFundamentalnodeScores;

    //make sure we know about this block
    uint256 hash = 0;
    if(!GetBlockHash(hash, nBlockHeight)) return -1;

    // scan for winner
    BOOST_FOREACH(CFundamentalnode& fn, vFundamentalnodes) {

        if(fn.protocolVersion < minProtocol) continue;
        if(fOnlyActive) {
            fn.Check();
            if(!fn.IsEnabled()) continue;
        }

        uint256 n = fn.CalculateScore(1, nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        vecFundamentalnodeScores.push_back(make_pair(n2, fn.vin));
    }

    sort(vecFundamentalnodeScores.rbegin(), vecFundamentalnodeScores.rend(), CompareValueOnly());

    int rank = 0;
    BOOST_FOREACH (PAIRTYPE(unsigned int, CTxIn)& s, vecFundamentalnodeScores){
        rank++;
        if(s.second == vin) {
            return rank;
        }
    }

    return -1;
}

std::vector<pair<int, CFundamentalnode> > CFundamentalnodeMan::GetFundamentalnodeRanks(int64_t nBlockHeight, int minProtocol)
{
    std::vector<pair<unsigned int, CFundamentalnode> > vecFundamentalnodeScores;
    std::vector<pair<int, CFundamentalnode> > vecFundamentalnodeRanks;

    //make sure we know about this block
    uint256 hash = 0;
    if(!GetBlockHash(hash, nBlockHeight)) return vecFundamentalnodeRanks;

    // scan for winner
    BOOST_FOREACH(CFundamentalnode& fn, vFundamentalnodes) {

        fn.Check();

        if(fn.protocolVersion < minProtocol) continue;
        if(!fn.IsEnabled()) {
            continue;
        }

        uint256 n = fn.CalculateScore(1, nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        vecFundamentalnodeScores.push_back(make_pair(n2, fn));
    }

    sort(vecFundamentalnodeScores.rbegin(), vecFundamentalnodeScores.rend(), CompareValueOnlyFN());

    int rank = 0;
    BOOST_FOREACH (PAIRTYPE(unsigned int, CFundamentalnode)& s, vecFundamentalnodeScores){
        rank++;
        vecFundamentalnodeRanks.push_back(make_pair(rank, s.second));
    }

    return vecFundamentalnodeRanks;
}

CFundamentalnode* CFundamentalnodeMan::GetFundamentalnodeByRank(int nRank, int64_t nBlockHeight, int minProtocol, bool fOnlyActive)
{
    std::vector<pair<unsigned int, CTxIn> > vecFundamentalnodeScores;

    // scan for winner
    BOOST_FOREACH(CFundamentalnode& fn, vFundamentalnodes) {

        if(fn.protocolVersion < minProtocol) continue;
        if(fOnlyActive) {
            fn.Check();
            if(!fn.IsEnabled()) continue;
        }

        uint256 n = fn.CalculateScore(1, nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        vecFundamentalnodeScores.push_back(make_pair(n2, fn.vin));
    }

    sort(vecFundamentalnodeScores.rbegin(), vecFundamentalnodeScores.rend(), CompareValueOnly());

    int rank = 0;
    BOOST_FOREACH (PAIRTYPE(unsigned int, CTxIn)& s, vecFundamentalnodeScores){
        rank++;
        if(rank == nRank) {
            return Find(s.second);
        }
    }

    return NULL;
}

void CFundamentalnodeMan::ProcessFundamentalnodeConnections()
{
    //Not Implemented

}

void CFundamentalnodeMan::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{

    if(fProMode) return; //disable Fnodes
    if(IsInitialBlockDownload()) return;

    LOCK(cs_process_message);

    if (strCommand == "fne") { //Fundamental Node Entry

        CTxIn vin;
        CService addr;
        CPubKey pubkey;
        CPubKey pubkey2;
        vector<unsigned char> vchSig;
        int64_t sigTime;
        int count;
        int current;
        int64_t lastUpdated;
        int protocolVersion;
        CScript donationAddress;
        int donationPercentage;
        std::string strMessage;

        //
        vRecv >> vin >> addr >> vchSig >> sigTime >> pubkey >> pubkey2 >> count >> current >> lastUpdated >> protocolVersion >> donationAddress >> donationPercentage;

        // make sure signature isn't in the future (past is OK)
        if (sigTime > GetAdjustedTime() + 60 * 60) {
            LogPrintf("fne - Signature rejected, too far into the future %s\n", vin.ToString().c_str());
            return;
        }

        bool isLocal = addr.IsRFC1918() || addr.IsLocal();
        //if(RegTest()) isLocal = false;

        std::string vchPubKey(pubkey.begin(), pubkey.end());
        std::string vchPubKey2(pubkey2.begin(), pubkey2.end());

        strMessage = addr.ToString() + boost::lexical_cast<std::string>(sigTime) + vchPubKey + vchPubKey2 + boost::lexical_cast<std::string>(protocolVersion)  + donationAddress.ToString() + boost::lexical_cast<std::string>(donationPercentage);

        if(donationPercentage < 0 || donationPercentage > 100){
            LogPrintf("fne - donation percentage out of range %d\n", donationPercentage);
            return;
        }

        if(protocolVersion < nFundamentalnodeMinProtocol) {
            LogPrintf("fne - ignoring outdated Fundamentalnode %s protocol version %d\n", vin.ToString().c_str(), protocolVersion);
            return;
        }

        CScript pubkeyScript;
        pubkeyScript.SetDestination(pubkey.GetID());

        if(pubkeyScript.size() != 25) {
            LogPrintf("fne - pubkey the wrong size\n");
            pfrom->Misbehaving( 100);
            return;
        }

        CScript pubkeyScript2;
        pubkeyScript2.SetDestination(pubkey2.GetID());

        if(pubkeyScript2.size() != 25) {
            LogPrintf("fne - pubkey2 the wrong size\n");
            pfrom->Misbehaving( 100);
            return;
        }

        if(!vin.scriptSig.empty()) {
            LogPrintf("fne - Ignore Not Empty ScriptSig %s\n",vin.ToString().c_str());
            return;
        }

        std::string errorMessage = "";
        if(!fnSigner.VerifyMessage(pubkey, vchSig, strMessage, errorMessage)){
            LogPrintf("fne - Got bad Fundamentalnode address signature\n");
            pfrom->Misbehaving( 100);
            return;
        }

        if(Params().NetworkID() == CChainParams::MAIN){
            if(addr.GetPort() != 5647) return;
        } else if(addr.GetPort() != 30420) return;

        //search existing Fundamentalnode list, this is where we update existing Fundamentalnodes with new dsee broadcasts
        CFundamentalnode* pfn = this->Find(vin);
        // if we are fundamentalnode but with undefined vin and this dsee is ours (matches our Fundamentalnode privkey) then just skip this part
        if(pfn != NULL && !(fFundamentalNode && activeFundamentalnode.vin == CTxIn() && pubkey2 == activeFundamentalnode.pubKeyFundamentalnode))
        {
            // count == -1 when it's a new entry
            //   e.g. We don't want the entry relayed/time updated when we're syncing the list
            // fn.pubkey = pubkey, IsVinAssociatedWithPubkey is validated once below,
            //   after that they just need to match
            if(count == -1 && pfn->pubkey == pubkey && !pfn->UpdatedWithin(FUNDAMENTALNODE_MIN_FNE_SECONDS)){
                pfn->UpdateLastSeen();

                if(pfn->sigTime < sigTime){ //take the newest entry
                    LogPrintf("dsee - Got updated entry for %s\n", addr.ToString().c_str());
                    pfn->pubkey2 = pubkey2;
                    pfn->sigTime = sigTime;
                    pfn->sig = vchSig;
                    pfn->protocolVersion = protocolVersion;
                    pfn->addr = addr;
                    pfn->donationAddress = donationAddress;
                    pfn->donationPercentage = donationPercentage;
                    pfn->Check();
                    if(pfn->IsEnabled())
                        fnmanager.RelayFundamentalnodeEntry(vin, addr, vchSig, sigTime, pubkey, pubkey2, count, current, lastUpdated, protocolVersion, donationAddress, donationPercentage);
                }
            }

            return;
        }

        // make sure the vout that was signed is related to the transaction that spawned the Fundamentalnode
        //  - this is expensive, so it's only done once per Fundamentalnode
        CTransaction tx;
        uint256 hashBlock;
        if(fnSigner.IsVinAssociatedWithPubkey(vin, pubkey, tx, hashBlock)) {

            if(pindexBest->nHeight > FN_AGE_ENFORCE_HEIGHT){

                int fn_input_blockheight = 0;

                if(mapBlockIndex.find(hashBlock) != mapBlockIndex.end())
                {
                    fn_input_blockheight = pindexBest->nHeight - mapBlockIndex[hashBlock]->nHeight;
                }
                else{
                    fn_input_blockheight = 0;}

                //Now return flase if age voilation
                if(fn_input_blockheight > BLOCK_AGE_THRESHOLD){
                    LogPrintf("fne -Age voilation \n");
                    return;
                }

            }

        }
        else{
            LogPrintf("fne - Got mismatched pubkey and vin Fehler1\n");
            pfrom->Misbehaving( 100);
            return;
        }

        if(fDebug) LogPrintf("fne - Got NEW Fundamentalnode entry %s\n", addr.ToString().c_str());

        // make sure it's burnt trnasaction
        //
        if(AcceptableFundamentalTxn(mempool, tx)){
            if(fDebug) LogPrintf("fne - Accepted Fundamentalnode entry %i %i\n", count, current);

            if(GetInputAge(vin) < FUNDAMENTALNODE_MIN_CONFIRMATIONS){
                LogPrintf("fne - Input must have least %d confirmations\n", FUNDAMENTALNODE_MIN_CONFIRMATIONS);
                pfrom->Misbehaving( 20);
                return;
            }

            // verify that sig time is legit in past
            // should be at least not earlier than block when  tx got FUNDAMENTALNODE_MIN_CONFIRMATIONS
            uint256 hashBlock = 0;
            GetTransaction(vin.prevout.hash, tx, hashBlock/* , true */);
            map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
            if (mi != mapBlockIndex.end() && (*mi).second)
            {
                CBlockIndex* pFNIndex = (*mi).second; // block for fn tx -> 1 confirmation
                CBlockIndex* pConfIndex = FindBlockByHeight((pFNIndex->nHeight + FUNDAMENTALNODE_MIN_CONFIRMATIONS - 1));  // block where tx got FUNDAMENTALNODE_MIN_CONFIRMATIONS
                if(pConfIndex->GetBlockTime() > sigTime)
                {
                    LogPrintf("fne - Bad sigTime %d for Fundamentalnode %20s %105s (%i conf block is at %d)\n",
                              sigTime, addr.ToString(), vin.ToString(), FUNDAMENTALNODE_MIN_CONFIRMATIONS, pConfIndex->GetBlockTime());
                    return;
                }
            }


            // use this as a peer
            addrman.Add(CAddress(addr), pfrom->addr, 2*60*60);
            
             //doesn't support multisig addresses
            if(donationAddress.IsPayToScriptHash()){
              donationAddress = CScript();
               donationPercentage = 0;
             }

            // add our Fundamentalnode
            CFundamentalnode fn(addr, vin, pubkey, vchSig, sigTime, pubkey2, protocolVersion, donationAddress, donationPercentage);
            fn.UpdateLastSeen(lastUpdated);
            this->Add(fn);

            // if it matches our Fundamentalnode privkey, then we've been remotely activated
            if(pubkey2 == activeFundamentalnode.pubKeyFundamentalnode && protocolVersion == PROTOCOL_VERSION){
                activeFundamentalnode.EnableHotColdFundamentalNode(vin, addr);
            }

            if(count == -1 && !isLocal)
                fnmanager.RelayFundamentalnodeEntry(vin, addr, vchSig, sigTime, pubkey, pubkey2, count, current, lastUpdated, protocolVersion, donationAddress, donationPercentage);

        } else {
            LogPrintf("fne - Rejected Fundamentalnode entry %s\n", addr.ToString().c_str());

        }
    }

    else if (strCommand == "fnep") { //FundamentalNode Entry Ping

        CTxIn vin;
        vector<unsigned char> vchSig;
        int64_t sigTime;
        bool stop;
        vRecv >> vin >> vchSig >> sigTime >> stop;

        LogPrintf("fnep - Received: vin: %s sigTime: %lld stop: %s\n", vin.ToString().c_str(), sigTime, stop ? "true" : "false");

        if (sigTime > GetAdjustedTime() + 60 * 60) {
            LogPrintf("fnep - Signature rejected, too far into the future %s\n", vin.ToString().c_str());
            return;
        }

        if (sigTime <= GetAdjustedTime() - 60 * 60) {
            LogPrintf("fnep - Signature rejected, too far into the past %s - %d %d \n", vin.ToString().c_str(), sigTime, GetAdjustedTime());
            return;
        }

        // see if we have this Fundamentalnode
        CFundamentalnode* pfn = this->Find(vin);
        if(pfn != NULL && pfn->protocolVersion >= nFundamentalnodeMinProtocol)
        {
            // LogPrintf("fnep - Found corresponding fn for vin: %s\n", vin.ToString().c_str());
            // take this only if it's newer
            if(pfn->lastFnep < sigTime)
            {
                std::string strMessage = pfn->addr.ToString() + boost::lexical_cast<std::string>(sigTime) + boost::lexical_cast<std::string>(stop);

                std::string errorMessage = "";
                if(!fnSigner.VerifyMessage(pfn->pubkey2, vchSig, strMessage, errorMessage))
                {
                    LogPrintf("fnep - Got bad Fundamentalnode address signature %s \n", vin.ToString().c_str());
                    //Misbehaving(pfrom->GetId(), 100);
                    return;
                }

                pfn->lastFnep = sigTime;

                if(!pfn->UpdatedWithin(FUNDAMENTALNODE_MIN_FNEP_SECONDS))
                {
                    if(stop) pfn->Disable();
                    else
                    {
                        pfn->UpdateLastSeen();
                        pfn->Check();
                        if(!pfn->IsEnabled()) return;
                    }
                    fnmanager.RelayFundamentalnodeEntryPing(vin, vchSig, sigTime, stop);
                }
            }
            return;
        }

        if(fDebug) LogPrintf("fnep - Couldn't find Fundamentalnode entry %s\n", vin.ToString().c_str());

        std::map<COutPoint, int64_t>::iterator i = mWeAskedForFundamentalnodeListEntry.find(vin.prevout);
        if (i != mWeAskedForFundamentalnodeListEntry.end())
        {
            int64_t t = (*i).second;
            if (GetTime() < t) return; // we've asked recently
        }

        // ask for the fne info once from the node that sent dseep

        LogPrintf("fnep - Asking source node for missing entry %s\n", vin.ToString().c_str());
        pfrom->PushMessage("fneg", vin);
        int64_t askAgain = GetTime() + FUNDAMENTALNODE_MIN_FNEP_SECONDS;
        mWeAskedForFundamentalnodeListEntry[vin.prevout] = askAgain;

    } else if (strCommand == "fvote") { //Fundamentalnode Vote

        CTxIn vin;
        vector<unsigned char> vchSig;
        int nVote;
        vRecv >> vin >> vchSig >> nVote;

        // see if we have this Fundamentalnode
        CFundamentalnode* pfn = this->Find(vin);
        if(pfn != NULL)
        {
            if((GetAdjustedTime() - pfn->lastVote) > (60*60))
            {
                std::string strMessage = vin.ToString() + boost::lexical_cast<std::string>(nVote);

                std::string errorMessage = "";
                if(!fnSigner.VerifyMessage(pfn->pubkey2, vchSig, strMessage, errorMessage))
                {
                    LogPrintf("fvote - Got bad Fundamentalnode address signature %s \n", vin.ToString().c_str());
                    return;
                }

                pfn->nVote = nVote;
                pfn->lastVote = GetAdjustedTime();

                //send to all peers
                LOCK(cs_vNodes);
                BOOST_FOREACH(CNode* pnode, vNodes)
                    pnode->PushMessage("fvote", vin, vchSig, nVote);
            }

            return;
        }

    } else if (strCommand == "fneg") { //Get Fundamentalnode list or specific entry

        CTxIn vin;
        vRecv >> vin;

        if(vin == CTxIn()) { //only should ask for this once
            //local network
            if(!pfrom->addr.IsRFC1918() && Params().NetworkID() == CChainParams::MAIN)
            {
                std::map<CNetAddr, int64_t>::iterator i = mAskedUsForFundamentalnodeList.find(pfrom->addr);
                if (i != mAskedUsForFundamentalnodeList.end())
                {
                    int64_t t = (*i).second;
                    if (GetTime() < t) {
                        pfrom->Misbehaving( 34);
                        LogPrintf("fneg - peer already asked me for the list\n");
                        return;
                    }
                }
                int64_t askAgain = GetTime() + FUNDAMENTALNODES_DSEG_SECONDS;
                mAskedUsForFundamentalnodeList[pfrom->addr] = askAgain;
            }
        } //else, asking for a specific node which is ok

        int count = this->size();
        int i = 0;

        BOOST_FOREACH(CFundamentalnode& fn, vFundamentalnodes) {

            if(fn.addr.IsRFC1918()) continue; //local network

            if(fn.IsEnabled())
            {
                if(fDebug) LogPrintf("fneg - Sending Fundamentalnode entry - %s \n", fn.addr.ToString().c_str());
                if(vin == CTxIn()){
                    pfrom->PushMessage("fne", fn.vin, fn.addr, fn.sig, fn.sigTime, fn.pubkey, fn.pubkey2, count, i, fn.lastTimeSeen, fn.protocolVersion, fn.donationAddress, fn.donationPercentage);
                } else if (vin == fn.vin) {
                    pfrom->PushMessage("fne", fn.vin, fn.addr, fn.sig, fn.sigTime, fn.pubkey, fn.pubkey2, count, i, fn.lastTimeSeen, fn.protocolVersion, fn.donationAddress, fn.donationPercentage);
                    LogPrintf("fneg - Sent 1 Fundamentalnode entries to %s\n", pfrom->addr.ToString().c_str());
                    return;
                }
                i++;
            }
        }

        LogPrintf("fneg - Sent %d Fundamentalnode entries to %s\n", i, pfrom->addr.ToString().c_str());
    }

}
void CFundamentalnodeMan::RelayFundamentalnodeEntry(const CTxIn vin, const CService addr, const std::vector<unsigned char> vchSig, const int64_t nNow, const CPubKey pubkey, const CPubKey pubkey2, const int count, const int current, const int64_t lastUpdated, const int protocolVersion, CScript donationAddress, int donationPercentage)
{
     LOCK(cs_vNodes);
     BOOST_FOREACH(CNode* pnode, vNodes)
     pnode->PushMessage("fne", vin, addr, vchSig, nNow, pubkey, pubkey2, count, current, lastUpdated, protocolVersion, donationAddress, donationPercentage);
}

void CFundamentalnodeMan::RelayFundamentalnodeEntryPing(const CTxIn vin, const std::vector<unsigned char> vchSig, const int64_t nNow, const bool stop)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
        pnode->PushMessage("fnep", vin, vchSig, nNow, stop);
}

void CFundamentalnodeMan::Remove(CTxIn vin)
{
    LOCK(cs);

    vector<CFundamentalnode>::iterator it = vFundamentalnodes.begin();
    while(it != vFundamentalnodes.end()){
        if((*it).vin == vin){
            if(fDebug) LogPrintf("CFundamentalnodeMan: Removing Fundamentalnode %s - %i now\n", (*it).addr.ToString().c_str(), size() - 1);
            vFundamentalnodes.erase(it);
            break;
        }
        ++it;
    }
}

std::string CFundamentalnodeMan::ToString() const
{
    std::ostringstream info;

    info << "Fundamentalnodes: " << (int)vFundamentalnodes.size() <<
            ", peers who asked us for Fundamentalnode list: " << (int)mAskedUsForFundamentalnodeList.size() <<
            ", peers we asked for Fundamentalnode list: " << (int)mWeAskedForFundamentalnodeList.size() <<
            ", entries in Fundamentalnode list we asked for: " << (int)mWeAskedForFundamentalnodeListEntry.size() <<
            ", nDsqCount: " << (int)nDsqCount;

    return info.str();
}
