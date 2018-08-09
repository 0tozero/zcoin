// Copyright (c) 2018 Tadhg Riordan, Zcoin Developer
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chainparams.h"
#include "chain.h"
#include "zmqabstract.h"
#include "zmqpublisher.h"
#include "main.h"
#include "util.h"
#include "rpc/server.h"
#include "script/standard.h"
#include "base58.h"
#include "znode-sync.h"
#include "net.h"
#include "script/ismine.h"
#include "wallet/wallet.h"
#include "wallet/wallet.cpp"

#include "client-api/methods.cpp"
#include "client-api/server.h"

#include "chainparamsbase.h"
#include "clientversion.h"
#include "rpc/protocol.h"
#include "util.h"
#include "utilstrencodings.h"

#include <boost/filesystem/operations.hpp>
#include <stdio.h>

#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/keyvalq_struct.h>

#include <univalue.h>

using namespace std::chrono;

static std::multimap<std::string, CZMQAbstractPublisher*> mapPublishers;
extern CWallet* pwalletMain;

bool CZMQAbstractPublisher::Initialize()
{
    LogPrint(NULL, "zmq: Initialize notification interface\n");
    assert(!pcontext);

    pcontext = zmq_init(1);

    if (!pcontext)
    {
        zmqError("Unable to initialize context");
        return false;
    }

    assert(!psocket);

    // set publishing topic
    SetTopic();

    // set API method string
    SetMethod();

    //set method string in request object
    request.setObject();
    request.push_back(Pair("collection", method));

    // set publish univalue as an object
    publish.setObject();

    // check if address is being used by other publish notifier
    std::multimap<std::string, CZMQAbstractPublisher*>::iterator i = mapPublishers.find(address);

    // check if address is being used by other publisher
    if (i==mapPublishers.end())
    {
        psocket = zmq_socket(pcontext, ZMQ_PUB);
        if (!psocket)
        {
            zmqError("Failed to create socket");
            return false;
        }

        if(CZMQAbstract::DEV_AUTH){
            // Set up PUB auth.
            vector<string> keys = readCert(CZMQAbstract::Server);

            string server_secret_key = keys.at(1);

            const int curve_server_enable = 1;
            zmq_setsockopt(psocket, ZMQ_CURVE_SERVER, &curve_server_enable, sizeof(curve_server_enable));
            zmq_setsockopt(psocket, ZMQ_CURVE_SECRETKEY, server_secret_key.c_str(), 40);
        }

        int rc = zmq_bind(psocket, address.c_str());
        if (rc!=0)
        {
            zmqError("Failed to bind address");
            zmq_close(psocket);
            return false;
        }

        // register this publisher for the address, so it can be reused for other publish publisher
        mapPublishers.insert(std::make_pair(address, this));
        return true;
    }
    else
    {
        LogPrint(NULL, "zmq: Reusing socket for address %s\n", address);

        psocket = i->second->psocket;
        mapPublishers.insert(std::make_pair(address, this));

        return true;
    }
}

void CZMQAbstractPublisher::Shutdown()
{
    if (pcontext)
    {
        assert(psocket);

        int count = mapPublishers.count(address);

        // remove this notifier from the list of publishers using this address
        typedef std::multimap<std::string, CZMQAbstractPublisher*>::iterator iterator;
        std::pair<iterator, iterator> iterpair = mapPublishers.equal_range(address);

        for (iterator it = iterpair.first; it != iterpair.second; ++it)
        {
            if (it->second==this)
            {
                mapPublishers.erase(it);
                break;
            }
        }
        if (count == 1)
        {
            LogPrint(NULL, "Close socket at authority %s\n", authority);
            int linger = 0;
            zmq_setsockopt(psocket, ZMQ_LINGER, &linger, sizeof(linger));
            zmq_close(psocket);
        }

        zmq_close(psocket);
        psocket = 0;

        zmq_ctx_destroy(pcontext);
        pcontext = 0;
    }
}

bool CZMQAbstractPublisher::Execute(){
    APIJSONRequest jreq;
    try {
        jreq.parse(request);

        publish.setObject();
        publish = tableAPI.execute(jreq, true);

        Publish();

    } catch (const UniValue& objError) {
        message = JSONAPIReply(NullUniValue, objError);
        if(!SendMessage()){
            throw JSONAPIError(API_MISC_ERROR, "Could not send msg");
        }
        return false;
    } catch (const std::exception& e) {
        message = JSONAPIReply(NullUniValue, JSONAPIError(API_PARSE_ERROR, e.what()));
        if(!SendMessage()){
            throw JSONAPIError(API_MISC_ERROR, "Could not send error msg");
        }
        return false;
    }
    
    return true;
}

bool CZMQAbstractPublisher::Publish(){
  try {
      // Send reply
      message = JSONAPIReply(publish, NullUniValue);
      if(!SendMessage()){
          throw JSONAPIError(API_MISC_ERROR, "Could not send msg");
      }
      return true;

  } catch (const UniValue& objError) {
      message = JSONAPIReply(NullUniValue, objError);
      if(!SendMessage()){
          throw JSONAPIError(API_MISC_ERROR, "Could not send msg");
      }
      return false;
  } catch (const std::exception& e) {
      message = JSONAPIReply(NullUniValue, JSONAPIError(API_PARSE_ERROR, e.what()));
      if(!SendMessage()){
          throw JSONAPIError(API_MISC_ERROR, "Could not send error msg");
      }
      return false;
  }
}

bool CZMQStatusEvent::NotifyStatus()
{
    Execute();
    return true;
}


bool CZMQConnectionsEvent::NotifyConnections()
{
    Execute();
    return true;
}
bool CZMQTransactionEvent::NotifyTransaction(const CTransaction &transaction)
{
    UniValue requestData(UniValue::VOBJ);
    requestData.push_back(Pair("txRaw",EncodeHexTx(transaction)));
    request.replace("data", requestData);

    Execute();

    return true;
}

bool CZMQBlockEvent::NotifyBlock(const CBlockIndex *pindex){
    request.replace("data", pindex->ToJSON());
    Execute();

    return true;
}