#include "walletserversession.h"
#include "util.h"
#include "main.h"
#include "bitcoinrpc.h"
#include "json/json_spirit.h"

#include <boost/filesystem.hpp>

WalletServerSession::WalletServerSession(std::string newSessionId, SynchronisedCommandQueue<Command>* queue)
{
    sessionId = newSessionId;
    m_queue = queue;
    walletOpen = false;
    walletServer.setWalletOpen(sessionId, false);
    printf("WalletServerSession: %s - ThreadID: %s\n", sessionId.c_str(), boost::lexical_cast<std::string>(boost::this_thread::get_id()).c_str());
}

WalletServerSession::~WalletServerSession()
{
}

void WalletServerSession::NotifyBlocksChanged()
{
    if(walletOpen)
    {
        // load the new block from disk
        CBlock block;
        CBlockIndex* pblockindex = mapBlockIndex[hashBestChain];
        block.ReadFromDisk(pblockindex);
        // loop over all transactions in new block

        BOOST_FOREACH(CTransaction& tx, block.vtx)
        {
            clientWallet->AddToWalletIfInvolvingMe(tx.GetHash(), tx, &block, true);
        }
        // update block height
        LOCK(clientWallet->cs_wallet);
        clientWallet->SetBestChain(CBlockLocator(pblockindex));
        nWalletDBUpdated++;
    }
}

void WalletServerSession::NotifyTransactionChanged(CWallet *wallet, const uint256 &hash, ChangeType status)
{
    if(walletOpen && status == CT_NEW)
    {
        CWalletTx tx;
        clientWallet->GetTransaction(hash, tx);
        int64 nFee;
        std::string strSentAccount;
        list<pair<CTxDestination, int64> > listReceived;
        list<pair<CTxDestination, int64> > listSent;
        tx.GetAmounts(listReceived, listSent, nFee, strSentAccount);
        printf("WalletServerSession - NotifyTransactionChanged: received: %lu sent: %lu\n", listReceived.size(), listSent.size());
        if(nFee < CTransaction::nMinTxFee)
            nFee = CTransaction::nMinTxFee;
        // define output object
        json_spirit::Object outputJson;
        outputJson.push_back(json_spirit::Pair("sessionid", sessionId));
        outputJson.push_back(json_spirit::Pair("correlationid", sessionId));
        outputJson.push_back(json_spirit::Pair("command", "transaction"));
        // add wallet info
        json_spirit::Object result;
        result.push_back(json_spirit::Pair("errorCode", 0));
        result.push_back(json_spirit::Pair("errorMessage", ""));
        result.push_back(json_spirit::Pair("transactionid", hash.ToString()));
        result.push_back(json_spirit::Pair("transactiontime",  boost::int64_t(tx.GetTxTime())));
        // Send
        if (listSent.size() > 0 || nFee != 0)
        {
            BOOST_FOREACH(const PAIRTYPE(CTxDestination, int64)& s, listSent)
            {
                result.push_back(json_spirit::Pair("transactiontype", "SENT"));
                result.push_back(json_spirit::Pair("address", CBitcoinAddress(s.first).ToString()));
                result.push_back(json_spirit::Pair("amount", FormatMoney(s.second,false)));
                result.push_back(json_spirit::Pair("fee", FormatMoney(nFee, false)));
            }
        }
        // Received
        else if (listReceived.size() > 0)
        {
            BOOST_FOREACH(const PAIRTYPE(CTxDestination, int64)& r, listReceived)
            {
                result.push_back(json_spirit::Pair("transactiontype", "RECEIVED"));
                result.push_back(json_spirit::Pair("address", CBitcoinAddress(r.first).ToString()));
                result.push_back(json_spirit::Pair("amount", FormatMoney(r.second, false)));
            }
        }
        outputJson.push_back(json_spirit::Pair("result", result));
        // send result to out queue
        WalletServerSession::sendMessageToQueue(outputJson);
    }
}

void WalletServerSession::sendMessageToQueue(json_spirit::Object outputJson)
{
    try {
        // construct a headermap
        STOMP::hdrmap headers;
        headers["Content-Type"] = string("application/json");
        // add an outgoing message to the topic
        std::string body = json_spirit::write(outputJson);
        printf("WalletServerSession %s JSON: %s\n", sessionId.c_str(), body.c_str());
        session_stomp_client->send(WalletServer::server_out_queue, headers, body);
    }
    catch (std::exception& e)
    {
        cerr << "Error in BoostStomp: " << e.what() << "\n";
    }
    // update last commandtime
    walletServer.setLastCommandTime(sessionId, (int)time(NULL));
}

void WalletServerSession::executeWalletCommand(Command data)
{
    // Double check if the command is for this session before execute
    if(sessionId.compare(data.sessionId) == 0)
    {
        if(data.command.compare("openwallet") == 0)
        {
            if(!walletOpen)
                WalletServerSession::openWallet(data);
            else
                walletServer.sendErrorMessage(108, sessionId, data.correlationId, data.command);
        }
        else if(data.command.compare("closewallet") == 0)
        {
            WalletServerSession::closeWallet(data);
        }
        else if(data.command.compare("getinfo") == 0)
        {
            if(walletOpen)
                WalletServerSession::getWalletInfo(data);
            else
                walletServer.sendErrorMessage(109, sessionId, data.correlationId, data.command);
        }
        else if(data.command.compare("getaddresslist") == 0)
        {
            if(walletOpen)
                WalletServerSession::getAddressBook(data);
            else
                walletServer.sendErrorMessage(109, sessionId, data.correlationId, data.command);
        }
        else if(data.command.compare("sendtoaddress") == 0)
        {
            if(walletOpen)
                WalletServerSession::sendCoinsToAddress(data);
            else
                walletServer.sendErrorMessage(109, sessionId, data.correlationId, data.command);
        }
        else if(data.command.compare("closesession") == 0)
        {
            WalletServerSession::closeSession();
        }
        else
        {
            printf("WalletServerSession command not found: %s\n", data.command.c_str());
            walletServer.sendErrorMessage(110, sessionId, data.correlationId, data.command);
        }
    }
}

void WalletServerSession::closeWalletIfOpen()
{
    printf("Close WalletServerSession sessionId: %s - Current WalletId: %s - Current Thread ID: %s\n",
           sessionId.c_str(), walletId.c_str(), boost::lexical_cast<std::string>(boost::this_thread::get_id()).c_str());
    if(walletOpen)
    {
        // detach and close wallet file
        printf("%s detach\n", clientWallet->strWalletFile.c_str());
        bitdb.dbenv.lsn_reset(clientWallet->strWalletFile.c_str(), 0);
        printf("%s closed\n", clientWallet->strWalletFile.c_str());
        // Create close wallet result message
        json_spirit::Object outputJson;
        outputJson.push_back(json_spirit::Pair("sessionid", sessionId));
        outputJson.push_back(json_spirit::Pair("command", "closewallet"));
        json_spirit::Object result;
        result.push_back(json_spirit::Pair("errorCode", 0));
        result.push_back(json_spirit::Pair("errorMessage", ""));
        outputJson.push_back(json_spirit::Pair("result", result));
        // send result to out queue
        WalletServerSession::sendMessageToQueue(outputJson);
        // Close Queue connection
        printf("WalletServerSession sessionId: %s stop stomp client.\n", this->sessionId.c_str());
        this->session_stomp_client->stop();
        printf("WalletServerSession sessionId: %s delete stomp client.\n", this->sessionId.c_str());
        delete this->session_stomp_client;
        printf("WalletServerSession sessionId: %s wallet close finished.\n", sessionId.c_str());
        walletOpen = false;
        walletServer.setWalletOpen(sessionId, false);
    }
}

void WalletServerSession::openWallet(Command data)
{
    printf("WalletServerSession sessionId: %s Execute %s\n", this->sessionId.c_str(), data.command.c_str());
    // define output object
    json_spirit::Object outputJson;
    outputJson.push_back(json_spirit::Pair("sessionid", sessionId));
    outputJson.push_back(json_spirit::Pair("correlationid", data.correlationId));
    outputJson.push_back(json_spirit::Pair("command", "openWallet"));
    // get WalletId from command arguments
    std::string openWalletId = "";
    std::map<std::string, std::string>::iterator it;
    it = data.arguments.find("walletid");
    if(it != data.arguments.end())
    {
        openWalletId = it->second;
    }
    if(openWalletId.length() == 0)
    {
        // send error message
        json_spirit::Object result;
        result.push_back(json_spirit::Pair("errorCode", 101));
        result.push_back(json_spirit::Pair("errorMessage", "No Wallet ID supplied in arguments array."));
        outputJson.push_back(json_spirit::Pair("result", result));
    }
    else
    {
        // verify walletId/accountId combination is valid
        bool commandValid = walletServer.isWalletAccountValid(openWalletId, data.accountId);
        if(commandValid)
        {
            // check wallet file is available on filesystem
            std::string walletFilenameString = openWalletId + ".dat";
            this->walletFilename = walletServer.getWalletServerPath() / walletFilenameString;
            if(boost::filesystem::exists(this->walletFilename))
            {
                printf("Wallet File Exists so open the wallet: %s\n", this->walletFilename.string().c_str());
                // open wallet
                int64 nStart = GetTimeMillis();
                bool fFirstRun = true;
                clientWallet = new CWallet(this->walletFilename.string());
                DBErrors nLoadWalletRet = clientWallet->LoadWallet(fFirstRun);
                std::ostringstream strLoadResult;
                if (nLoadWalletRet != DB_LOAD_OK)
                {
                    if (nLoadWalletRet == DB_CORRUPT)
                        strLoadResult << "Error loading wallet file: Wallet corrupted";
                    else if (nLoadWalletRet == DB_NONCRITICAL_ERROR)
                    {
                        strLoadResult << "Warning: error reading wallet file! All keys read correctly, but transaction data"
                                     " or address book entries might be missing or incorrect.";
                    }
                    else if (nLoadWalletRet == DB_TOO_NEW)
                        strLoadResult << "Error loading wallet file: Wallet requires newer version of CasinoCoin";
                    else if (nLoadWalletRet == DB_NEED_REWRITE)
                    {
                        strLoadResult << "Wallet needed to be rewritten: restart CasinoCoin to complete";
                    }
                    else
                        strLoadResult << "Error loading " << walletFilenameString.c_str();
                    // load wallet result
                    printf("WalletServerSession - Load Wallet result: %s\n", strLoadResult.str().c_str());
                    // send error message
                    json_spirit::Object result;
                    result.push_back(json_spirit::Pair("errorCode", 103));
                    std::string openWalletError = std::string("Open Wallet Error: ");
                    openWalletError.append(strLoadResult.str());
                    result.push_back(json_spirit::Pair("errorMessage", openWalletError));
                    outputJson.push_back(json_spirit::Pair("result", result));
                }
                else
                {
                    // connect to new Transaction Notifications
                    printf("WalletServerSession -  Connect to new Transaction Notification\n");
                    clientWallet->NotifyTransactionChanged.connect(boost::bind(&WalletServerSession::NotifyTransactionChanged, this, _1, _2, _3));
                    // set wallet open in session
                    walletOpen = true;
                    walletServer.setWalletOpen(sessionId, true);
                    walletServer.setLastCommandTime(sessionId, (int)time(NULL));
                    // get blockchain best block
                    CBlockIndex *pindexBlockchain = pindexBest;
                    // get wallet genesis block
                    CBlockIndex *pindexWalletGenesisBlock;
                    CBlockLocator genesisLocator;
                    if(clientWallet->GetWalletGenesisBlock(genesisLocator))
                        pindexWalletGenesisBlock = genesisLocator.GetBlockIndex();
                    else
                        pindexWalletGenesisBlock = pindexGenesisBlock;
                    // get wallet best block
                    CBlockLocator walletLocator;
                    CBlockIndex *pindexWallet;
                    if (clientWallet->GetBestChain(walletLocator))
                        pindexWallet = walletLocator.GetBlockIndex();
                    else
                        pindexWallet = pindexWalletGenesisBlock;
                    printf("WalletServerSession - pindexBlockchain: %i  pindexWallet: %i pindexWalletGenesisBlock: %i\n", pindexBlockchain->nHeight, pindexWallet->nHeight, pindexWalletGenesisBlock->nHeight);
                    if(pindexBlockchain->nHeight > pindexWallet->nHeight)
                    {
                        CBlockIndex *pindexRescan = pindexWallet;
                        if(pindexWalletGenesisBlock->nHeight > pindexRescan->nHeight)
                            pindexRescan = pindexWalletGenesisBlock;
                        printf("WalletServerSession - Rescanning last %i blocks (from block %i) for wallet %s\n", pindexBest->nHeight - pindexRescan->nHeight, pindexRescan->nHeight, walletFilenameString.c_str());
                        nStart = GetTimeMillis();
                        clientWallet->ScanForWalletTransactions(pindexRescan);
                        printf("WalletServerSession - rescan      %15"PRI64d"ms\n", GetTimeMillis() - nStart);
                        clientWallet->SetBestChain(CBlockLocator(pindexBest));
                        nWalletDBUpdated++;
                    }
                    // Add wallet transactions that aren't already in a block to mapTransactions
                    clientWallet->ReacceptWalletTransactions();
                    // set walletId in session
                    walletId = openWalletId;
                    // send result message to out queue
                    json_spirit::Object result;
                    result.push_back(json_spirit::Pair("errorCode", 0));
                    result.push_back(json_spirit::Pair("errorMessage", ""));
                    outputJson.push_back(json_spirit::Pair("result", result));
                 }
            }
            else
            {
                // send error message
                json_spirit::Object result;
                result.push_back(json_spirit::Pair("errorCode", 103));
                std::string openWalletFileError = "Wallet file " + this->walletFilename.string() + " does not exist on WalletServer.";
                result.push_back(json_spirit::Pair("errorMessage", openWalletFileError));
                outputJson.push_back(json_spirit::Pair("result", result));
            }
        }
        else
        {
            // send error message
            json_spirit::Object result;
            result.push_back(json_spirit::Pair("errorCode", 102));
            result.push_back(json_spirit::Pair("errorMessage", "Invalid Account ID for given Wallet ID."));
            outputJson.push_back(json_spirit::Pair("result", result));
        }
    }
    // send result to out queue
    WalletServerSession::sendMessageToQueue(outputJson);
}

void WalletServerSession::closeWallet(Command data)
{
    printf("WalletServerSession sessionId: %s Execute closeWallet\n", sessionId.c_str());
    closeWalletIfOpen();
}

json_spirit::Value WalletServerSession::getWalletInfo(Command data)
{
    printf("WalletServerSession sessionId: %s Execute getWalletInfo\n", this->sessionId.c_str());
    // define output object
    json_spirit::Object outputJson;
    outputJson.push_back(json_spirit::Pair("sessionid", sessionId));
    outputJson.push_back(json_spirit::Pair("correlationid", data.correlationId));
    outputJson.push_back(json_spirit::Pair("command", "getinfo"));
    // add wallet info
    json_spirit::Object result;
    result.push_back(json_spirit::Pair("errorCode", 0));
    result.push_back(json_spirit::Pair("errorMessage", ""));
    result.push_back(json_spirit::Pair("version",(int)CLIENT_VERSION));
    result.push_back(json_spirit::Pair("protocolversion",(int)PROTOCOL_VERSION));
    result.push_back(json_spirit::Pair("blocks", (int)nBestHeight));
    result.push_back(json_spirit::Pair("coinsupply", FormatMoney(GetTotalCoinSupply(nBestHeight,false),false)));
    result.push_back(json_spirit::Pair("timeoffset", (boost::int64_t)GetTimeOffset()));
    result.push_back(json_spirit::Pair("connections",(int)vNodes.size()));
    result.push_back(json_spirit::Pair("difficulty", (double)GetDifficulty()));
    if (clientWallet) {
        result.push_back(json_spirit::Pair("walletversion", clientWallet->GetVersion()));
        result.push_back(json_spirit::Pair("defaultaddress", CBitcoinAddress(clientWallet->vchDefaultKey.GetID()).ToString()));
        result.push_back(json_spirit::Pair("balance", FormatMoney(clientWallet->GetBalance(),false)));
        result.push_back(json_spirit::Pair("unconfirmedbalance", FormatMoney(clientWallet->GetUnconfirmedBalance(),false)));
        balancesMapType balances = clientWallet->GetAddressBalances();
        json_spirit::Array addressBalanceArray;
        BOOST_FOREACH(balancesMapType::value_type balance, balances)
        {
            json_spirit::Object addressInfo;
            addressInfo.push_back(json_spirit::Pair("address",CBitcoinAddress(balance.first).ToString()));
            addressInfo.push_back(json_spirit::Pair("balance", ValueFromAmount(balance.second)));
            addressBalanceArray.push_back(addressInfo);
        }
        result.push_back(json_spirit::Pair("addresses", addressBalanceArray));
        result.push_back(json_spirit::Pair("keypoololdest",(boost::int64_t)clientWallet->GetOldestKeyPoolTime()));
        result.push_back(json_spirit::Pair("keypoolsize", (int)clientWallet->GetKeyPoolSize()));
    }
    result.push_back(json_spirit::Pair("paytxfee", FormatMoney(CTransaction::nMinTxFee, false)));
    result.push_back(json_spirit::Pair("mininput", FormatMoney(nMinimumInputValue, false)));
    if (clientWallet && clientWallet->IsCrypted())
        result.push_back(json_spirit::Pair("unlocked_until", (boost::int64_t)nWalletUnlockTime));
    outputJson.push_back(json_spirit::Pair("result", result));
    // send result to out queue
    WalletServerSession::sendMessageToQueue(outputJson);
    return outputJson;
}

json_spirit::Value WalletServerSession::getAddressBook(Command data)
{
    printf("WalletServerSession sessionId: %s Execute getAddressBook\n", sessionId.c_str());
    balancesMapType balances = clientWallet->GetAddressBalances();
    // define output object
    json_spirit::Object outputJson;
    outputJson.push_back(json_spirit::Pair("sessionid", sessionId));
    outputJson.push_back(json_spirit::Pair("correlationid", data.correlationId));
    outputJson.push_back(json_spirit::Pair("command", data.command));
    // add wallet info
    json_spirit::Object result;
    result.push_back(json_spirit::Pair("errorCode", 0));
    result.push_back(json_spirit::Pair("errorMessage", ""));
    json_spirit::Array addressBalanceArray;
    BOOST_FOREACH(balancesMapType::value_type balance, balances)
    {
        json_spirit::Object addressInfo;
        addressInfo.push_back(json_spirit::Pair("address",CBitcoinAddress(balance.first).ToString()));
        addressInfo.push_back(json_spirit::Pair("balance", ValueFromAmount(balance.second)));
        addressBalanceArray.push_back(addressInfo);
    }
    result.push_back(json_spirit::Pair("addresses", addressBalanceArray));
    outputJson.push_back(json_spirit::Pair("result", result));
    // send result to out queue
    WalletServerSession::sendMessageToQueue(outputJson);
    return outputJson;
}

json_spirit::Value WalletServerSession::sendCoinsToAddress(Command data)
{
    std::string strAddress = "";
    std::string strAmount = "";
    std::string strComment = "";
    // loop over arguments and get the values
    std::map<std::string, std::string>::iterator argit;
    for(argit = data.arguments.begin(); argit != data.arguments.end(); argit++) {
        if(argit->first.compare("address")==0)
            strAddress = argit->second;
        else if(argit->first.compare("amount")==0)
            strAmount = argit->second;
        else if(argit->first.compare("comment")==0)
            strComment = argit->second;
    }
    printf("WalletServerSession: %s sendCoinsToAddress: %s Amount: %s\n", sessionId.c_str(), strAddress.c_str(), strAmount.c_str());
    // define output object
    json_spirit::Object outputJson;
    outputJson.push_back(json_spirit::Pair("sessionid", sessionId));
    outputJson.push_back(json_spirit::Pair("correlationid", data.correlationId));
    outputJson.push_back(json_spirit::Pair("command", data.command));
    // create coin address
    CBitcoinAddress address(strAddress);
    if (!address.IsValid())
    {
        // send error message
        json_spirit::Object result;
        result.push_back(json_spirit::Pair("errorCode", 111));
        result.push_back(json_spirit::Pair("errorMessage", "Invalid CasinoCoin address."));
        outputJson.push_back(json_spirit::Pair("result", result));
        // send result to out queue
        WalletServerSession::sendMessageToQueue(outputJson);
        return outputJson;
    }
    // Amount
    int64 nAmount;
    ParseMoney(strAmount, nAmount);
    if (nAmount <= 0)
    {
        // send error message
        json_spirit::Object result;
        result.push_back(json_spirit::Pair("errorCode", 112));
        result.push_back(json_spirit::Pair("errorMessage", "Amount of coins to sent to address must be greater than 0."));
        outputJson.push_back(json_spirit::Pair("result", result));
        // send result to out queue
        WalletServerSession::sendMessageToQueue(outputJson);
        return outputJson;
    }
    // Wallet comments
    CWalletTx wtx;
    if(!strComment.empty())
        wtx.mapValue["comment"] = strComment;
    // check if wallet is locked
//    if (pwalletMain->IsLocked())
//        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");
    // execute send
    string strError = clientWallet->SendMoneyToDestination(address.Get(), nAmount, wtx);
    if (strError != "")
    {
        // send error message
        json_spirit::Object result;
        result.push_back(json_spirit::Pair("errorCode", 10));
        result.push_back(json_spirit::Pair("errorMessage", "Error sending coins: " + strError));
        outputJson.push_back(json_spirit::Pair("result", result));
    }
    else
    {
        json_spirit::Object result;
        result.push_back(json_spirit::Pair("errorCode", 0));
        result.push_back(json_spirit::Pair("errorMessage", ""));
        result.push_back(json_spirit::Pair("txid", wtx.GetHash().GetHex()));
        outputJson.push_back(json_spirit::Pair("result", result));
    }
    // send result to out queue
    WalletServerSession::sendMessageToQueue(outputJson);
    return outputJson;
}

void WalletServerSession::closeSession()
{
    printf("WalletServerSession sessionId: %s Execute closeSession\n", sessionId.c_str());
    closeWalletIfOpen();
    // remove wallet object
    if(clientWallet != NULL)
        delete clientWallet;
    // set shutdownComplete
    shutdownComplete = true;
    printf("WalletServerSession sessionId: %s Session Close Finished!\n", sessionId.c_str());
}
