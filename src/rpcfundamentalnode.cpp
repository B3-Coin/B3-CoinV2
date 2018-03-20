#include "main.h"
#include "core.h"
#include "db.h"
#include "init.h"
#include "fn-activity.h"
#include "fn-manager.h"
#include "fn-config.h"
#include "rpcserver.h"
#include <boost/lexical_cast.hpp>

#include <fstream>
using namespace json_spirit;
using namespace std;

Value fundamentalnode(const Array& params, bool fHelp)
{
    string strCommand;
    if (params.size() >= 1)
        strCommand = params[0].get_str();

    if (fHelp  ||
        (strCommand != "start" && strCommand != "start-alias" && strCommand != "start-many" && strCommand != "stop" && strCommand != "stop-alias" && strCommand != "stop-many" && strCommand != "list" && strCommand != "list-conf" && strCommand != "count"  && strCommand != "enforce"
            && strCommand != "debug" && strCommand != "current" && strCommand != "winners" && strCommand != "genkey" && strCommand != "connect" && strCommand != "outputs" && strCommand != "vote-many" && strCommand != "vote"))
        throw runtime_error(
                "fundamentalnode \"command\"... ( \"passphrase\" )\n"
                "Set of commands to execute fundamentalnode related actions\n"
                "\nArguments:\n"
                "1. \"command\"        (string or set of strings, required) The command to execute\n"
                "2. \"passphrase\"     (string, optional) The wallet passphrase\n"
                "\nAvailable commands:\n"
                "  count        - Print number of all known fundamentalnodes (optional: 'enabled', 'both')\n"
                "  current      - Print info on current fundamentalnode winner\n"
                "  debug        - Print fundamentalnode status\n"
                "  genkey       - Generate new fundamentalnodeprivkey\n"
                "  enforce      - Enforce fundamentalnode payments\n"
                "  outputs      - Print fundamentalnode compatible outputs\n"
                "  start        - Start fundamentalnode configured in b3coin.conf\n"
                "  start-alias  - Start single fundamentalnode by assigned alias configured in fundamentalnode.conf\n"
                "  start-many   - Start all fundamentalnodes configured in fundamentalnode.conf\n"
                "  stop         - Stop fundamentalnode configured in b3coin.conf\n"
                "  stop-alias   - Stop single fundamentalnode by assigned alias configured in fundamentalnode.conf\n"
                "  stop-many    - Stop all fundamentalnodes configured in fundamentalnode.conf\n"
                "  list         - Print list of all known fundamentalnodes (see fundamentalnodelist for more info)\n"
                "  list-conf    - Print fundamentalnode.conf in JSON format\n"
                "  winners      - Print list of fundamentalnode winners\n"
                "  vote-many    - Vote on a B3 initiative\n"
                "  vote         - Vote on a B3 initiative\n"
                );


    if (strCommand == "stop")
    {
        if(!fFundamentalNode) return "you must set fundamentalnode=1 in the configuration";

        if(pwalletMain->IsLocked()) {
            SecureString strWalletPass;
            strWalletPass.reserve(100);

            if (params.size() == 2){
                strWalletPass = params[1].get_str().c_str();
            } else {
                throw runtime_error(
                    "Your wallet is locked, passphrase is required\n");
            }

            if(!pwalletMain->Unlock(strWalletPass)){
                return "incorrect passphrase";
            }
        }

        std::string errorMessage;
        if(!activeFundamentalnode.StopFundamentalNode(errorMessage)) {
        	return "stop failed: " + errorMessage;
        }
        pwalletMain->Lock();

        if(activeFundamentalnode.status == FUNDAMENTALNODE_STOPPED) return "successfully stopped fundamentalnode";
        if(activeFundamentalnode.status == FUNDAMENTALNODE_NOT_CAPABLE) return "not capable fundamentalnode";

        return "unknown";
    }

    if (strCommand == "stop-alias")
    {
	    if (params.size() < 2){
			throw runtime_error(
			"command needs at least 2 parameters\n");
	    }

	    std::string alias = params[1].get_str().c_str();

    	if(pwalletMain->IsLocked()) {
    		SecureString strWalletPass;
    	    strWalletPass.reserve(100);

			if (params.size() == 3){
				strWalletPass = params[2].get_str().c_str();
			} else {
				throw runtime_error(
				"Your wallet is locked, passphrase is required\n");
			}

			if(!pwalletMain->Unlock(strWalletPass)){
				return "incorrect passphrase";
			}
        }

    	bool found = false;

		Object statusObj;
		statusObj.push_back(Pair("alias", alias));

    	BOOST_FOREACH(CFundamentalnodeConfig::CFundamentalnodeEntry mne, fundamentalnodeConfig.getEntries()) {
    		if(mne.getAlias() == alias) {
    			found = true;
    			std::string errorMessage;
    			bool result = activeFundamentalnode.StopFundamentalNode(mne.getIp(), mne.getPrivKey(), errorMessage);

				statusObj.push_back(Pair("result", result ? "successful" : "failed"));
    			if(!result) {
   					statusObj.push_back(Pair("errorMessage", errorMessage));
   				}
    			break;
    		}
    	}

    	if(!found) {
    		statusObj.push_back(Pair("result", "failed"));
    		statusObj.push_back(Pair("errorMessage", "could not find alias in config. Verify with list-conf."));
    	}

    	pwalletMain->Lock();
    	return statusObj;
    }

    if (strCommand == "stop-many")
    {
    	if(pwalletMain->IsLocked()) {
			SecureString strWalletPass;
			strWalletPass.reserve(100);

			if (params.size() == 2){
				strWalletPass = params[1].get_str().c_str();
			} else {
				throw runtime_error(
				"Your wallet is locked, passphrase is required\n");
			}

			if(!pwalletMain->Unlock(strWalletPass)){
				return "incorrect passphrase";
			}
		}

		int total = 0;
		int successful = 0;
		int fail = 0;


		Object resultsObj;

		BOOST_FOREACH(CFundamentalnodeConfig::CFundamentalnodeEntry mne, fundamentalnodeConfig.getEntries()) {
			total++;

			std::string errorMessage;
			bool result = activeFundamentalnode.StopFundamentalNode(mne.getIp(), mne.getPrivKey(), errorMessage);

			Object statusObj;
			statusObj.push_back(Pair("alias", mne.getAlias()));
			statusObj.push_back(Pair("result", result ? "successful" : "failed"));

			if(result) {
				successful++;
			} else {
				fail++;
				statusObj.push_back(Pair("errorMessage", errorMessage));
			}

			resultsObj.push_back(Pair("status", statusObj));
		}
		pwalletMain->Lock();

		Object returnObj;
		returnObj.push_back(Pair("overall", "Successfully stopped " + boost::lexical_cast<std::string>(successful) + " fundamentalnodes, failed to stop " +
				boost::lexical_cast<std::string>(fail) + ", total " + boost::lexical_cast<std::string>(total)));
		returnObj.push_back(Pair("detail", resultsObj));

		return returnObj;

    }

    if (strCommand == "list")
    {
        Array newParams(params.size() - 1);
        std::copy(params.begin() + 1, params.end(), newParams.begin());
        return fundamentalnodelist(newParams, fHelp);
    }

    if (strCommand == "count")
    {
        if (params.size() > 2){
            throw runtime_error(
            "too many parameters\n");
        }
        if (params.size() == 2)
        {
            if(params[1] == "enabled") return fnmanager.CountEnabled();
            if(params[1] == "both") return boost::lexical_cast<std::string>(fnmanager.CountEnabled()) + " / " + boost::lexical_cast<std::string>(fnmanager.size());
        }
        return fnmanager.size();
    }

    if (strCommand == "start")
    {
        if(!fFundamentalNode) return "you must set fundamentalnode=1 in the configuration";

        if(pwalletMain->IsLocked()) {
            SecureString strWalletPass;
            strWalletPass.reserve(100);

            if (params.size() == 2){
                strWalletPass = params[1].get_str().c_str();
            } else {
                throw runtime_error(
                    "Your wallet is locked, passphrase is required\n");
            }

            if(!pwalletMain->Unlock(strWalletPass)){
                return "incorrect passphrase";
            }
        }

        if(activeFundamentalnode.status != FUNDAMENTALNODE_REMOTELY_ENABLED && activeFundamentalnode.status != FUNDAMENTALNODE_IS_CAPABLE){
            activeFundamentalnode.status = FUNDAMENTALNODE_NOT_PROCESSED; // TODO: consider better way
            std::string errorMessage;
            activeFundamentalnode.ManageStatus();
            pwalletMain->Lock();
        }

        if(activeFundamentalnode.status == FUNDAMENTALNODE_REMOTELY_ENABLED) return "fundamentalnode started remotely";
        if(activeFundamentalnode.status == FUNDAMENTALNODE_INPUT_TOO_NEW) return "fundamentalnode input must have at least 15 confirmations";
        if(activeFundamentalnode.status == FUNDAMENTALNODE_STOPPED) return "fundamentalnode is stopped";
        if(activeFundamentalnode.status == FUNDAMENTALNODE_IS_CAPABLE) return "successfully started fundamentalnode";
        if(activeFundamentalnode.status == FUNDAMENTALNODE_NOT_CAPABLE) return "not capable fundamentalnode: " + activeFundamentalnode.notCapableReason;
        if(activeFundamentalnode.status == FUNDAMENTALNODE_SYNC_IN_PROCESS) return "sync in process. Must wait until client is synced to start.";

        return "unknown";
    }

    if (strCommand == "start-alias")
    {
	    if (params.size() < 2){
			throw runtime_error(
			"command needs at least 2 parameters\n");
	    }

	    std::string alias = params[1].get_str().c_str();

    	if(pwalletMain->IsLocked()) {
    		SecureString strWalletPass;
    	    strWalletPass.reserve(100);

			if (params.size() == 3){
				strWalletPass = params[2].get_str().c_str();
			} else {
				throw runtime_error(
				"Your wallet is locked, passphrase is required\n");
			}

			if(!pwalletMain->Unlock(strWalletPass)){
				return "incorrect passphrase";
			}
        }

    	bool found = false;

		Object statusObj;
		statusObj.push_back(Pair("alias", alias));

    	BOOST_FOREACH(CFundamentalnodeConfig::CFundamentalnodeEntry mne, fundamentalnodeConfig.getEntries()) {
    		if(mne.getAlias() == alias) {
    			found = true;
    			std::string errorMessage;

                std::string strDonateAddress = mne.getDonationAddress();
                std::string strDonationPercentage = mne.getDonationPercentage();

    			bool result = activeFundamentalnode.Register(mne.getIp(), mne.getPrivKey(), mne.getTxHash(), mne.getOutputIndex(), strDonateAddress, strDonationPercentage, errorMessage);

    			statusObj.push_back(Pair("result", result ? "successful" : "failed"));
    			if(!result) {
					statusObj.push_back(Pair("errorMessage", errorMessage));
				}
    			break;
    		}
    	}

    	if(!found) {
    		statusObj.push_back(Pair("result", "failed"));
    		statusObj.push_back(Pair("errorMessage", "could not find alias in config. Verify with list-conf."));
    	}

    	pwalletMain->Lock();
    	return statusObj;

    }

    if (strCommand == "start-many")
    {
    	if(pwalletMain->IsLocked()) {
			SecureString strWalletPass;
			strWalletPass.reserve(100);

			if (params.size() == 2){
				strWalletPass = params[1].get_str().c_str();
			} else {
				throw runtime_error(
				"Your wallet is locked, passphrase is required\n");
			}

			if(!pwalletMain->Unlock(strWalletPass)){
				return "incorrect passphrase";
			}
		}

		std::vector<CFundamentalnodeConfig::CFundamentalnodeEntry> mnEntries;
		mnEntries = fundamentalnodeConfig.getEntries();

		int total = 0;
		int successful = 0;
		int fail = 0;

		Object resultsObj;

		BOOST_FOREACH(CFundamentalnodeConfig::CFundamentalnodeEntry mne, fundamentalnodeConfig.getEntries()) {
			total++;

			std::string errorMessage;

            std::string strDonateAddress = mne.getDonationAddress();
            std::string strDonationPercentage = mne.getDonationPercentage();

			bool result = activeFundamentalnode.Register(mne.getIp(), mne.getPrivKey(), mne.getTxHash(), mne.getOutputIndex(), strDonateAddress, strDonationPercentage, errorMessage);

			Object statusObj;
			statusObj.push_back(Pair("alias", mne.getAlias()));
            statusObj.push_back(Pair("result", result ? "successful" : "failed"));

			if(result) {
				successful++;
			} else {
				fail++;
				statusObj.push_back(Pair("errorMessage", errorMessage));
			}

			resultsObj.push_back(Pair("status", statusObj));
		}
		pwalletMain->Lock();

		Object returnObj;
		returnObj.push_back(Pair("overall", "Successfully started " + boost::lexical_cast<std::string>(successful) + " fundamentalnodes, failed to start " +
				boost::lexical_cast<std::string>(fail) + ", total " + boost::lexical_cast<std::string>(total)));
		returnObj.push_back(Pair("detail", resultsObj));

		return returnObj;
    }

    if (strCommand == "debug")
    {
        if(activeFundamentalnode.status == FUNDAMENTALNODE_REMOTELY_ENABLED) return "fundamentalnode started remotely";
        if(activeFundamentalnode.status == FUNDAMENTALNODE_INPUT_TOO_NEW) return "fundamentalnode input must have at least 15 confirmations";
        if(activeFundamentalnode.status == FUNDAMENTALNODE_IS_CAPABLE) return "successfully started fundamentalnode";
        if(activeFundamentalnode.status == FUNDAMENTALNODE_STOPPED) return "fundamentalnode is stopped";
        if(activeFundamentalnode.status == FUNDAMENTALNODE_NOT_CAPABLE) return "not capable fundamentalnode: " + activeFundamentalnode.notCapableReason;
        if(activeFundamentalnode.status == FUNDAMENTALNODE_SYNC_IN_PROCESS) return "sync in process. Must wait until client is synced to start.";

        CTxIn vin = CTxIn();
        CPubKey pubkey = CScript();
        CKey key;
        bool found = activeFundamentalnode.GetFundamentalNodeVin(vin, pubkey, key);
        if(!found){
            return "Missing fundamentalnode input, please look at the documentation for instructions on fundamentalnode creation";
        } else {
            return "No problems were found";
        }
    }

    if (strCommand == "create")
    {

        return "Not implemented yet, please look at the documentation for instructions on fundamentalnode creation";
    }

    if (strCommand == "current")
    {
        CFundamentalnode* winner = fnmanager.GetCurrentFundamentalNode(1);
        if(winner) {
            Object obj;
            CScript pubkey;
            pubkey.SetDestination(winner->pubkey.GetID());
            CTxDestination address1;
            ExtractDestination(pubkey, address1);
            CBitcoinAddress address2(address1);

            obj.push_back(Pair("IP:port",       winner->addr.ToString().c_str()));
            obj.push_back(Pair("protocol",      (int64_t)winner->protocolVersion));
            obj.push_back(Pair("vin",           winner->vin.prevout.hash.ToString().c_str()));
            obj.push_back(Pair("pubkey",        address2.ToString().c_str()));
            obj.push_back(Pair("lastseen",      (int64_t)winner->lastTimeSeen));
            obj.push_back(Pair("activeseconds", (int64_t)(winner->lastTimeSeen - winner->sigTime)));
            return obj;
        }

        return "unknown";
    }

    if (strCommand == "genkey")
    {
        CKey secret;
        secret.MakeNewKey(false);

        Object obj;

        CPubKey pubkey;
        pubkey = secret.GetPubKey();
        std::string hexstring = HexStr(pubkey.begin(), pubkey.end());
        CScript scriptaddr ;
        scriptaddr.SetDestination(pubkey.GetID());

        CTxDestination address1;
        ExtractDestination(scriptaddr, address1);
        CBitcoinAddress address2(address1);

        obj.push_back(Pair(" Secret key ",  CBitcoinSecret(secret).ToString()));
        obj.push_back(Pair(" Public key ",  hexstring));
        obj.push_back(Pair(" Address "   , address2.ToString().c_str()));

        return obj;
    }

    if (strCommand == "winners")
    {
        Object obj;

        for(int nHeight = pindexBest->nHeight-10; nHeight < pindexBest->nHeight+20; nHeight++)
        {
            CScript payee;
            if(fundamentalnodePayments.GetBlockPayee(nHeight, payee)){
                CTxDestination address1;
                ExtractDestination(payee, address1);
                CBitcoinAddress address2(address1);
                obj.push_back(Pair(boost::lexical_cast<std::string>(nHeight),       address2.ToString().c_str()));
            } else {
                obj.push_back(Pair(boost::lexical_cast<std::string>(nHeight),       ""));
            }
        }

        return obj;
    }

    if(strCommand == "enforce")
    {
        return (uint64_t)enforceFundamentalnodePaymentsTime;
    }

    if(strCommand == "connect")
    {
        std::string strAddress = "";
        if (params.size() == 2){
            strAddress = params[1].get_str().c_str();
        } else {
            throw runtime_error(
                "Fundamentalnode address required\n");
        }

        CService addr = CService(strAddress);

        if(ConnectNode((CAddress)addr, NULL/* , true */)){
            return "successfully connected";
        } else {
            return "error connecting";
        }
    }

    if(strCommand == "list-conf")
    {
    	std::vector<CFundamentalnodeConfig::CFundamentalnodeEntry> mnEntries;
    	mnEntries = fundamentalnodeConfig.getEntries();

        Object resultObj;

        BOOST_FOREACH(CFundamentalnodeConfig::CFundamentalnodeEntry mne, fundamentalnodeConfig.getEntries()) {
            Object mnObj;
            mnObj.push_back(Pair("alias", mne.getAlias()));
            mnObj.push_back(Pair("address", mne.getIp()));
            mnObj.push_back(Pair("privateKey", mne.getPrivKey()));
            mnObj.push_back(Pair("txHash", mne.getTxHash()));
            mnObj.push_back(Pair("outputIndex", mne.getOutputIndex()));
            mnObj.push_back(Pair("donationAddress", mne.getDonationAddress()));
            mnObj.push_back(Pair("donationPercent", mne.getDonationPercentage()));
            resultObj.push_back(Pair("fundamentalnode", mnObj));
        }

        return resultObj;
    }

    if (strCommand == "outputs"){
        // Find possible candidates
        vector<COutput> possibleCoins = activeFundamentalnode.SelectCoinsFundamentalnode();

        Object obj;
        BOOST_FOREACH(COutput& out, possibleCoins) {
            obj.push_back(Pair(out.tx->GetHash().ToString().c_str(), boost::lexical_cast<std::string>(out.i)));
        }

        return obj;

    }

    if(strCommand == "vote-many")
    {
        std::vector<CFundamentalnodeConfig::CFundamentalnodeEntry> mnEntries;
        mnEntries = fundamentalnodeConfig.getEntries();
        
        if (params.size() != 2)
        throw runtime_error("You can only vote 'yea' or 'nay'");

        std::string vote = params[1].get_str().c_str();
        if(vote != "yea" && vote != "nay") return "You can only vote 'yea' or 'nay'";
        int nVote = 0;
        if(vote == "yea") nVote = 1;
        if(vote == "nay") nVote = -1;


	int success = 0;
	int failed = 0;

        Object resultObj;

        BOOST_FOREACH(CFundamentalnodeConfig::CFundamentalnodeEntry mne, fundamentalnodeConfig.getEntries()) {
            std::string errorMessage;
            std::vector<unsigned char> vchFundamentalNodeSignature;
            std::string strFundamentalNodeSignMessage;

            CPubKey pubKeyCollateralAddress;
            CKey keyCollateralAddress;
            CPubKey pubKeyFundamentalnode;
            CKey keyFundamentalnode;

if(!fnSigner.SetKey(mne.getPrivKey(), errorMessage, keyFundamentalnode, pubKeyFundamentalnode)){
printf(" Error upon calling SetKey for %s\n", mne.getAlias().c_str());
failed++;
continue;
}

CFundamentalnode* pfn = fnmanager.Find(pubKeyFundamentalnode);
if(pfn == NULL)
{
printf("Can't find fundamentalnode by pubkey for %s\n", mne.getAlias().c_str());
failed++;
continue;
            }

            std::string strMessage = pfn->vin.ToString() + boost::lexical_cast<std::string>(nVote);

if(!fnSigner.SignMessage(strMessage, errorMessage, vchFundamentalNodeSignature, keyFundamentalnode)){
	printf(" Error upon calling SignMessage for %s\n", mne.getAlias().c_str());
	failed++;
	continue;
}

if(!fnSigner.SignMessage(strMessage, errorMessage, vchFundamentalNodeSignature, keyFundamentalnode)){
	printf(" Error upon calling SignMessage for %s\n", mne.getAlias().c_str());
	failed++;
	continue;
}

success++;

            //send to all peers
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodes)
                pnode->PushMessage("fvote", pfn->vin, vchFundamentalNodeSignature, nVote);

        }
return("Voted successfully " + boost::lexical_cast<std::string>(success) + " time(s) and failed " + boost::lexical_cast<std::string>(failed) + " time(s).");
    }

    if(strCommand == "vote")
    {
        std::vector<CFundamentalnodeConfig::CFundamentalnodeEntry> mnEntries;
        mnEntries = fundamentalnodeConfig.getEntries();
        
        if (params.size() != 2)
        throw runtime_error("You can only vote 'yea' or 'nay'");

        std::string vote = params[1].get_str().c_str();
        if(vote != "yea" && vote != "nay") return "You can only vote 'yea' or 'nay'";
        int nVote = 0;
        if(vote == "yea") nVote = 1;
        if(vote == "nay") nVote = -1;

        // Choose coins to use
        CPubKey pubKeyCollateralAddress;
        CKey keyCollateralAddress;
        CPubKey pubKeyFundamentalnode;
        CKey keyFundamentalnode;

        std::string errorMessage;
        std::vector<unsigned char> vchFundamentalNodeSignature;
        std::string strMessage = activeFundamentalnode.vin.ToString() + boost::lexical_cast<std::string>(nVote);

        if(!fnSigner.SetKey(strFundamentalNodePrivKey, errorMessage, keyFundamentalnode, pubKeyFundamentalnode))
            return(" Error upon calling SetKey");

        if(!fnSigner.SignMessage(strMessage, errorMessage, vchFundamentalNodeSignature, keyFundamentalnode))
            return(" Error upon calling SignMessage");

        if(!fnSigner.VerifyMessage(pubKeyFundamentalnode, vchFundamentalNodeSignature, strMessage, errorMessage))
            return(" Error upon calling VerifyMessage");

        //send to all peers
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes)
            pnode->PushMessage("fvote", activeFundamentalnode.vin, vchFundamentalNodeSignature, nVote);

    }

    return Value::null;
}

Value fundamentalnodelist(const Array& params, bool fHelp)
{
    std::string strMode = "status";
    std::string strFilter = "";

    if (params.size() >= 1) strMode = params[0].get_str();
    if (params.size() == 2) strFilter = params[1].get_str();

    if (fHelp ||
            (strMode != "status" && strMode != "vin" && strMode != "pubkey" && strMode != "lastseen" && strMode != "activeseconds" && strMode != "rank"
                && strMode != "protocol" && strMode != "full" && strMode != "votes" && strMode != "donation" && strMode != "pose"))
    {
        throw runtime_error(
                "fundamentalnodelist ( \"mode\" \"filter\" )\n"
                "Get a list of fundamentalnodes in different modes\n"
                "\nArguments:\n"
                "1. \"mode\"      (string, optional/required to use filter, defaults = status) The mode to run list in\n"
                "2. \"filter\"    (string, optional) Filter results. Partial match by IP by default in all modes, additional matches in some modes\n"
                "\nAvailable modes:\n"
                "  activeseconds  - Print number of seconds fundamentalnode recognized by the network as enabled\n"
                "  donation       - Show donation settings\n"
                "  full           - Print info in format 'status protocol pubkey vin lastseen activeseconds' (can be additionally filtered, partial match)\n"
                "  lastseen       - Print timestamp of when a fundamentalnode was last seen on the network\n"
                "  pose           - Print Proof-of-Service score\n"
                "  protocol       - Print protocol of a fundamentalnode (can be additionally filtered, exact match))\n"
                "  pubkey         - Print public key associated with a fundamentalnode (can be additionally filtered, partial match)\n"
                "  rank           - Print rank of a fundamentalnode based on current block\n"
                "  status         - Print fundamentalnode status: ENABLED / EXPIRED / VIN_SPENT / REMOVE / POS_ERROR (can be additionally filtered, partial match)\n"
                "  vin            - Print vin associated with a fundamentalnode (can be additionally filtered, partial match)\n"
                "  votes          - Print all fundamentalnode votes for a Bitsend initiative (can be additionally filtered, partial match)\n"
                );
    }

    Object obj;
    if (strMode == "rank") {
        std::vector<pair<int, CFundamentalnode> > vFundamentalnodeRanks = fnmanager.GetFundamentalnodeRanks(pindexBest->nHeight);
        BOOST_FOREACH(PAIRTYPE(int, CFundamentalnode)& s, vFundamentalnodeRanks) {
            std::string strAddr = s.second.addr.ToString();
            if(strFilter !="" && strAddr.find(strFilter) == string::npos) continue;
            obj.push_back(Pair(strAddr,       s.first));
        }
    } else {
        std::vector<CFundamentalnode> vFundamentalnodes = fnmanager.GetFullFundamentalnodeVector();
        BOOST_FOREACH(CFundamentalnode& mn, vFundamentalnodes) {
            std::string strAddr = mn.addr.ToString();
            if (strMode == "activeseconds") {
                if(strFilter !="" && strAddr.find(strFilter) == string::npos) continue;
                obj.push_back(Pair(strAddr,       (int64_t)(mn.lastTimeSeen - mn.sigTime)));
            } else if (strMode == "donation") {
                CTxDestination address1;
                ExtractDestination(mn.donationAddress, address1);
                CBitcoinAddress address2(address1);

                if(strFilter !="" && address2.ToString().find(strFilter) == string::npos &&
                    strAddr.find(strFilter) == string::npos) continue;

                std::string strOut = "";

                if(mn.donationPercentage != 0){
                    strOut = address2.ToString().c_str();
                    strOut += ":";
                    strOut += boost::lexical_cast<std::string>(mn.donationPercentage);
                }
                obj.push_back(Pair(strAddr,       strOut.c_str()));
            } else if (strMode == "full") {
                CScript pubkey;
                pubkey.SetDestination(mn.pubkey.GetID());
                CTxDestination address1;
                ExtractDestination(pubkey, address1);
                CBitcoinAddress address2(address1);

                std::ostringstream addrStream;
                addrStream << setw(21) << strAddr;

                std::ostringstream stringStream;
                stringStream << setw(10) <<
                               mn.Status() << " " <<
                               mn.protocolVersion << " " <<
                               address2.ToString() << " " <<
                               mn.vin.prevout.hash.ToString() << " " <<
                               mn.lastTimeSeen << " " << setw(8) <<
                               (mn.lastTimeSeen - mn.sigTime);
                std::string output = stringStream.str();
                stringStream << " " << strAddr;
                if(strFilter !="" && stringStream.str().find(strFilter) == string::npos &&
                        strAddr.find(strFilter) == string::npos) continue;
                obj.push_back(Pair(addrStream.str(), output));
            } else if (strMode == "lastseen") {
                if(strFilter !="" && strAddr.find(strFilter) == string::npos) continue;
                obj.push_back(Pair(strAddr,       (int64_t)mn.lastTimeSeen));
            } else if (strMode == "protocol") {
                if(strFilter !="" && strFilter != boost::lexical_cast<std::string>(mn.protocolVersion) &&
                    strAddr.find(strFilter) == string::npos) continue;
                obj.push_back(Pair(strAddr,       (int64_t)mn.protocolVersion));
            } else if (strMode == "pubkey") {
                CScript pubkey;
                pubkey.SetDestination(mn.pubkey.GetID());
                CTxDestination address1;
                ExtractDestination(pubkey, address1);
                CBitcoinAddress address2(address1);

                if(strFilter !="" && address2.ToString().find(strFilter) == string::npos &&
                    strAddr.find(strFilter) == string::npos) continue;
                obj.push_back(Pair(strAddr,       address2.ToString().c_str()));
            } else if (strMode == "pose") {
                if(strFilter !="" && strAddr.find(strFilter) == string::npos) continue;
                std::string strOut = boost::lexical_cast<std::string>(mn.nScanningErrorCount);
                obj.push_back(Pair(strAddr,       strOut.c_str()));
            } else if(strMode == "status") {
                std::string strStatus = mn.Status();
                if(strFilter !="" && strAddr.find(strFilter) == string::npos && strStatus.find(strFilter) == string::npos) continue;
                obj.push_back(Pair(strAddr,       strStatus.c_str()));
            } else if (strMode == "vin") {
                if(strFilter !="" && mn.vin.prevout.hash.ToString().find(strFilter) == string::npos &&
                    strAddr.find(strFilter) == string::npos) continue;
                obj.push_back(Pair(strAddr,       mn.vin.prevout.hash.ToString().c_str()));
            } else if(strMode == "votes"){
                std::string strStatus = "ABSTAIN";

                //voting lasts 7 days, ignore the last vote if it was older than that
                if((GetAdjustedTime() - mn.lastVote) < (60*60*8))
                {
                    if(mn.nVote == -1) strStatus = "NAY";
                    if(mn.nVote == 1) strStatus = "YEA";
                }

                if(strFilter !="" && (strAddr.find(strFilter) == string::npos && strStatus.find(strFilter) == string::npos)) continue;
                obj.push_back(Pair(strAddr,       strStatus.c_str()));
            }
        }
    }
    return obj;

}
