

// Copyright (c) 2016-2017 The B3Coin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef FUNDAMENTALNODE_POS_H
#define FUNDAMENTALNODE_POS_H

#include "bignum.h"
#include "sync.h"
#include "net.h"
#include "key.h"
#include "core.h"
#include "util.h"
#include "script.h"
#include "base58.h"
#include "main.h"

using namespace std;
using namespace boost;

class CFundamentalnodeScanning;
class CFundamentalnodeScanningError;

extern map<uint256, CFundamentalnodeScanningError> mapFundamentalnodeScanningErrors;
extern CFundamentalnodeScanning fnscan;

static const int MIN_FUNDAMENTALNODE_POS_PROTO_VERSION = 70075;
static const int BLOCK_AGE_THRESHOLD = 7*7215;//7 months
static const int FN_AGE_ENFORCE_HEIGHT = 85000;

/*
	1% of the network is scanned every 2.5 minutes, making a full
	round of scanning take about 4.16 hours. We're targeting about
	a day of proof-of-service errors for complete removal from the
	fundamentalnode system.
*/
static const int FUNDAMENTALNODE_SCANNING_ERROR_THESHOLD = 6;  //6 Bitsenddev  to little Fundamentalnode for test

#define SCANNING_SUCCESS                       1
#define SCANNING_ERROR_NO_RESPONSE             2
#define SCANNING_ERROR_IX_NO_RESPONSE          3
#define SCANNING_ERROR_MAX                     3

void ProcessMessageFundamentalnodePOS(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);

class CFundamentalnodeScanning
{
public:
    void DoFundamentalnodePOSChecks();
    void CleanFundamentalnodeScanningErrors();
};

// Returns how many fundamentalnodes are allowed to scan each block
int GetCountScanningPerBlock();

class CFundamentalnodeScanningError
{
public:
    CTxIn vinFundamentalnodeA;
    CTxIn vinFundamentalnodeB;
    int nErrorType;
    int nExpiration;
    int nBlockHeight;
    std::vector<unsigned char> vchFundamentalNodeSignature;

    CFundamentalnodeScanningError ()
    {
        vinFundamentalnodeA = CTxIn();
        vinFundamentalnodeB = CTxIn();
        nErrorType = 0;
        nExpiration = GetTime()+(60*60);
        nBlockHeight = 0;
    }

    CFundamentalnodeScanningError (CTxIn& vinFundamentalnodeAIn, CTxIn& vinFundamentalnodeBIn, int nErrorTypeIn, int nBlockHeightIn)
    {
    	vinFundamentalnodeA = vinFundamentalnodeAIn;
    	vinFundamentalnodeB = vinFundamentalnodeBIn;
    	nErrorType = nErrorTypeIn;
    	nExpiration = GetTime()+(60*60);
    	nBlockHeight = nBlockHeightIn;
    }

    CFundamentalnodeScanningError (CTxIn& vinFundamentalnodeBIn, int nErrorTypeIn, int nBlockHeightIn)
    {
        //just used for IX, FundamentalnodeA is any client
        vinFundamentalnodeA = CTxIn();
        vinFundamentalnodeB = vinFundamentalnodeBIn;
        nErrorType = nErrorTypeIn;
        nExpiration = GetTime()+(60*60);
        nBlockHeight = nBlockHeightIn;
    }

    uint256 GetHash() const {return SerializeHash(*this);}

    bool SignatureValid();
    bool Sign();
    bool IsExpired() {return GetTime() > nExpiration;}
    void Relay();
    bool IsValid() {
    	return (nErrorType > 0 && nErrorType <= SCANNING_ERROR_MAX);
    }

    IMPLEMENT_SERIALIZE
    (
        READWRITE(vinFundamentalnodeA);
        READWRITE(vinFundamentalnodeB);
        READWRITE(nErrorType);
        READWRITE(nExpiration);
        READWRITE(nBlockHeight);
        READWRITE(vchFundamentalNodeSignature);
    )
};


#endif
