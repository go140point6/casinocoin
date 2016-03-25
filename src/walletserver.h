#ifndef WALLETSERVER_H
#define WALLETSERVER_H

#include <string>
#include <map>
#include <queue>
#include <boost/filesystem.hpp>
#include <boost/signals2.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread.hpp>
#include "stomp/booststomp.h"
#include "main.h"

struct Session {
    std::string email;
    std::string sessionId;
    int creationTime;
    bool walletOpen;
    int lastCommandTime;
};

struct Command {
    std::string accountId;
    std::string sessionId;
    std::string correlationId;
    std::string command;
    std::map<std::string, std::string> arguments;
};

// Queue class that has thread synchronisation
template <typename T> class SynchronisedCommandQueue
{
    private:
        std::queue<T> m_queue; // Use STL queue to store data
        boost::mutex m_mutex; // The mutex to synchronise on
        boost::condition_variable m_cond; // The condition to wait for

    public:
        // Add data to the queue and notify others
        void Enqueue(const T& data)
        {
            // Acquire lock on the queue
            boost::unique_lock<boost::mutex> lock(m_mutex);
            // Add the data to the queue
            m_queue.push(data);
            // Notify others that data is ready
            m_cond.notify_one();
        } // Lock is automatically released here

        // Get data from the queue. Wait for data if not available
        T Dequeue()
        {
            // Acquire lock on the queue
            boost::unique_lock<boost::mutex> lock(m_mutex);
            // When there is no data, wait till someone fills it.
            // Lock is automatically released in the wait and obtained
            // again after the wait
            while (m_queue.size()==0)
                m_cond.wait(lock);
            // Retrieve the data from the queue
            T result = m_queue.front();
            m_queue.pop();
            return result;
        } // Lock is automatically released here
};

class QueueProducer
{
    private:
        std::string m_session_id; // The id of the session
        SynchronisedCommandQueue<Command>* m_queue; // The queue to use

    public:
        // Constructor with id and the queue to use
        QueueProducer(std::string sessionId, SynchronisedCommandQueue<Command>* queue)
        {
            m_session_id = sessionId;
            m_queue=queue;
        }

        void operator () ()
        {
            // keep running until interupted
            while(true)
                boost::this_thread::interruption_point();
        }

        // The thread function fills the queue with data
        void Enqueue(Command data)
        {
            m_queue->Enqueue(data);
        }

        std::string getSessionId()
        {
            return m_session_id;
        }
};

typedef std::map<std::string, Session> sessionKeyValueType;
typedef std::map<std::string, QueueProducer> sessionCommandQueueType;
typedef std::map<std::string, std::string> keyValueType;

extern bool isServerRunning;

// Server Start/Stop/Flush methods
void StartWalletServerThread();
void StopWalletServerThread();

// Class that cointains all WalletServer commands
class WalletServer
{
    public:
        // Constructor
        WalletServer();

        // WalletServer Signals
        boost::signals2::signal<void (std::string accountId, Session session)> NotifyStartNewWalletServerSession;
        boost::signals2::signal<void (std::string sessionId)> SessionShutdownSignal;

        // topics and queues
        static std::string block_notifications_topic;
        static std::string server_in_queue;
        static std::string server_out_queue;
        // ActiveMQ parameters
        static std::string stomp_host;
        static int stomp_port;
        // STOMP Client
        STOMP::BoostStomp* stomp_client;
        // WalletServer genesisblock
        static CBlockIndex wsGenesisBlock;

        // list persistance methods
        void loadWalletServerSecrets();
        void saveWalletServerSecrets();
        void loadWalletList();
        void saveWalletList();

        // wallet commands
        bool isNewAccountId(std::string accountId);
        sessionKeyValueType& getSessions();
        Session& getSession(std::string sessionId);
        bool deleteSession(std::string sessionId);
        std::string createNewWallet(std::string accountId, std::string passphrase);
        keyValueType getWallets();
        bool isWalletAccountValid(std::string walletId, std::string accountId);
        boost::filesystem::path getWalletServerPath();
        void setLastCommandTime(std::string sessionId, int newTime);
        void setWalletOpen(std::string sessionId, bool newStatus);

        void sendErrorMessage(int errorCode, std::string sessionId, std::string correlationId, std::string command);
        bool in_queue_callback(STOMP::Frame& _frame);
        bool out_queue_callback(STOMP::Frame& _frame);
        void NotifyBlocksChanged();
        void NotifySessionCreated(std::string accountId, Session session);

};

extern WalletServer walletServer;

#endif // WALLETSERVER_H
