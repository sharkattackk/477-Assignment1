
#include "server.hpp"


KeyValueServer::KeyValueServer(string nodeName): Node(nodeName){
    // note this is the same as installng the service software on the server
    // it doesn't run yet until the node is started.

    cout << "Main: " << nodeName << endl;
    cout << "Service: adding kv service" << endl;

    shared_ptr<KeyValueService> Service = make_shared<KeyValueService>(nodeName,weak_from_this());
    addService(Service);
}
