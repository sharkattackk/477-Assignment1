
#include "network.hpp"
#include "service.hpp"

class KeyValueServer: public Node{

    public:
        KeyValueServer(string nodeName);
	~KeyValueServer(){
	}
};
