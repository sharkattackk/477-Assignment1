#ifndef __NETWORK_HPP__
#define __NETWORK_HPP__

#include <vector>
#include <queue>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <exception>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using namespace std;

int network_init(int argc, char * argv[]);
void mclose(int sockfd);

//+
// struct: udpMesage
// simuluated UDP message. src and dest addressport
// pointer to data
//-

struct udpMessage{
    in_addr_t src;
    in_addr_t dst;
    in_port_t srcPort;
    in_port_t dstPort;
    unique_ptr<uint8_t[]> data;
    uint32_t len;
};

//+
// class: udpBufferQueue
//
// Purpose:
// each socket has a UDP buffer. In reality all
// sockets share a buffer, but easier to do this
// way and the difference is not significant for the
// purpose of the assignments.
//
// This class wraps a c++ queue to provide a limit
// on the size of the queue. Additional items are
// ignored.
//
// It also adds a mutex to make the queue thread safe,
// and provides a queue read option with a timout
// to model the timeout on a socket.
//
//
//-

class udpBufferQueue {
	mutex qmutex;
	condition_variable cond_var;
	queue<shared_ptr<udpMessage>> buffer;
	bool thread_alive;

    public:
        void thread_stop(int);
	void reset(int);
	void addMessage(shared_ptr<udpMessage> aMessage, int sockfd);
	shared_ptr<udpMessage> firstMessage(int sockfd);
	shared_ptr<udpMessage> firstMessageTimeout(struct timeval tv, int sockfd);
};

//+
// class exitThread
//
// Purpose:
// used to handle terminating a thread.
// C++ does not have a way to kill a thread
// from the outside.
// There are two types of threads in this program
// one for each service running on a node and
// one for the client runing on a node.
// When we terminate a node we set the alive
// on the service and on the node to false,
// and the library funcitons check the alive flag
// if false, then they throw this exception 
// which is caught at the point in the code where
// the threads were started, allowing them to
// exit
//-

class exitThread: public exception{
    virtual const char * what(){
       return "exit thread";
    }
};

class Node;

class Service {

    protected:
        // service name
        string name;
	atomic<bool> alive;
	weak_ptr<Node> parent;
	thread theThread;

    public:
        Service(string nodeName, weak_ptr<Node> p)
		: name(nodeName),parent(p),alive(true) {}
	virtual ~Service(){
	}
        virtual void start() = 0;
        virtual void stop(); // = 0;
	friend class Node;

	friend int socket(int domain, int type, int protocol);
	friend int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
	friend ssize_t recvfrom(int sockfd, void *buf, size_t blen, int flags, struct sockaddr *src_addr, socklen_t *addrlen);
	friend ssize_t sendto(int sockfd, const void *buf, size_t blen, int flags, const struct sockaddr *dest_addr, socklen_t addrlen);
	friend int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
	friend void mclose(int sockfd);
	friend void delayedDelivery(shared_ptr<Node> this_node, shared_ptr<Node> dstNetNode,  shared_ptr<udpMessage> msg);
	friend void deliverPacket(string name, shared_ptr<Node> dstNetNode,  shared_ptr<udpMessage> msg);
};

//+
// struct: Socket
//
// This struct tracks the internal state of our
// simulated socket.
//    bool in_use - the socket has been allocated by a socket call, freed by the mclose function
//    int_port_t port_no - 16 bit port number assigned to the socket. 0 if no port assigned.
//    struct timeval tv - timout for the socket if it has been set by setsockopt
//    udpBufferQueue mesages - the queue structure for incomming messages
//-

struct Socket {
    bool in_use;
    in_port_t port_no;
    struct timeval tv;
    udpBufferQueue messages;
    shared_ptr<Service> owner;
};


extern std::mutex nodes_mutex;
extern map<thread::id,shared_ptr<Node>> nodes;
extern std::map<in_addr_t,shared_ptr<Node>> router;
extern map<thread::id,shared_ptr<Service>> serviceMap;

class Node : public enable_shared_from_this<Node>{
    protected:
        // system identity
        string name;
	string addr_str;

	// network infrastructure
	struct in_addr address;
	static const int numSockets = 5;
	struct Socket sockets[numSockets];
	vector<shared_ptr<Service>> services;
	vector<thread> service_threads;
	///map<thread::id,thread> service_threads;
	std::atomic<bool> alive;

        void addService(shared_ptr<Service> s);

    public:
	Node(string nodeName);
	virtual ~Node(){
	}
	void setAddress(string address);

	// service Functions
	void startServices();
	void stopServices();
	void waitForServices();


	// start and stop client on this node.
        virtual void start();
        virtual void stop();
     
        friend class Service;
	friend int socket(int domain, int type, int protocol);
	friend int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
	friend ssize_t recvfrom(int sockfd, void *buf, size_t blen, int flags, struct sockaddr *src_addr, socklen_t *addrlen);
	friend ssize_t sendto(int sockfd, const void *buf, size_t blen, int flags, const struct sockaddr *dest_addr, socklen_t addrlen);
	friend int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
	friend void mclose(int sockfd);
	friend void delayedDelivery(shared_ptr<Node> this_node, shared_ptr<Node> dstNetNode,  shared_ptr<udpMessage> msg);
	friend void deliverPacket(string name, shared_ptr<Node> dstNetNode,  shared_ptr<udpMessage> msg);
};



#endif
