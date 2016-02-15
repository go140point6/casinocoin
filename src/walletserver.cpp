#include <fstream>
#include "walletserver.h"
#include "main.h"
#include "util.h"
#include "bitcoinrpc.h"
#include "ui_interface.h"
#include "stomp/booststomp.h"

#include <boost/filesystem.hpp>
#include <boost/serialization/map.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>

using namespace std;
using namespace boost;
using namespace STOMP;

bool isServerRunning = false;
static BoostStomp*  stomp_client;
// topics and queues
static string block_notifications_topic = "/topic/Blocks";
static string cmd_in_queue = "/queue/WalletCmdIn";
static string cmd_out_queue = "/queue/WalletCmdOut";
// ActiveMQ parameters
static string stomp_host = GetArg("-activemqstomphost", "localhost");
static int stomp_port = GetArg("activemqstompport", 61613);
// in memory key/value stores
typedef std::map<std::string, std::string> keyValueType;
static keyValueType sessions;
static keyValueType walletServerSecrets;

// Handler for NotifyBlocksChanged signal
static void NotifyBlocksChanged()
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
        headers["content-type"] = string("application/json");
        string body = json_spirit::write_string(jsonBlock, false);
        // add an outgoing message to the topic
        stomp_client->send(block_notifications_topic, headers, body);

    }
    catch (std::exception& e)
    {
        cerr << "Error in BoostStomp: " << e.what() << "\n";
    }

}

// Handler for NotifySessionCreated signal
static void NotifySessionCreated(std::string accountId, std::string sessionId)
{
    sessions.insert(std::make_pair(accountId, sessionId));
    cout << "Inserted: " << accountId.c_str() << " / " << sessionId.c_str() << endl;
}

// load wallet server secret map from filesystem
void loadWalletServerSecrets()
{
    boost::filesystem::path walletServerMapPath = GetDataDir() / "walletserver.dat";
    std::ifstream ifs(walletServerMapPath.string().c_str());
    boost::archive::binary_iarchive bia(ifs);
    bia >> walletServerSecrets;
}

// save wallet server secret map to filesystem
void saveWalletServerSecrets()
{
    boost::filesystem::path walletServerMapPath = GetDataDir() / "walletserver.dat";
    std::ofstream ofs(walletServerMapPath.string().c_str());
    boost::archive::binary_oarchive boa(ofs);
    boa << walletServerSecrets;
}

//bool session_callback(STOMP::Frame& _frame) {
//    cout << "--Incoming STOMP Frame--" << endl;
//    cout << "  Headers:" << endl;
//    hdrmap headers = _frame.headers();
//    for (STOMP::hdrmap::iterator it = headers.begin() ; it != headers.end(); it++ )
//        cout << "\t" << (*it).first << "\t=>\t" << (*it).second << endl;
//    //
//    cout << "  Body: (size: " << _frame.body().v.size() << " chars):" << endl;
//    hexdump(_frame.body().c_str(), _frame.body().v.size() );
//    return(true); // return false if we want to disacknowledge the frame (send NACK instead of ACK)
//}

bool in_queue_callback(STOMP::Frame& _frame) {
    cout << "--Incoming STOMP Frame--" << endl;
    cout << "  Headers:" << endl;
    hdrmap headers = _frame.headers();
    for (STOMP::hdrmap::iterator it = headers.begin() ; it != headers.end(); it++ )
        cout << "\t" << (*it).first << "\t=>\t" << (*it).second << endl;
    //
    cout << "  Body: (size: " << _frame.body().v.size() << " chars):" << endl;
    hexdump(_frame.body().c_str(), _frame.body().v.size() );
    return(true); // return false if we want to disacknowledge the frame (send NACK instead of ACK)
}

void StartWalletServerThread()
{
    // Make this thread recognisable as the wallet server thread
    RenameThread("casinocoin-walletserver");
    printf("CasinoCoin WalletServer Daemon starting\n");
    // get the wallet dir
    boost::filesystem::path walletServerPath = GetDataDir() / "wallets";
    boost::filesystem::create_directories(walletServerPath);
    printf("Using wallet directory: %s\n", walletServerPath.string().c_str());
	// initiate a new BoostStomp client
	stomp_client = new BoostStomp(stomp_host, stomp_port);
	// start the client, (by connecting to the STOMP server)
	stomp_client->start();
    // subscribe to the In Queue to receive the incomming commands
    stomp_client->subscribe(cmd_in_queue, (STOMP::pfnOnStompMessage_t) &in_queue_callback);
    isServerRunning = true;
    // connect to NotifyStartNewWalletServerSession signal
    uiInterface.NotifyStartNewWalletServerSession.connect(boost::bind(NotifySessionCreated, _1, _2));
    // connect to NotifyBlocksChanged signal
    uiInterface.NotifyBlocksChanged.connect(boost::bind(NotifyBlocksChanged));
}

void StopWalletServerThread()
{
    if(isServerRunning)
    {
        // unsubscribe from In Queue
        stomp_client->unsubscribe(cmd_in_queue);
        // flush all Server Wallets
        printf("WalletServer flushing wallets\n");
        // close queue connections
        printf("WalletServer closing queue connections\n");
        stomp_client->stop();
        delete stomp_client;
        // stop server thread
        isServerRunning = false;
        printf("WalletServer STOPPED\n");
    }
}

bool isNewAccountId(std::string accountId){
    return (sessions.count(accountId) == 0);
}
