// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef ACTIVEFUNDAMENTALNODE_H
#define ACTIVEFUNDAMENTALNODE_H

#include "bignum.h"
#include "sync.h"
#include "net.h"
#include "key.h"
#include "core.h"
#include "init.h"
#include "wallet.h"
#include "spork.h"
#include "fundamentalnode.h"


//static const int64_t FUNDAMENTALNODEAMOUNT = (2500000 + 1)*COIN;//2500000;
inline int64_t GetFNCollateral(int nHeight) {    
    if (nHeight > 95000)
        return 15000000*COIN; //15 million
    
    if (nHeight > 85000)
        return 20000000*COIN; //20 million

    return 25000000*COIN; //25 million

}

// Responsible for activating the Fundamentalnode and pinging the network
class CActiveFundamentalnode
{
public:
	// Initialized by init.cpp
	// Keys for the main Fundamentalnode
	CPubKey pubKeyFundamentalnode;

	// Initialized while registering Fundamentalnode
	CTxIn vin;
    CService service;

    int status;
    std::string notCapableReason;

    CActiveFundamentalnode()
    {        
        status = FUNDAMENTALNODE_NOT_PROCESSED;
    }

    /// Manage status of main Fundamentalnode
    void ManageStatus(); 

    /// Ping for main Fundamentalnode
    bool Fnep(std::string& errorMessage);
    /// Ping for any Fundamentalnode
    bool Fnep(CTxIn vin, CService service, CKey key, CPubKey pubKey, std::string &retErrorMessage, bool stop);

    /// Stop main Fundamentalnode
    bool StopFundamentalNode(std::string& errorMessage); 
    /// Stop remote Fundamentalnode
    bool StopFundamentalNode(std::string strService, std::string strKeyFundamentalnode, std::string& errorMessage); 
    /// Stop any Fundamentalnode
    bool StopFundamentalNode(CTxIn vin, CService service, CKey key, CPubKey pubKey, std::string& errorMessage); 

    /// Register remote Fundamentalnode
    bool Register(std::string strService, std::string strKey, std::string txHash, std::string strOutputIndex, std::string strDonationAddress, std::string strDonationPercentage, std::string& errorMessage); 
    /// Register any Fundamentalnode
    bool Register(CTxIn vin, CService service, CKey key, CPubKey pubKey, CKey keyFundamentalnode, CPubKey pubKeyFundamentalnode, CScript donationAddress, int donationPercentage, std::string &retErrorMessage); 

    /// Get input that can be used for the Fundamentalnode
    bool GetFundamentalNodeVin(CTxIn& vin, CPubKey& pubkey, CKey& secretKey);
    bool GetFundamentalNodeVin(CTxIn& vin, CPubKey& pubkey, CKey& secretKey, std::string strTxHash, std::string strOutputIndex);
    vector<COutput> SelectCoinsFundamentalnode();
    bool GetVinFromOutput(COutput out, CTxIn& vin, CPubKey& pubkey, CKey& secretKey);

    /// Enable hot wallet mode (run a Fundamentalnode with no funds)
    bool EnableHotColdFundamentalNode(CTxIn& vin, CService& addr);
};

extern CActiveFundamentalnode activeFundamentalnode;

#endif
