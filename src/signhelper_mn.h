



#include "main.h"

#include "sync.h"
#include "fn-activity.h"

#include "base58.h"




class CFNSignHelper{

	public:
	CScript collateralPubKey;
    /// Is the inputs associated with this public key? 
    bool IsVinAssociatedWithPubkey(CTxIn& vin, CPubKey& pubkey, CTransaction& tx, uint256& hashBlock){
		CScript payee2;
		payee2.SetDestination(pubkey.GetID());
        //bool IsBurntTxn;TODO: implement input check
        //bool IsFnTxn;

        if(GetTransaction(vin.prevout.hash, tx, hashBlock)){

            BOOST_FOREACH(CTxOut out, tx.vout){
                if(out.nValue == 1*COIN){
                    if(out.scriptPubKey == payee2) return true;
				}
			}

		}

		return false;
	}
    /// Set the private/public key values, returns true if successful
    bool SetKey(std::string strSecret, std::string& errorMessage, CKey& key, CPubKey& pubkey){
		CBitcoinSecret vchSecret;
		bool fGood = vchSecret.SetString(strSecret);

		if (!fGood) {
			errorMessage = _("Invalid private key.");
			return false;
		}

		key = vchSecret.GetKey();
		pubkey = key.GetPubKey();

		return true;
	}

    /// Sign the message, returns true if successful
    bool SignMessage(std::string strMessage, std::string& errorMessage, vector<unsigned char>& vchSig, CKey key)
	{
	    CHashWriter ss(SER_GETHASH, 0);
		ss << strMessageMagic;
		ss << strMessage;

		if (!key.SignCompact(ss.GetHash(), vchSig)) {
			errorMessage = _("Signing failed.");
			return false;
		}

		return true;
	}
    /// Verify the message, returns true if succcessful
    bool VerifyMessage(CPubKey pubkey, vector<unsigned char>& vchSig, std::string strMessage, std::string& errorMessage)
	{
		CHashWriter ss(SER_GETHASH, 0);
		ss << strMessageMagic;
		ss << strMessage;

		CPubKey pubkey2;
		if (!pubkey2.RecoverCompact(ss.GetHash(), vchSig)) {
			errorMessage = _("Error recovering public key.");
			return false;
		}

		if (fDebug && pubkey2.GetID() != pubkey.GetID())
			LogPrintf("CFnSigner::VerifyMessage -- keys don't match: %s %s", pubkey2.GetID().ToString(), pubkey.GetID().ToString());

		return (pubkey2.GetID() == pubkey.GetID());
	}
	
	bool SetCollateralAddress(std::string strAddress){
		CBitcoinAddress address;
		if (!address.SetString(strAddress))
		{
            LogPrintf("CFnSigner::SetCollateralAddress - Invalid collateral address\n");
			return false;
		}
		collateralPubKey.SetDestination(address.Get());
		return true;
	}
    void InitCollateralAddress(){
        std::string strAddress = "";
        
            strAddress = "Sfe14q84qEHJZuUHqSVNX9Sog7gzzaR5Vc";//sample address
            
        
        SetCollateralAddress(strAddress);
    }

};

void ThreadBitPool();



extern CFNSignHelper fnSigner;
extern std::string strFundamentalNodePrivKey;


