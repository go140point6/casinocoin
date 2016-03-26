#include <fstream>
#include "walletserver.h"
#include "walletserversession.h"
#include "util.h"
#include "bitcoinrpc.h"
#include "wallet.h"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/serialization/map.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>

#include "json/json_spirit.h"
#include "json/json_spirit_writer_template.h"

using namespace std;
using namespace boost;
using namespace STOMP;

// Wallets Path
static boost::filesystem::path walletServerPath;

bool isServerRunning = false;

// in memory key/value store for sessions
static sessionKeyValueType sessions;
// in memory key/value store for session command queues
static sessionCommandQueueType sessionQueues;
// key string = accountId
//static std::map<std::string, WalletSession> walletSessions;
// persisted key/value stores for wallets and server secrets
// key string = walletId / value string = accountId
static keyValueType wallets;
// key string = walletId / value string = wallet server secret
static keyValueType walletServerSecrets;

// WalletServerSessions Thread Group
boost::thread_group walletServerSessionsTG;

std::string WalletServer::block_notifications_topic = std::string("/topic/Blocks");
std::string WalletServer::server_in_queue = "/queue/ServerInQueue";
std::string WalletServer::server_out_queue = "/queue/ServerOutQueue";
std::string WalletServer::stomp_host = GetArg("-activemqstomphost", "localhost");
int WalletServer::stomp_port = GetArg("activemqstompport", 61613);

// Constructor
WalletServer::WalletServer()
{
}

void StartWalletServerThread()
{
    // Make this thread recognisable as the wallet server thread
    RenameThread("casinocoin-walletserver");
    printf("CasinoCoin WalletServer Daemon starting\n");
    // set server to running
    isServerRunning = true;
    // create the wallet directory if it not exists
    walletServerPath  = GetDataDir() / "wallets";
    boost::filesystem::create_directories(walletServerPath);
    printf("Using wallet directory: %s\n", walletServerPath.string().c_str());
    // load wallet list and server secrets
    walletServer.loadWalletServerSecrets();
    walletServer.loadWalletList();
    // initiate a new BoostStomp client
    walletServer.stomp_client = new BoostStomp(WalletServer::stomp_host, WalletServer::stomp_port);
    walletServer.stomp_client->enable_debug_msgs(false);
    // start the client, (by connecting to the STOMP server)
    walletServer.stomp_client->start();
    // subscribe to server in queues
    walletServer.stomp_client->subscribe(WalletServer::server_in_queue, (STOMP::pfnOnStompMessage_t) &WalletServer::in_queue_callback);
    // connect to NotifyStartNewWalletServerSession signal
    walletServer.NotifyStartNewWalletServerSession.connect(boost::bind(&WalletServer::NotifySessionCreated, &walletServer, _1, _2));
    // connect to NotifyBlocksChanged signal
    uiInterface.NotifyBlocksChanged.connect(boost::bind(&WalletServer::NotifyBlocksChanged, &walletServer));
}

void StopWalletServerThread()
{
    if(isServerRunning)
    {
        // stop all wallet sessions
        printf("StopWalletServerThread - Sending shutdown signal to all sessions\n");
        for (sessionKeyValueType::iterator it=sessions.begin(); it!=sessions.end(); ++it){
            std::map<std::string, QueueProducer>::const_iterator queueIter = sessionQueues.find(it->second.sessionId);
            if(queueIter != sessionQueues.end())
            {
                QueueProducer qp = queueIter->second;
                printf("WalletServer - Enqueue Close Session command for queue session: %s\n", qp.getSessionId().c_str());
                std::map<std::string, std::string> argumentsMap;
                Command cmd = {it->second.email, it->second.sessionId, "correlationId", "closesession",argumentsMap};
                qp.Enqueue(cmd);
            }
        }
        // remove sessions
        sessions.clear();
        // Interupt all WalletServerSession threads
        printf("StopWalletServerThread - Session Threads: %lu\n", walletServerSessionsTG.size());
        printf("StopWalletServerThread - Interrupt All\n");
        walletServerSessionsTG.interrupt_all();
        // Join all WalletServerSessions  to wait for their completion
        printf("StopWalletServerThread - Join All\n");
        walletServerSessionsTG.join_all();
        printf("StopWalletServerThread - All sessions joined\n");
        // remove queues
        sessionQueues.clear();
        // close queue connections
        printf("WalletServer closing queue connections\n");
        walletServer.stomp_client->stop();
        delete walletServer.stomp_client;
        // save wallet list and server secrets to file
        walletServer.saveWalletList();
        walletServer.saveWalletServerSecrets();
        // stop server thread
        isServerRunning = false;
        printf("WalletServer STOPPED\n");
    }
}

void flushWalletsThread()
{
    printf("WalletServer - flushWalletsThread");
//    TRY_LOCK(bitdb.cs_db,lockDb);
//    if (lockDb)
//    {
//        // Don't do this if any databases are in use
//        int nRefCount = 0;
//        map<string, int>::iterator mi = bitdb.mapFileUseCount.begin();
//        while (mi != bitdb.mapFileUseCount.end())
//        {
//            nRefCount += (*mi).second;
//            mi++;
//        }
//        if (nRefCount == 0)
//        {
//            boost::this_thread::interruption_point();
//            map<string, int>::iterator mi = bitdb.mapFileUseCount.find(clientWallet->strWalletFile);
//            if (mi != bitdb.mapFileUseCount.end())
//            {
//                int64 nStart = GetTimeMillis();
//                // Flush wallet.dat so it's self contained
//                bitdb.CloseDb(clientWallet->strWalletFile);
//                bitdb.CheckpointLSN(clientWallet->strWalletFile);
//                bitdb.mapFileUseCount.erase(mi++);
//            }
//        }
//    }
//    else
//        printf("WalletServerSession - Could not get lock to execute Flush for session %s\n",sessionId.c_str());
}

// Handler for NotifyBlocksChanged signal
void WalletServer::NotifyBlocksChanged()
{
    printf("CasinoCoin WalletServer Received NotifyBlocksChanged Signal: %i BlockHash: %s\n", nBestHeight, hashBestChain.ToString().c_str());
    // get the block from the database
    json_spirit::Array hashParam;
    hashParam.push_back(hashBestChain.ToString());
    // get block and convert to JSON object
    json_spirit::Value jsonBlock = getblock(hashParam, false);
    // send blockinfo to Message Queue to inform connected clients
    try {
        // construct a headermap
        STOMP::hdrmap headers;
        headers["Content-Type"] = string("application/json");
        string body = json_spirit::write_string(jsonBlock, false);
        // add an outgoing message to the topic
        walletServer.stomp_client->send(WalletServer::block_notifications_topic, headers, body);

    }
    catch (std::exception& e)
    {
        cerr << "Error in BoostStomp: " << e.what() << "\n";
    }
}

// Handler for incomming queue messages
bool WalletServer::in_queue_callback(STOMP::Frame& _frame)
{
    std::string jsonBody = std::string(_frame.body().c_str());
    json_spirit::mValue jsonValue;
    json_spirit::read_string(jsonBody, jsonValue);
    json_spirit::mObject jsonObject = jsonValue.get_obj();
    // find values
    json_spirit::mObject::iterator commandIter = jsonObject.find("command");
    json_spirit::mObject::iterator sessionIter = jsonObject.find("sessionid");
    json_spirit::mObject::iterator accountIter = jsonObject.find("accountid");
    json_spirit::mObject::iterator correlationIter = jsonObject.find("correlationid");
    if(commandIter != jsonObject.end() &&
       sessionIter != jsonObject.end() &&
       accountIter != jsonObject.end() &&
       correlationIter != jsonObject.end())
    {
        std::string command = commandIter->second.get_str();
        std::string sessionId = sessionIter->second.get_str();
        std::string accountId = accountIter->second.get_str();
        std::string correlationId = correlationIter->second.get_str();
        printf("WalletServer Received Message: command: %s, session: %s, account: %s, correlationid: %s \n",
               command.c_str(), sessionId.c_str(), accountId.c_str(), correlationId.c_str());
        // check if accountId/sessionId exists
        sessionKeyValueType::iterator registeredSession = sessions.find(accountId);
        if(registeredSession != sessions.end())
        {
            Session msgSession = registeredSession->second;
            if(msgSession.sessionId.compare(sessionId) == 0)
            {
                if(command.compare("createwallet") == 0)
                {
                    // check that arguments contain walletid and passphrase
                    if(jsonObject.find("arguments") != jsonObject.end())
                    {
                        json_spirit::mObject argumentsObject = jsonObject.find("arguments")->second.get_obj();
                        if (argumentsObject.find("passphrase") != argumentsObject.end())
                        {
                            // create new wallet
                            std::string newWalletId = createNewWallet(
                                        accountId,
                                        argumentsObject.find("passphrase")->second.get_str()
                            );
                            printf("WalletServer created new wallet with id: %s\n", newWalletId.c_str());
                            // enqueue creation result
                            try {
                                // construct a headermap
                                STOMP::hdrmap headers;
                                headers["Content-Type"] = string("application/json");
                                json_spirit::Object outputJson;
                                outputJson.push_back(json_spirit::Pair("sessionid", sessionId));
                                outputJson.push_back(json_spirit::Pair("correlationid", correlationId));
                                outputJson.push_back(json_spirit::Pair("command", command));
                                json_spirit::Object result;
                                result.push_back(json_spirit::Pair("errorCode", 0));
                                result.push_back(json_spirit::Pair("errorMessage", ""));
                                result.push_back(json_spirit::Pair("walletid", newWalletId));
                                outputJson.push_back(json_spirit::Pair("result", result));
                                // add an outgoing message to the topic
                                std::string body = json_spirit::write(outputJson);
                                printf("WalletServer JSON: %s\n", body.c_str());
                                walletServer.stomp_client->send(WalletServer::server_out_queue, headers, body);
                            }
                            catch (std::exception& e)
                            {
                                cerr << "Error in BoostStomp: " << e.what() << "\n";
                            }
                        }
                        else
                        {
                            sendErrorMessage(104, sessionId, correlationId, command);
                        }
                    }
                }
                else
                {
                    // all other commands will be send to the WalletServerSessions
                    //
                    // Get the arguments object
                    if(jsonObject.find("arguments") != jsonObject.end())
                    {
                        json_spirit::mObject argumentsObject = jsonObject.find("arguments")->second.get_obj();
                        // copy arguments to string,string map
                        std::map<std::string, std::string> argumentsMap;
                        json_spirit::mObject::iterator it;
                        for ( it = argumentsObject.begin(); it != argumentsObject.end(); it++ )
                        {
                            argumentsMap.insert(std::make_pair(it->first, it->second.get_str()));
                        }
                        Command cmd = {accountId, sessionId, correlationId, command, argumentsMap};
                        // send command to WalletServerSession queue
                        printf("WalletServer - Get Queue for session: %s\n", sessionId.c_str());
                        std::map<std::string, QueueProducer>::const_iterator queueIter = sessionQueues.find(sessionId);
                        if(queueIter != sessionQueues.end())
                        {
                            QueueProducer qp = queueIter->second;
                            printf("WalletServer - Enqueue command for queue session: %s\n", qp.getSessionId().c_str());
                            qp.Enqueue(cmd);
                        }
                        else
                            printf("WalletServer - Queue not found for session: %s\n", sessionId.c_str());
                    }
                    else
                    {
                        // Construct and send error message
                        sendErrorMessage(100, sessionId, correlationId, command);
                    }
                }
            }
            else
                sendErrorMessage(105, sessionId, correlationId, command);

        }
        else
            sendErrorMessage(106, sessionId, correlationId, command);
    }
    else
        printf("WalletServer could not parse message. Either SessionId, AccountId, CorrelationId or Command is missing");
    // processing complete
    return(true); // return false if we want to disacknowledge the frame (send NACK instead of ACK)
}

bool WalletServer::out_queue_callback(STOMP::Frame& _frame)
{
    printf("out_queue_callback: %s", _frame.body().c_str());
    return(true); // return false if we want to disacknowledge the frame (send NACK instead of ACK)
}

// Handler for NotifySessionCreated signal
void WalletServer::NotifySessionCreated(std::string accountId, Session session)
{
    // save session in memory
    sessions.insert(std::make_pair(accountId, session));
    Session &newSession = sessions.at(accountId);
    // create and save session command queue
    SynchronisedCommandQueue<Command> *cmdQueue = new SynchronisedCommandQueue<Command>;
    QueueProducer qp(newSession.sessionId, cmdQueue);
    sessionQueues.insert(std::make_pair(newSession.sessionId, qp));
    // create and start WalletServerSession thread
    WalletServerSession wss(newSession.sessionId, cmdQueue);
    walletServerSessionsTG.create_thread(wss);
}

// load wallet server secret map from filesystem
void WalletServer::loadWalletServerSecrets()
{
    // load existing walletserver.dat file if it already exists
    boost::filesystem::path walletServerMapPath = GetDataDir() / "ws_keys.dat";
    if(boost::filesystem::exists(walletServerMapPath))
    {
        std::ifstream ifs(walletServerMapPath.string().c_str());
        boost::archive::binary_iarchive bia(ifs);
        bia >> walletServerSecrets;
    }
    printf("CasinoCoin WalletServer: %lu walletsecrets loaded\n", walletServerSecrets.size());
}

// save wallet server secret map to filesystem
void WalletServer::saveWalletServerSecrets()
{
    boost::filesystem::path walletServerMapPath = GetDataDir() / "ws_keys.dat";
    std::ofstream ofs(walletServerMapPath.string().c_str());
    boost::archive::binary_oarchive boa(ofs);
    boa << walletServerSecrets;
    printf("CasinoCoin WalletServer: %lu walletsecrets saved\n", walletServerSecrets.size());
}

// load wallet list map from filesystem
void WalletServer::loadWalletList()
{
    // load existing walletlist.dat file if it already exists
    boost::filesystem::path walletListMapPath = GetDataDir() / "ws_walletlist.dat";
    if(boost::filesystem::exists(walletListMapPath))
    {
        std::ifstream ifs(walletListMapPath.string().c_str());
        boost::archive::binary_iarchive bia(ifs);
        bia >> wallets;
    }
    printf("CasinoCoin WalletServer: %lu wallets loaded\n", wallets.size());
}

// save wallet list map to filesystem
void WalletServer::saveWalletList()
{
    boost::filesystem::path walletListMapPath = GetDataDir() / "ws_walletlist.dat";
    std::ofstream ofs(walletListMapPath.string().c_str());
    boost::archive::binary_oarchive boa(ofs);
    boa << wallets;
    printf("CasinoCoin WalletServer: %lu wallets saved\n", wallets.size());
}

bool WalletServer::isNewAccountId(std::string accountId)
{
    return (sessions.count(accountId) == 0);
}

sessionKeyValueType& WalletServer::getSessions()
{
    return sessions;
}

Session& WalletServer::getSession(std::string sessionId)
{
    // loop over sessions to find the session with its session id
    BOOST_FOREACH(sessionKeyValueType::value_type &session, sessions)
    {
        if(session.second.sessionId.compare(sessionId) == 0)
        {
            return session.second;
        }
    }
    // Not found so define default output object
    Session s = {"","",0,false,0};
    return s;
}

keyValueType WalletServer::getWallets()
{
    return wallets;
}

bool WalletServer::deleteSession(std::string sessionId)
{
    // loop over sessions to find the session with its session id
    BOOST_FOREACH(sessionKeyValueType::value_type &session, sessions)
    {
        if(session.second.sessionId.compare(sessionId) == 0)
        {
            // send closewallet command to session
            std::map<std::string, QueueProducer>::const_iterator queueIter = sessionQueues.find(session.second.sessionId);
            if(queueIter != sessionQueues.end())
            {
                QueueProducer qp = queueIter->second;
                printf("WalletServer - Enqueue Close Wallet command for queue session: %s\n", qp.getSessionId().c_str());
                std::map<std::string, std::string> argumentsMap;
                Command cmd = {session.second.email, session.second.sessionId, "correlationId", "closewallet",argumentsMap};
                qp.Enqueue(cmd);
            }
            // remove sessions object
            sessions.erase(session.first);

            return true;
        }
    }
    return false;
}

std::string WalletServer::createNewWallet(std::string accountId, std::string passphrase)
{
    printf("WalletServer Create new wallet for accout id: %s\n", accountId.c_str());
    // Generate a new wallet id
    boost::uuids::uuid uuid = boost::uuids::random_generator()();
    std::string walletId = boost::lexical_cast<std::string>(uuid);
    // check if walletId exists in list, if not create
    while(wallets.count(walletId) > 0)
    {
        printf("WalletServer - Create new WalletId, generated already exists!\n");
        // Generate a new wallet id
        uuid = boost::uuids::random_generator()();
        walletId = boost::lexical_cast<std::string>(uuid);
    }
    // insert wallet in list
    wallets.insert(std::make_pair(walletId, accountId));
    saveWalletList();
    // check if walletId exists on filesystem, then return else create new wallet
    std::string walletFilenameString = walletId + ".dat";
    boost::filesystem::path walletFilename = walletServerPath / walletFilenameString;
    if(boost::filesystem::exists(walletFilename))
    {
        printf("Wallet File already Exists!: %s\n", walletFilename.string().c_str());
        return "";
    }
    else
    {
        printf("Create Wallet File with name: %s\n", walletFilename.string().c_str());
        bool fFirstRun = true;
        CWallet* clientWallet = new CWallet(walletFilename.string());
        DBErrors nLoadWalletRet = clientWallet->LoadWallet(fFirstRun);
        std::ostringstream strLoadResult;
        if (nLoadWalletRet != DB_LOAD_OK)
        {
            if (nLoadWalletRet == DB_CORRUPT)
                strLoadResult << _("Error loading wallet file: Wallet corrupted") << "\n";
            else if (nLoadWalletRet == DB_NONCRITICAL_ERROR)
            {
                printf("Warning: error reading wallet file! All keys read correctly, but transaction data"
                             " or address book entries might be missing or incorrect.");
            }
            else if (nLoadWalletRet == DB_TOO_NEW)
                strLoadResult << _("Error loading wallet file: Wallet requires newer version of CasinoCoin") << "\n";
            else if (nLoadWalletRet == DB_NEED_REWRITE)
            {
                strLoadResult << _("Wallet needed to be rewritten: restart CasinoCoin to complete") << "\n";
            }
            else
                strLoadResult << _("Error loading wallet.dat") << "\n";
            // load wallet result
            printf("Load Wallet result: %s", strLoadResult.str().c_str());
            return "";
        }
        else
        {
            strLoadResult << "Wallet file succesfully loaded, Firstrun?: " << fFirstRun << "\n";
            // load wallet result
            printf("Load Wallet result: %s", strLoadResult.str().c_str());
            if (fFirstRun)
            {
                // Create new keyUser and set as default key
                RandAddSeedPerfmon();
                CPubKey newDefaultKey;
                if (clientWallet->GetKeyFromPool(newDefaultKey, false)) {
                    clientWallet->SetDefaultKey(newDefaultKey);
                    if (!clientWallet->SetAddressBookName(clientWallet->vchDefaultKey.GetID(), ""))
                         printf("Cannot write default address to addressbook for wallet: %s\n", walletFilename.string().c_str());
                }
                // set current blockindex as wallet genesis block
                clientWallet->SetWalletGenesisBlock(CBlockLocator(pindexBest));
                clientWallet->SetBestChain(CBlockLocator(pindexBest));
                nWalletDBUpdated++;
            }
            // encrypt wallet with user and server passphrase

            // close and unload wallet
            bitdb.CloseDb(walletFilename.string());
        }
        return walletId;
    }
/*

    printf("%s", strErrors.str().c_str());
    printf(" wallet      %15"PRI64d"ms\n", GetTimeMillis() - nStart);

    RegisterWallet(pwalletMain);

    CBlockIndex *pindexRescan = pindexBest;
    if (GetBoolArg("-rescan"))
        pindexRescan = pindexGenesisBlock;
    else
    {
        CWalletDB walletdb("wallet.dat");
        CBlockLocator locator;
        if (walletdb.ReadBestBlock(locator))
            pindexRescan = locator.GetBlockIndex();
        else
            pindexRescan = pindexGenesisBlock;
    }
    if (pindexBest && pindexBest != pindexRescan)
    {
        uiInterface.InitMessage(_("Rescanning..."));
        printf("Rescanning last %i blocks (from block %i)...\n", pindexBest->nHeight - pindexRescan->nHeight, pindexRescan->nHeight);
        nStart = GetTimeMillis();
        pwalletMain->ScanForWalletTransactions(pindexRescan, true);
        printf(" rescan      %15"PRI64d"ms\n", GetTimeMillis() - nStart);
        pwalletMain->SetBestChain(CBlockLocator(pindexBest));
        nWalletDBUpdated++;
    }
*/
}

bool WalletServer::isWalletAccountValid(std::string walletId, std::string accountId)
{
    keyValueType::iterator wallet = wallets.find(walletId);
    if(wallet != wallets.end())
    {
        std::string listAccountId = wallet->second;
        if(listAccountId.compare(accountId) == 0)
            return true;
        else
            return false;
    }
    else
        return false;
}

boost::filesystem::path WalletServer::getWalletServerPath()
{
    return walletServerPath;
}

// send json error message to out queue
void WalletServer::sendErrorMessage(int errorCode, std::string sessionId, std::string correlationId, std::string command)
{
    // Construct and send error message
    STOMP::hdrmap headers;
    headers["Content-Type"] = string("application/json");
    json_spirit::Object outputJson;
    outputJson.push_back(json_spirit::Pair("sessionid", sessionId));
    outputJson.push_back(json_spirit::Pair("correlationid", correlationId));
    outputJson.push_back(json_spirit::Pair("command", command));
    json_spirit::Object result;
    result.push_back(json_spirit::Pair("errorCode", errorCode));
    // define error messages
    std::string errorMessage = "There was an error executing command " + command;
    if(errorCode == 10)
        errorMessage = "There was an error executing command '" + command + "'";
    else if(errorCode == 100)
        errorMessage = "No arguments supplied for WalletServer command " + command;
    else if(errorCode == 101)
        errorMessage = "No Wallet ID supplied in arguments array.";
    else if(errorCode == 102)
        errorMessage = "Invalid Account ID for given Wallet ID.";
    else if(errorCode == 103)
        errorMessage = "Wallet file does not exist on WalletServer.";
    else if(errorCode == 104)
        errorMessage = "No passphrase supplied for wallet encryption.";
    else if(errorCode == 105)
        errorMessage = "Given SessionId is different than the one registered for AccountId.";
    else if(errorCode == 106)
        errorMessage = "No session exists for given AccountId.";
    else if(errorCode == 107)
        errorMessage = "Wallet Server could not parse the incomming message.";
    else if(errorCode == 108)
        errorMessage = "Wallet is already open.";
    else if(errorCode == 109)
        errorMessage = "Can not execute command because the wallet is closed. Please open the wallet before executing commands on it.";
    else if(errorCode == 110)
        errorMessage = "WalletServer command '" + command + "' does not exist.";
    else if(errorCode == 111)
        errorMessage = "Invalid CasinoCoin address.";
    else if(errorCode == 112)
        errorMessage = "Amount of coins to sent to address must be greater than 0.";
    // create output JSON
    result.push_back(json_spirit::Pair("errorMessage", errorMessage));
    outputJson.push_back(json_spirit::Pair("result", result));
    // add an outgoing message to the topic
    std::string body = json_spirit::write(outputJson);
    printf("WalletServer JSON: %s\n", body.c_str());
    walletServer.stomp_client->send(WalletServer::server_out_queue, headers, body);
}

void WalletServer::setLastCommandTime(std::string sessionId, int newTime)
{
    getSession(sessionId).lastCommandTime = newTime;
}

void WalletServer::setWalletOpen(std::string sessionId, bool newStatus)
{
    getSession(sessionId).walletOpen = newStatus;
}
