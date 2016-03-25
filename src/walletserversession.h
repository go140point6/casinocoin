#ifndef WALLETSERVERSESSION_H
#define WALLETSERVERSESSION_H

#include <string>
#include "walletserver.h"
#include "wallet.h"
#include "json/json_spirit.h"
#include "stomp/booststomp.h"
#include <boost/filesystem.hpp>
#include <boost/thread.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>

typedef std::map<CTxDestination, int64> balancesMapType;

// Class that consumes objects from a queue
class WalletServerSession
{
    private:
        SynchronisedCommandQueue<Command>* m_queue; // The queue to use
        std::string sessionId; // The id of the wallet session
        std::string walletId;  // The wallet id of the session
        boost::filesystem::path walletFilename;
        // ActiveMQ Connection
        STOMP::BoostStomp* session_stomp_client;
        // Wallet
        CWallet* clientWallet;
        bool walletOpen;
        // Handle new block signal
        void NotifyBlocksChanged();
        // Handle new transaction signal
        void NotifyTransactionChanged(CWallet *wallet, const uint256 &hash, ChangeType status);
        // shutdown condition
        bool shutdownComplete;

        // wallet commands
        void openWallet(Command data);
        void closeWallet(Command data);
        json_spirit::Value getWalletInfo(Command data);
        json_spirit::Value getAddressBook(Command data);
        json_spirit::Value sendCoinsToAddress(Command data);

    public:
        // Constructor with id and the queue to use.
        WalletServerSession(std::string newSessionId, SynchronisedCommandQueue<Command>* queue);
        // Destructor
        ~WalletServerSession();

        // Default operator that starts the ActiveMQ connection and reads data from the WalletServer queue
        void operator () ()
        {
            printf("WalletServerSession - ActiveMQ connection for session: %s\n", sessionId.c_str());
            // initialize wallet pointer to NULL
            clientWallet = NULL;
            shutdownComplete = false;
            // initiate a new BoostStomp client
            session_stomp_client = new STOMP::BoostStomp(WalletServer::stomp_host, WalletServer::stomp_port);
            session_stomp_client->enable_debug_msgs(false);
            // start the client, (by connecting to the STOMP server)
            session_stomp_client->start();
            // connect to Signals
            uiInterface.NotifyBlocksChanged.connect(boost::bind(&NotifyBlocksChanged, this));
            printf("WalletServerSession - Start Dequeue for session: %s\n", sessionId.c_str());
            while (true)
            {
                // Get the data from the queue and print it
                Command data = m_queue->Dequeue();
                printf("Consumer Session: %s consumed: %s for session: %s ThreadID: %s\n",
                       sessionId.c_str(), data.command.c_str(), data.sessionId.c_str(),
                       boost::lexical_cast<std::string>(boost::this_thread::get_id()).c_str());
                executeWalletCommand(data);
                // if closesesion command then wait for shutdown complete
                if(data.command.compare("closesession") == 0)
                {
                    while(!shutdownComplete)
                    {
                        printf("Wait until closesession shutdownComplete");
                        MilliSleep(100);
                    }
                }
                // Make sure we can be interrupted
                boost::this_thread::interruption_point();
            }
        }

        // public methods
        void sendMessageToQueue(json_spirit::Object outputJson);
        void executeWalletCommand(Command data);
        void closeWalletIfOpen();
        void setWalletOpen();
        bool isWalletOpen();
        void closeSession();
};

#endif // WALLETSERVERSESSION_H
