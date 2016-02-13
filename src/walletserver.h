#ifndef WALLETSERVER_H
#define WALLETSERVER_H

#include <boost/signals2/signal.hpp>

extern bool isServerRunning;

void StartWalletServerThread();
void StopWalletServerThread();

bool isNewAccountId(std::string accountId);

#endif // WALLETSERVER_H
