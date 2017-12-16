//#include "signhelper_mn.h"
#include "fn-manager.h"
#include "fundamentalnode.h"
#include "fn-activity.h"

/*
Global
*/
int RequestedFundamentalNodeList = 0;


void ThreadBitPool()
{
    if(fProMode) return; //disable all Darksend/Fundamentalnode related functionality

    // Make this thread recognisable as the wallet flushing thread
    RenameThread("b3coin-bitpool");

    unsigned int c = 0;
    std::string errorMessage;

    while (true)
    {
        c++;

        MilliSleep(1000);
        //LogPrintf("ThreadBitPool::check timeout\n");

        

        if(c % (60*60) == 0)
        {
            LOCK(cs_main);
            /*
                cs_main is required for doing CFundamentalnode.Check because something
                is modifying the coins view without a mempool lock. It causes
                segfaults from this code without the cs_main lock.
            */
            fnmanager.CheckAndRemove();
            fnmanager.ProcessFundamentalnodeConnections();
            fundamentalnodePayments.CleanPaymentList();
            
        }

        if(c % FUNDAMENTALNODE_PING_SECONDS == 0) activeFundamentalnode.ManageStatus();

        if(c % FUNDAMENTALNODES_DUMP_SECONDS == 0) DumpFundamentalnodes();

        //try to sync the Fundamentalnode list and payment list every 5 seconds from at least 3 nodes
        if(c % 5 == 0 && RequestedFundamentalNodeList < 3){
            bool fIsInitialDownload = IsInitialBlockDownload();
            if(!fIsInitialDownload) {
                LOCK(cs_vNodes);
                BOOST_FOREACH(CNode* pnode, vNodes)
                {
                    if ( true/*pnode->nVersion >= MIN_PEER_PROTO_VERSION*/ ) {

                        //keep track of who we've asked for the list
                        if(pnode->HasFulfilledRequest("fnsync")) continue;
                        pnode->FulfilledRequest("fnsync");

                        LogPrintf("Successfully synced, asking for Fundamentalnode list and payment list\n");

                        //request full fn list only if Fundamentalnodes.dat was updated quite a long time ago
                        fnmanager.FnlUpdate(pnode);

                        pnode->PushMessage("fnget"); //sync payees
                        pnode->PushMessage("getsporks"); //get current network sporks
                        RequestedFundamentalNodeList++;
                    }
                }
            }
        }
    }
}
