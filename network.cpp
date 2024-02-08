/*+
 * File:	network.cpp
 *
 * Purpose:
 *   This module implements the simulated UDP network connections for the ELEC 477
 * distributed systems course. It provides implementations for a limited number of
 * IPV4 system calls. This allows the student nodes to run as threads, and to simulate
 * network failures between the nodes. It is not intended to be a full and faithful
 * simulations of sockets. Just close enough for the course.
 *
 *  Implemented Network Failures:
 *     Drop packet rate
 *     Receive Buffer Size (also drops but not for the same reason)
 *     Packet delay (allowing out of order packts).
 *
 * TODO
 *  More faithful documentation
 * - delayed packets arriving at dead nodes or services 
 *	- must check entity still exists, and check that alive
 *	- must track and collect the threads
 *
 *
 * Author:	Thomas R. Dean
 *
 * Date:	August 2023
 *
 * Copyright 2023 Thomas R. Dean
-*/

#include <iostream>
#include <sstream>
#include <string>
#include <cstring>
#include <thread>
#include <map>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <regex>
#include <random>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "network.hpp"
#include "dumpHex.hpp"

// thread id of main control thread
// used to confirm that socket functions
// not called from main thread

thread::id main_id;
class exitThread exit_thread;

// In unix, a socket is a file descriptor (int32_t).
// To close a slocket, you call close, but calling close
// on our socket will create an error. We can't provide
// an alternate version of close In the student
// code we use a #define to redefine close to mclose.
// mclose can check the range of the fd and call close
// for the real file descriptors

const int firstFD = 1000;
const int numSockets = 5;

// again, a simple default
const int firstEmpherimalPort = 3800;

// while UDP messages can be up to 655507 bytes long,
// I don't want to simulate message fragementation.
// 1400 is roughly the max for a single ethernet packet,
// and is enough for the class assignments.

const int udpMessageMaxLen = 1400;

// a small nubmer of UDP buffers allows us to
// easily create receive buffer overruns
// TODO make this a parameter since it is not
// used in any declarations, only as a guard
// in the udp message queue class.
// TODO add as field in the udp Buffer class.
// would allow different buffer sizes on different nodes.

const int maxUDPBuffers = 2;

// Debugging/tracing flags
static bool enterExit = false;
static bool networkActions = false;
static uint32_t networkDebug = 0;
static uint32_t nodeDebug = 0;

// network fuzzing options
float dropProbability=0.0;
float delayProbability=0.0;
uint32_t delayMin=0;
uint32_t delayMax=0;

// information routine
static shared_ptr<Node> getNode(thread::id tid, string functionName);
static shared_ptr<Service> getService(thread::id tid, string functionName);
static string getName(thread::id tid);

//+
// class udpBufferQueue
//
// This class is a wrapper arround the c++ stl queue class.
// The wrapper provies synchrnoization between threads and
// represents the buffer associated with a socket in a real OS.
// 
//-

//+
// Method: reset
//
// This resets the message queue associated with a simulated socket.
//
// Parameter
//   sockfd - the socket number passed by the library routines, used only
// for debugging.
//-

void udpBufferQueue::reset(int sockfd){
    if (networkDebug & 4){
        stringstream ss;
	ss << getName(this_thread::get_id()) << ": in bufferQueue reset socket " << sockfd << endl;
	cout << ss.str();
    }
    buffer = queue<shared_ptr<udpMessage>>();
    thread_alive = true;
}

//+
// Method: thread_stop
//
// Used when terminating a thread. C++ cannot force termiant a thread.
// So the thread has an alive flag that the library checks periodically.
// If the thread is waiting on a socket, it will be waiting on the bufferqueue.
// The buffer queue also has a copy of the flag. This method sets the
// flag to false, and wakes up any thread waiting on the queue. 
//
// Parameter
//   sockfd - the socket number passed by the library routines, used only
// for debugging.
//-

void udpBufferQueue::thread_stop(int socketfd){
    if (networkDebug & 4){
        stringstream ss;
	ss << getName(this_thread::get_id()) << ": in bufferQueue thread_stop socket " << socketfd << endl;
	cout << ss.str();
    }
    lock_guard lock(qmutex);
    thread_alive=false;
    cond_var.notify_one();
}

//+
// Method: addMessage
//
// Used to model a message arriving at a socket. The message
// is added to the list and any waiting processes are signaled.
//
// Parameter
//   aMessage - the UDP message.
//   sockfd - the socket number passed by the library routines, used only
// for debugging.
//-

void udpBufferQueue::addMessage(shared_ptr<udpMessage> aMessage, int sockfd){
    if (networkDebug & 4){
        stringstream ss;
	ss << getName(this_thread::get_id()) << ": in bufferQueue addMessage dst socket " << sockfd << endl;
	cout << ss.str();
    }
    lock_guard lock(qmutex);
    if (!thread_alive){
	if (networkActions){
	    stringstream ss;
	    ss << "Buffer: socket not alive, message dopeed" << endl;
	    cout << ss.str();
	}
	return;
    }
    // check length and drop if too many.
    if (buffer.size() < maxUDPBuffers){
	//cout <<"pushing message" << endl;
	buffer.push(aMessage);
    } else {
	// change if move to smart pointers
	if (networkActions){
	    stringstream ss;
	    ss << "Buffer: destination buffer full, message dopeed" << endl;
	    cout << ss.str();
	}
	//delete aMessage.data; now smart pointer, should auto delete
	return;
    }
    // only notify if pushed.
    cond_var.notify_one();
}

//+
// Method: firstMessage
//
// Return the first message. blocks if queue is empty
//
// Parameter
//   sockfd - the socket number passed by the library routines, used only
// for debugging.
//
// Returns
//   the UDP message.
//
//-
    
shared_ptr<udpMessage> udpBufferQueue::firstMessage(int sockfd){
    stringstream ss;
    if (networkDebug & 4){
	ss << getName(this_thread::get_id()) << ": in bufferQueue firstMessage socket " << sockfd << endl;
	cout << ss.str();
    }
    //cout << "waiting for message" << endl;
    unique_lock lock(qmutex);
    cond_var.wait(lock, [&]{ return !buffer.empty() || !thread_alive; });
    if (networkDebug & 4){
	ss = stringstream();
	ss << "stopped waiting, tread_alive = " << thread_alive << endl;
	cout <<  ss.str();
    }
    // if thread is dead, throw back to thread spawn to cleanly exit
    if (!thread_alive) throw  exit_thread;

    if (networkDebug & 4){
	ss = stringstream();
	ss << "receiving message" << endl;
	cout <<  ss.str();
    }
    shared_ptr<udpMessage> res = buffer.front();
    buffer.pop();
    return  res;
}

//+
// Method: firstMessageTimeout
//
// Return the first message. blocks if queue is empty. However block
// will timeout to model timout on sockets.
//
// Parameter
//   tv - timoeut parameter
//   sockfd - the socket number passed by the library routines, used only
// for debugging.
//
// Returns
//   the UDP message.
//
// add debugging
//-

shared_ptr<udpMessage> udpBufferQueue::firstMessageTimeout(struct timeval tv, int sockfd){
    stringstream ss;
    if (networkDebug & 4){
	ss << getName(this_thread::get_id()) << ": in bufferQueue firstMessageTimeout socket " << sockfd << endl;
	cout << ss.str();
    }
    //cout << "waiting for message2" << endl;
    unique_lock lock(qmutex);
    //cond_var.wait(lock, [&]{ return !buffer.empty(); });
    bool flag = true;
    auto endTime = chrono::steady_clock::now();
    if (tv.tv_sec > 0) endTime += chrono::seconds(tv.tv_sec);
    if (tv.tv_usec > 0) endTime += chrono::microseconds(tv.tv_usec);
    atomic<int> i{0};
    while(flag){
	auto res = cond_var.wait_until(lock,endTime);
	if (!thread_alive) {
	   throw exit_thread;
	}
	if (res == cv_status::timeout){
	    // timout
	    return nullptr;
	} else {
	    // normal finish
	    if (!buffer.empty()){
	        shared_ptr<udpMessage> res = buffer.front();
		buffer.pop();
		return res;
	    }
	}
    }
    //cout << "receiving message2" << endl;
    shared_ptr<udpMessage> res = buffer.front();
    buffer.pop();
    return res;
}

// Each node will run it's own thread.
// The assignments are simple enough that each node
// only needs a single thread.
// our system tracks nodes by the threadid,
// and also has a shortcut map to map addresses
// to the 

mutex nodes_mutex;
map<thread::id,shared_ptr<Node>> nodes;			// thread id -> node for clients
map<thread::id,shared_ptr<Service>> allservices;	// thread id -> service for servers
map<thread::id,string> names;				// thread id -> names for debugging
map<in_addr_t,shared_ptr<Node>> router;			// address -> node for routing

// prototypes for internal functions
// internal dispatching function
static void dispatch(shared_ptr<udpMessage> msg);
// wait before delivering - models delayed messages
void delayedDelivery(string this_name, shared_ptr<Node> dstNode,  shared_ptr<udpMessage> msg);
// internal deliver function
void deliverPacket(string this_name, shared_ptr<Node> dstNode,  shared_ptr<udpMessage> msg);

// random for error modelling.
static float get_random_float();
uniform_real_distribution<> delayDist;
static int get_random_range();

//*************************************************************************
// Private functions called from main.cpp for library management
//*************************************************************************

// initialize the library
int network_init(int argc, char * argv[]){
    if (enterExit) cout << "Main: enter network_init" << endl;
    
    // scan arguments for network specific arguments
    // size of receive buffers (per node?)
    // error types
    // drop packet rate
    // delay packet rate (may cause out of order packet).
    // TODO should remove our args from the list
    // for now just scan for flags.
   
    const regex delayprob_regex("--delayprob=(0.[0-9]+)");
    const regex dropprob_regex("--dropprob=(0.[0-9]+)");
    const regex delayrange_regex("--delayrange=([0-9]+),([0-9]+)");
    const regex netdebug_regex("--networkdebug=([0-9])");
    const regex nodedebug_regex("--nodedebug=([0-9])");
    smatch numberMatch;
    for (int i = 1; i < argc; i++){
        if (strcmp(argv[i],"--nethelp") == 0){
	   cout << "Network Simulation Options:" << endl;
	   cout << "  --dropprob=float        probability to drop a packet" << endl;
	   cout << "  --delayprob=float       probability to delay a packet" << endl;
	   cout << "  --delayrange=int,int    min max ms to delay a packet" << endl;
	   cout << "  --networkactions        print network actions" << endl;
	   cout << "  --networkenterexit      print network funtion enter exit data" << endl;
	   cout << "  --networkdebug=int      set network debug level" << endl;
	   cout << "  --nodedebug=int      set network debug level" << endl;
	   exit(0);
	}
	if (strcmp(argv[i],"--networkactions") == 0){
            networkActions=true;
	}
	if (strcmp(argv[i],"--networkenterexit") == 0){
            enterExit=true;
	}
	string argvs = string(argv[i]);
	if (strncmp(argv[i],"--networkdebug",14) == 0){
	    // if no = set debug level to 1
            networkDebug=1;
	    if(regex_match(argvs,numberMatch,netdebug_regex)){
	        // if it matches, then debug level is in numberMatch[1]
		networkDebug = stoi(numberMatch[1].str()) ;
		cout << " network debug level : " <<  networkDebug << endl;
	    }
	}
	if (strncmp(argv[i],"--nodedebug",11) == 0){
	    // if no = set debug level to 1
            nodeDebug=1;
	    if(regex_match(argvs,numberMatch,nodedebug_regex)){
	        // if it matches, then debug level is in numberMatch[1]
		nodeDebug = stoi(numberMatch[1].str()) ;
		cout << " node debug level : " <<  nodeDebug << endl;
	    }
	}
	if(regex_match(argvs,numberMatch,dropprob_regex)){
	    dropProbability=stof(numberMatch[1].str());
	    //cout << " delay prob matched: " << dropProbability << endl;
	}
	if(regex_match(argvs,numberMatch,delayprob_regex)){
	    delayProbability=stof(numberMatch[1].str());
	    //cout << " delay prob matched: " <<  delayProbability << endl;
	}
	if(regex_match(argvs,numberMatch,delayrange_regex)){
	    delayMin=stoi(numberMatch[1].str());
	    delayMax=stoi(numberMatch[2].str());
	    //cout << " delay range aatched: (" <<  delayMin << "," << delayMax << ")" << endl;
	    if (delayMax < delayMin) {
		cout << "*******delay range (" << delayMin << "," << delayMax << ") invalid" << endl;
		exit(1);
	    }
	}
    }

    // set up distributions...
    if (delayProbability > 0.0){
       if (delayMin > 0){
	  delayDist = uniform_real_distribution<>(delayMin,delayMax);
       } else {
          // default is 1 to 30 ms
	  delayDist = uniform_real_distribution<>(1,30);
       }
    }

    // save main trhread id
    main_id = this_thread::get_id();

    // add node checks if we are the main thread
    // refactor away from NetNode
    names[main_id] = "Main";
    
    if (enterExit) cout << "Main: exit network_init argc = " << argc << endl;
    return argc;
}

void Service::stop(){
   alive = false;
}

void Node::setAddress(string addr){
    addr_str = addr;
    inet_aton(addr.c_str(),&(address));
    lock_guard<mutex> guard(nodes_mutex);
    router.insert(make_pair(address.s_addr, shared_from_this()));
}

void Node::addService(shared_ptr<Service> s){
    services.push_back(s);
}

// node 
Node::Node(string nodeName):alive(true){
    for (int i = 0; i < numSockets; i++){
	sockets[i].port_no = 0;
	sockets[i].in_use = false;
	sockets[i].tv.tv_sec = 0;
	sockets[i].tv.tv_usec = 0;
	sockets[i].owner = nullptr;
    }
}

void Node::startServices(){
   cout << "number of services is " << services.size() << endl;
   vector<shared_ptr<Service>>::iterator it;
   // global maps are not thread safe
   lock_guard<mutex> guard(nodes_mutex);
   for (it = services.begin(); it != services.end(); it++){
       cout << "Service: " << (*it)->name << endl;
       shared_ptr<Service> tmp = *it;
       tmp->theThread = thread([tmp](){
          try {
	      tmp -> start(); 
	  } catch (exitThread & e){
	  }
       });

       nodes.insert(make_pair(tmp->theThread.get_id(),shared_from_this()));
       allservices.insert(make_pair(tmp->theThread.get_id(),tmp));
       names.insert(make_pair(tmp->theThread.get_id(),tmp->name));
   }
}

void Node::waitForServices(){
   vector<shared_ptr<Service>>::iterator it;
   for (it = services.begin(); it != services.end(); it++){
       shared_ptr<Service> tmp = *it;
       if (tmp->theThread.joinable()){
          tmp->theThread.join();
       }
   }
}

void Node::stopServices(){
   vector<shared_ptr<Service>>::iterator it;
   for (it = services.begin(); it != services.end(); it++){
       shared_ptr<Service> tmp = *it;
       tmp -> stop();
   }
   // the above typically sets the alive field to zero so they stop looping.
   // now have to kill the socket waits.
   for (int i = 0; i < numSockets; i++){
       if (sockets[i].owner != nullptr){
           sockets[i].messages.thread_stop(i + firstFD);
       }
   }
}

void Node::start(){
    alive = true;
}

void Node::stop(){
    alive = false;
    // stop and wait any services on this node.
    stopServices();
    waitForServices();
    // find all sockets that are not service sockets and kill them
   for (int i = 0; i < numSockets; i++){
       if (sockets[i].owner == nullptr){
           sockets[i].messages.thread_stop(i + firstFD);
       }
   }
}

//**********************************************************
// currently only IPV4 UDP is simulated...
//**********************************************************

//+
// function: socket
//
// Purpose:
//   This function replaces the function that would come from
// the standard library. The real socket function creates a network
// socket for the current process. In our case, we create
// a simulated socket in our library. The real socket
// function returns a file descriptor that can be used for regular
// i/o fuctions, and the standard library close function is used
// to close the socket. In our case we use fd numbers starging at
// firstFD (default 1000) so that our sockets cannot be confu
// with the system file descriptors. This allows our close function
// to forward real fds to the system.
// 
// parameters:
//	domain   - network domain - only AF_INET (ipv4) recognized.
//	type     - network connection type - only SOCK_DGRAM (UDP) recognized.
//	protocol - sub protocol type - no sub protocols of TCP so only 0 recognized.
// returns
//   simulated socket number   firstFD + socket entry in Node structure
//   -1 on an error
//
// Errors:
//   the variable errno is set to the appropriate simulated error when the
// values are rwong or we run out of simulated socket descriptors (i.e. student
// doesn't call close appropriately.
//
//   EAFNOSUPPORT - something other than AF_INET was passed
//   EPROTONOSUPPORT - only SOCK_DGRAM,0 (UDP) supported
//   EMFILE - out of simulated sockets.
//-

int socket(int domain, int type, int protocol){

    thread::id this_id = this_thread::get_id();
    shared_ptr<Node> this_node = getNode(this_id, "socket"s);
    shared_ptr<Service> this_service = getService(this_id, "socket"s);
    string name = getName(this_id);
    stringstream ss;

    if (enterExit){
        ss << name << ": enter socket" << endl;
        cout << ss.str();
    }


    if (this_service != nullptr){
        // service releated thread
	if (!this_service -> alive){
	    if (enterExit){
		ss << name << ": exit socket Service not alive" << endl;
		cout << ss.str();
	    }
	    throw exit_thread;
        }
    } else {
        if (!this_node -> alive){
	    if (enterExit){
		ss << name << ": exit socket Node not alive" << endl;
		cout << ss.str();
	    }
	    throw exit_thread;
	}
    }

    // check parameters for valid simulation values
    if (domain != AF_INET){ 
    	errno = EAFNOSUPPORT;
	return -1;
    }

    if (type != SOCK_DGRAM){
    	errno = EPROTONOSUPPORT;
	return -1;
    }

    if (protocol != 0) {
    	errno = EPROTONOSUPPORT;
	return -1;
    }

    // find an unused socket
    int i;
    for (i = 0; i < numSockets; i++){
	if(!this_node -> sockets[i].in_use){
	    this_node -> sockets[i].in_use = true;
	    this_node -> sockets[i].port_no = 0;
	    this_node -> sockets[i].messages.reset(i + firstFD); // set flag to alive

	    // if this is a service thread, record the owner
	    // so that we can close it if the service dies or
	    // is killed.
	    
	    this_node -> sockets[i].owner = this_service;

	    // file descriptor is firstFD(default 1000)
	    // larger than any unix file descriptor.
            if (enterExit){
                ss = stringstream();
                ss << name << ": exit socket val = " << firstFD+i << endl;
                cout  << ss.str();
            }
	    return firstFD+i;
	}
    }

    // no sockets available
    errno = EMFILE;
    
    // for now
    if (enterExit){
        ss = stringstream();
        ss << name << ": exit socket val = -1" << endl;
        cout  << ss.str();
    }

    return -1;
}

//+
// function: bind
//
// Purpose:
//   This function replaces the function that would come from
// the standard library. The real bind function binds the socket
// to an address (i.e. one of the interfaces)  and port.
// In our case we confirm the address is the address of the machine,
// and set the port number on the socket. INADDR_ANY is a wildcard
// that maches the address of the node.
// 
// parameters:
//	sockfd   - the allocated socket
//	addr     - The address and port to bind
//	addrlen  - the length of the data in addr.
// returns
//   0 on success 
//   -1 on an error
//
// Errors:
//   the variable errno is set to the appropriate simulated error when the
// values are wrong
//
//   EINVAL - length not long enough for a socket addrews structure
//   EADDRNOTAVAIL - not the address of the node or INADDR_ANY
//   EBADF - sockfd is not a socket that has been opened by this thread
//-

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen){

    thread::id this_id = this_thread::get_id();
    shared_ptr<Node> this_node = getNode(this_id,"bind"s);
    shared_ptr<Service> this_service = getService(this_id,"bind"s);
    string name = getName(this_id);
    
    stringstream ss;
    if (enterExit){
        ss << name << ": enter bind, sock number is " << sockfd << endl;
        cout  << ss.str();
    }

     if (this_service != nullptr){
        if (!this_service -> alive){
            if (enterExit){
                ss << name << ": exit bind Service not alive" << endl;
                cout << ss.str();
            }
            throw exit_thread;
        }
    } else {
        if (!this_node -> alive){
	    if (enterExit){
		ss << name << ": exit socket Node not alive" << endl;
		cout << ss.str();
	    }
	    throw exit_thread;
	}
    }

    // check length first
    socklen_t len = sizeof(sockaddr_in);
    if (len != addrlen){
        errno = EINVAL;
        if (enterExit){
            ss = stringstream();
            ss << name << ": exit bind, wrong length of address structure:" << addrlen << endl;
            cout  << ss.str();
        }
	return -1;
    }

    if (((sockaddr_in*)addr)->sin_addr.s_addr != INADDR_ANY && ((sockaddr_in*)addr)->sin_addr.s_addr != this_node->address.s_addr){
        errno = EADDRNOTAVAIL;
        if (enterExit){
            ss = stringstream();
            ss << name << ": exit bind, given address("<< inet_ntoa(((sockaddr_in*)addr)->sin_addr) << ") does not match the node address("
                << inet_ntoa(this_node->address) << ")"  << endl;
            cout  << ss.str();
        }
	return -1;
    }

    // so the address is our address (omly one address)

    // check the socket fd for a valid socket range
    if (sockfd < firstFD || sockfd > firstFD + numSockets -1){
        // invalid socket
        errno = EBADF;
        if (enterExit){
            ss = stringstream();
            ss << name << ": exit bind, invalid socket number (" << sockfd << ")"  << endl;
            cout  << ss.str();
        }
        return -1;
    }
    
    // is it one we allocated?
    if (this_node -> sockets[sockfd - firstFD].in_use == false){
        errno = EBADF;
        if (enterExit){
            ss = stringstream();
            ss << name << ": exit bind, socket (" << sockfd << ") not in use"  << endl;
            cout  << ss.str();
        }
        return -1;
    }
    
    
    // bind the port number (translate back to host form (network form only for transmission)
    this_node -> sockets[sockfd - firstFD].port_no = ntohs(((sockaddr_in*)addr)->sin_port);
    if (networkActions){
        ss = stringstream();
        ss << name << ": bind, port number of socket " << sockfd << " set to " << this_node -> sockets[sockfd - firstFD].port_no << endl;
        cout << ss.str();
    }

    if (enterExit){
        ss = stringstream();
        ss << name << ": exit bind val = 0 (success)" << endl;
        cout  << ss.str();
    }
    return 0;
}

//+
// function: recvfrom
//
// Purpose:
//   This function replaces the function that would come from
// the standard library. The real recvfrom function waits for
// a udp message, or data from a TCP socket. It also handles
// issues with failure of reassembly of fragmented packets.
// Our simulation does not deal with fragmeented packets.
// 
// parameters:
//	sockfd    - the allocated socket
//	buff,blen - The buffer to store the UDP message
//	flags     - operation flags MSG_TRUNC to truncate a messagte to the lenth of a buffer rather than drop.
//	src_addr  - will be filled with the address of the sending node.
//	addrlen   - size or the src_addr buffer
// returns
//   numbrer of bytes read
//   -1 on an error
//
// Errors:
//   the variable errno is set to the appropriate simulated error when the
// values are wrong
//
//  EAGAIN if socket is non blocking
//  EBADF invalid descriptor
//  EFAULT addr is  ouside of address space
//  EINTR interrupted by signal before data was avcaiable
//  ENOMEM unable to allocate memory
//  ENOTSOCK not a socket
// -

ssize_t recvfrom(int sockfd, void *buf, size_t blen, int flags, struct sockaddr *src_addr, socklen_t *addrlen){

    //FLASGS in
    // MSG_TRUNC - return real length even when it was longer than passed buffer
    
    thread::id this_id = this_thread::get_id();
    shared_ptr<Node> this_node = getNode(this_id,"recvfrom"s);
    shared_ptr<Service> this_service = getService(this_id,"recvfrom"s);
    string name = getName(this_id);

    stringstream ss;
    if (enterExit){
        ss << name << ": enter recvfrom" << endl;
        cout << ss.str();
    }

    if (this_service!= nullptr){
        if (!this_service -> alive){
            if (enterExit){
                ss << name << ": exit recvfrom Service not alive" << endl;
                cout << ss.str();
            }
            throw exit_thread;
        }
    } else {
        if (!this_node -> alive){
	    if (enterExit){
		ss << name << ": exit socket Node not alive" << endl;
		cout << ss.str();
	    }
	    throw exit_thread;
	}
    }



    // check length first
    socklen_t len = sizeof(sockaddr_in);
    if (len != *addrlen){
        errno = EINVAL;
        if (enterExit){
            ss = stringstream();
            ss << name << ": recvfrom lenth parameter (" << *addrlen << " is not length of ipv4 socket address (" << len << ")" << endl;
            cout  << ss.str();
        }
	return -1;
    }

    // check the socket fd for a valid socket range
    if (sockfd < firstFD || sockfd > firstFD + numSockets -1){
        // invalid socket
        errno = EBADF;
        if (enterExit){
            ss = stringstream();
            ss << name << ": exit recvfrom, invalid socket number (" << sockfd << ")"  << endl;
            cout  << ss.str();
        }
        return -1;
    }

    // is it one we allocated?
    if (this_node -> sockets[sockfd - firstFD].in_use == false){
        errno = EBADF;
        if (enterExit){
            ss = stringstream();
            ss << name << ": exit recvfrom, socket (" << sockfd << ") not in use"  << endl;
            cout  << ss.str();
        }
        return -1;
    }

    shared_ptr<udpMessage> msg;
    //cout << "about to check port " << this_node->sockets[sockfd-firstFD].port_no << endl;

    if (this_node -> sockets[sockfd - firstFD].tv.tv_sec == 0 && this_node -> sockets[sockfd - firstFD].tv.tv_usec == 0){
        // no timeout
        msg = this_node -> sockets[sockfd - firstFD].messages.firstMessage(sockfd);
    } else {
        // timeout.
        msg = this_node -> sockets[sockfd - firstFD].messages.firstMessageTimeout(this_node -> sockets[sockfd - firstFD].tv,sockfd);
	if (msg == nullptr){
	    // timeout occured
	    if (networkActions){
		ss = stringstream();
		struct in_addr src = {msg -> src};
		ss << name << ": timeout wiating  for msg" << endl;
		cout << ss.str();
	    }
	    errno = EWOULDBLOCK; //EAGAIN; // linux uses EWOULDBLOCK
	    return -1;
	}

    }
    
    if (networkActions){
        ss = stringstream();
        struct in_addr src = {msg -> src};
        ss << name << ": received message from (" << inet_ntoa(src) << "," << ntohs(msg->srcPort) << ")" << endl;
        cout << ss.str();
    }

    // store src address in parameters so caller knows
    // who the message came from.
    ((struct sockaddr_in*)src_addr) -> sin_addr.s_addr = msg->src;
    ((struct sockaddr_in*)src_addr) -> sin_port = msg->srcPort;
    ((struct sockaddr_in*)src_addr) -> sin_family = AF_INET;
    
    int retlen = 0;
    if (msg->len > blen){
        // only copy the data there is room for.
        memcpy(buf,msg->data.get(),blen);
        if (flags & MSG_TRUNC){
            // return real length if flags tell us to
            retlen = msg->len;
        } else {
            // return valid bytes in buffer
            retlen = blen;
        }
    } else {
        memcpy(buf,msg->data.get(),msg->len);
        // buffer has more than enough bytes for the message data.
        retlen = msg->len;
    }
    
    // message data now in buffer, delete.

    if (enterExit){
        ss = stringstream();
        ss << name << ": exit recvfrom val = " << retlen << endl;
        cout << ss.str();
    }
    return  retlen;
}

//+
// function: sendto
//
// Purpose:
//   This function replaces the function that would come from
// the standard library. The real sendto function sends
// a udp message, or data using a TCP socket.
// 
// parameters:
//	sockfd    - the allocated socket
//	buff,blen - The buffer holding the message to be sent
//	flags     - operation flags -- ignored
//	dest_addr  - the address to send the message to
//	addrlen   - size or the dest_addr buffer
// returns
//   numbrer of bytes sent
//   -1 on an error
//
// Errors:
//   the variable errno is set to the appropriate simulated error when the
// values are wrong
//
// Errors:
//   the variable errno is set to the appropriate simulated error when the
// values are wrong
//
//  EAGAIN - would block (not for udp)
//  EBADF invalid descriptor
//  EINVAL invlid argument
//  ENOBUFS no buffers
//  ENOMEM no memory
//  ENOTSOCK
//+

ssize_t sendto(int sockfd, const void *buf, size_t blen, int flags, const struct sockaddr *dest_addr, socklen_t addrlen){

    // FLAGS in
    //MSG_CONFIRM - link layer got a reply from other side
    //
    thread::id this_id = this_thread::get_id();
    shared_ptr<Node> this_node = getNode(this_id,"sendto"s);
    shared_ptr<Service> this_service = getService(this_id,"sendto"s);
    string name = getName(this_id);

    stringstream ss;
    if (enterExit){
        ss << name << ": enter sendto" << endl;
        cout << ss.str();
    }

    if (this_service!= nullptr){
        if (!this_service -> alive){
            if (enterExit){
                ss << name << ": exit sendto Service not alive" << endl;
                cout << ss.str();
            }
            throw exit_thread;
        }
    } else {
        if (!this_node -> alive){
	    if (enterExit){
		ss << name << ": exit socket Node not alive" << endl;
		cout << ss.str();
	    }
	    throw exit_thread;
	}
    }


    // check length first
    socklen_t len = sizeof(sockaddr_in);
    if (len != addrlen){
        errno = EINVAL;
        if (enterExit){
            ss = stringstream();
            ss << name << ": sendto lenth parameter (" << addrlen << " is not length of ipv4 socket address (" << len << ")" << endl;
            cout  << ss.str();
        }
	return -1;
    }
    // check the socket fd for a valid socket range
    if (sockfd < firstFD || sockfd > firstFD + numSockets -1){
        // invalid socket
        errno = EBADF;
        if (enterExit){
            ss = stringstream();
            ss << name << ": exit sendto, invalid socket number (" << sockfd << ")"  << endl;
            cout  << ss.str();
        }
        return -1;
    }
    // is it one we allocated?
    if (this_node -> sockets[sockfd - firstFD].in_use == false){
        errno = EBADF;
        if (enterExit){
            ss = stringstream();
            ss << name << ": exit sendto, socket (" << sockfd << ") not in use"  << endl;
            cout  << ss.str();
        }
        return -1;
    }

    if (blen > udpMessageMaxLen) {
        errno = ENOMEM;
        if (enterExit){
            ss = stringstream();
            ss << name << ": exit sendto, message to lonng (>" << udpMessageMaxLen << ")"  << endl;
            cout  << ss.str();
        }
        return -1;
    }
    
    // first check if send port bound (usually the case in the client)
    if (this_node -> sockets[sockfd - firstFD].port_no == 0){
        // port not bound, pick an empherimal port
        // simple max + 1 algorithm
        int minEmphPort = firstEmpherimalPort;
        for(int i = 0; i < numSockets; i++){
            if(this_node->sockets[i].port_no >= minEmphPort){
                minEmphPort = this_node->sockets[i].port_no+1;
            }
        }
        this_node -> sockets[sockfd - firstFD].port_no = minEmphPort;
	if (networkActions){
	    ss = stringstream();
	    ss << name << ": sending port for socket " << sockfd << " not bound, setting to empherimal Port niumber " << minEmphPort << endl;
	    cout << ss.str();
	}
    }
    
    // send the message
    // make a copy to send through the dispatch function
    shared_ptr<udpMessage> msg = make_shared<udpMessage>();
    msg -> src = this_node->address.s_addr;
    msg -> srcPort = htons(this_node -> sockets[sockfd - firstFD].port_no);
    msg -> dst = ((struct sockaddr_in*)dest_addr)->sin_addr.s_addr;
    msg -> dstPort = ((struct sockaddr_in*)dest_addr)->sin_port;
    msg -> len = blen;
    msg -> data = make_unique<uint8_t[]>(blen);
    memcpy(msg->data.get(),buf,blen);
    
    if (networkActions){
        ss = stringstream();
        struct in_addr dst = {msg->dst};
        ss << name << ": message to (" << inet_ntoa(dst) << "," << ntohs(msg->dstPort) << ")" << endl;
        cout << ss.str();
    }

    // this is the simulated network part
    dispatch(msg);
    
    if (enterExit){
        ss = stringstream();
        ss << name << ": exit sendto val = " << blen << endl;
        cout << ss.str();
    }
    return  blen;
}

//+
// function: setsockopt
//
// Purpose:
//   This function replaces the function that would come from
// the standard library. The real setsockopt funciton handles
// a variety of options. The only option our simulation supports
// is to set a timeout for the recvfrom function
// 
// parameters:
//	sockfd    - the allocated socket
//	level     - what level of opteration (only SOL_SOCKET supported)
//	optname    - the name of the option to set (only SO_RCVTIMEO supported)
//	optval,optlen  - pointer to the memory containing the option and its length
// returns
//   0 on success
//   -1 on an error
//
// Errors:
//   the variable errno is set to the appropriate simulated error when the
// values are wrong
//
// Errors:
//   the variable errno is set to the appropriate simulated error when the
// values are wrong
//
// EBADF invalid descriptor
// EFAULT invlid ptr
// EINVAL invlid vlaue
//-

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen){
    // main flag in the optsios is the set timeout

    thread::id this_id = this_thread::get_id();
    shared_ptr<Node> this_node = getNode(this_id,"setsockopts");
    shared_ptr<Service> this_service = getService(this_id,"setsockopts");
    string name = getName(this_id);

    stringstream ss;
    if (enterExit){
        ss << name << ": enter setsockopt" << endl;
        cout << ss.str();
    }

    if (this_service!= nullptr){
        if (!this_service -> alive){
            if (enterExit){
                ss << name << ": exit setsockopt Service not alive" << endl;
                cout << ss.str();
            }
            throw exit_thread;
        }
    } else {
        if (!this_node -> alive){
	    if (enterExit){
		ss << name << ": exit socket Node not alive" << endl;
		cout << ss.str();
	    }
	    throw exit_thread;
	}
    }


    // check length first
    socklen_t len = sizeof(struct timeval);
    if (len != optlen){
        errno = EINVAL;
        if (enterExit){
            ss = stringstream();
            ss << name << ": exit setsocketopt lenth parameter (" << optlen << " is not length of struct time_val (" << len << ")" << endl;
            cout  << ss.str();
        }
	return -1;
    }

    // check the socket fd for a valid socket range
    if (sockfd < firstFD || sockfd > firstFD + numSockets -1){
        // invalid socket
        errno = EBADF;
        if (enterExit){
            ss = stringstream();
            ss << name << ": exit setsocketopt, invalid socket number (" << sockfd << ")"  << endl;
            cout  << ss.str();
        }
        return -1;
    }

    // is it one we allocated?
    if (this_node -> sockets[sockfd - firstFD].in_use == false){
        errno = EBADF;
        if (enterExit){
            ss = stringstream();
            ss << name << ": exit setsocketopt, socket (" << sockfd << ") not in use"  << endl;
            cout  << ss.str();
        }
        return -1;
    }

    // only level of SOL_SOCKET and  optname of SO_RCVTIMEO supported
    if (level != SOL_SOCKET || optname != SO_RCVTIMEO){
      errno = EINVAL;
        if (enterExit){
            ss = stringstream();
            ss << name << ": exit setsocketopt, only SO_RCVTIMEO supported"  << endl;
            cout  << ss.str();
        }
        return -1;
    }

    // store timeout for socket
    this_node->sockets[sockfd - firstFD].tv = *(struct timeval*)optval;
    if (networkActions){
        ss = stringstream();
        ss << name << ": setsocketopt timeout of socket " << sockfd << " set to ("
            << ((struct timeval*)optval)->tv_sec << ", " << ((struct timeval*)optval)->tv_usec << ")" << endl;
        cout << ss.str();
    }

    if (enterExit){
        ss = stringstream();
        ss << name << ": exit setsockopt val = 0" << endl;
        cout << ss.str();
    }
    return 0;
}


//+
// function: mclose
//
// Purpose:
//   This function replaces the function that would come from
// the standard library. unfortunatley we cannot simply implement
// close, because then we would not be able to forwared the call
// to it. Instead we use a preprocessor to replace "clsoe" with
// "mclose" which allows us to intercept the calls. If the 
// file descriptor is firstFD (default 1000) or larger then
// it is one of our sockets, otherwise forward to the system close.
// 
// parameters:
//	sockfd    - the file descriptor
// returns
//   void
//
// Errors:
//   the variable errno is set to the appropriate simulated error when the
// values are wrong
//
// Errors:
//   the variable errno is set to the appropriate simulated error when the
// values are wrong. Note some other error numbers  might be set by the real close
// function
//
// EBADF invalid descriptor
// EINVAL invlid vlaue
//-

// the real one...
extern "C" int close(int);
void mclose(int sockfd){
    thread::id this_id = this_thread::get_id();
    shared_ptr<Node> this_node = getNode(this_id,"mclose"s);
    shared_ptr<Service> this_service = getService(this_id,"mclose"s);
    string name = getName(this_id);
    
    stringstream ss;
    if (enterExit){
        ss << name << ": enter mclose, sockfd = " << sockfd << endl;
        cout  << ss.str();
    }

    if (this_service!= nullptr){
        if (!this_service -> alive){
            if (enterExit){
                ss << name << ": exit mclose Service not alive" << endl;
                cout << ss.str();
            }
            throw exit_thread;
        }
    } else {
        if (!this_node -> alive){
	    if (enterExit){
		ss << name << ": exit socket Node not alive" << endl;
		cout << ss.str();
	    }
	    throw exit_thread;
	}
    }


    // check the socket fd for a valid socket range
    if (sockfd < firstFD || sockfd > firstFD + numSockets -1){
	// not one of ours, let os handle it
	ss = stringstream();
	ss << name << ": " << sockfd << " is not one of ours, calling system close" << endl;
	cout  << ss.str();

	close(sockfd);
	return;
    }

    // is it one we allocated?
    if (this_node -> sockets[sockfd - firstFD].in_use == true){
	this_node -> sockets[sockfd - firstFD].in_use = false;
	this_node -> sockets[sockfd - firstFD].port_no = 0;
	this_node -> sockets[sockfd - firstFD].tv.tv_sec = 0;
	this_node -> sockets[sockfd - firstFD].tv.tv_sec = 0;
	this_node -> sockets[sockfd - firstFD].messages.reset(sockfd);
	this_node -> sockets[sockfd - firstFD].owner = nullptr;
    } else {
	// socket was not allocated??
	errno = EBADF;
    }

    if (enterExit){
        ss = stringstream();
        ss << name << ": exit mclose" << endl;
        cout  << ss.str();
    }
    return;
}

//+
// Function:    dispatch
//
//  This function routes the message to the correct
// socket queue. It finds the node structure that
// matches the destination address and then searches
// for the approrpaite socket that matches the dest port
// if neither is found, the message is dropped.
//
//  If found, it adds the message to the socket buffer.
//
// Note: if the buffer is full, the message is dropped by
// the buffer, not dispatch.
//-

static void dispatch(shared_ptr<udpMessage> msg){
    thread::id this_id = this_thread::get_id();
    shared_ptr<Node> this_node = getNode(this_id,"dispatch"s);
    string name = getName(this_id);

    stringstream ss;
    if (enterExit){
        ss << name << ": enter dispatch" << endl;
        cout << ss.str();
    }

    // compute chance of dropping a packet here
    if (dropProbability > 0.0){
        if (get_random_float() < dropProbability) {
	    if (networkActions){
                ss = stringstream();
                ss << name << ": random packet loss" << endl;
                cout  << ss.str();
	    }
	    if (enterExit){
                ss = stringstream();
                ss << name << ": exit dispatch dropped packet" << endl;
                cout  << ss.str();
            }
	    return;
        }
    }
   

    // find the node to which the packet should be delivered
    map<in_addr_t,shared_ptr<Node>>::iterator it;

    shared_ptr<Node> dstNode = nullptr;
    
    // subscope for the guard to only apply to the search
    // of the router map.
    {
        lock_guard<mutex> guard(nodes_mutex);
        it = router.find(msg->dst);
        if (it == router.end()){
            // address doens't exist, drop the packet
            //delete msg.data;
            
            if (enterExit){
                ss = stringstream();
                ss << name << ": exit dispatch, no matching address" << endl;
                cout  << ss.str();
            }
            return;
        }
        dstNode = it->second;
    }

    // dstNode is the destination node
    // dstNode should not be nullptr here...

    if (networkDebug > 0){
	cout << "Found the server" << endl;
    }

    // check delay
    if (delayProbability > 0.0){
        if (get_random_float() < delayProbability) {
	    if (networkActions){
                ss = stringstream();
                ss << name << ": random packet delay" << endl;
                cout  << ss.str();
	    }

	    // spin off thread to wait before sending packet.
	    thread delayThread(delayedDelivery,name,dstNode,msg);
	    // detach so the orignal node can continue while packet is
	    // delayed.
	    delayThread.detach();


	    if (enterExit){
                ss = stringstream();
                ss << name << ": exit dispatch packet delayed" << endl;
                cout  << ss.str();
            }
	    // look in service map first
	    return;
        }
    } else {
	deliverPacket(name, dstNode, msg);
    }
    
    

    if (enterExit){
        ss = stringstream();
        ss << name << ": exit dispatch, no delivery, msg dropped" << endl;
        cout  << ss.str();
    }
    
    return;
}

//+
// function delayedDlivery
//
//  Called by dispatch when a message mucst be delayed.
// uses the get_random_range which returns a uniform number
// between delayMin and delayMax. 
//
// Parameters:
//	this_name - the name of the sending thread (Node or Service)
//	dstNode - the destination node
//      msg - the message.
//-

void delayedDelivery(string this_name, shared_ptr<Node> dstNode,  shared_ptr<udpMessage> msg){
    stringstream ss;
    if (enterExit){
        ss << this_name << ": enter delayedDelivery" << endl;
        cout << ss.str();
    }
    int delayMs = get_random_range();
    if (networkActions){
	ss = stringstream();
	struct in_addr dst = {msg->dst};
	ss << this_name << ": delayedDelivery, waiting for " << delayMs << "ms" << endl;
	cout  << ss.str();
    }

    this_thread::sleep_for(chrono::milliseconds(delayMs));

    deliverPacket(this_name, dstNode,  msg);
    //delete msg.data;

    if (enterExit){
        ss = stringstream();
        ss << this_name << ": exit delayedDelivery, message sent" << endl;
        cout  << ss.str();
    }
}

//+
// function deliverPacket
//
//  This function does the actual deliveray of the UDP message
// to the destination socket. Separated into a separate function
// from dispatch so that it can be delayed by a random amount
// in delayedDelivery.  Since delayedDelivery will be executed
// in it's own thread, we cannot used the thread id to identify
// the node. So the src node is passed as a parameter through
// the thread start.
//
//  This function is called directly by dispatch if the
// delivery is not delayed.
//
// Parameters:
//	this_name - the name of the sending thread (Node or Service)
//	dstNode - the destination node
//      msg - the message.
//-

void deliverPacket(string this_name, shared_ptr<Node> dstNode,  shared_ptr<udpMessage> msg){
    stringstream ss;
    if (enterExit){
        ss << this_name << ": enter deliverMessage" << endl;
        cout << ss.str();
    }

    if (!dstNode-dstNode->alive){
        // drop the packet
	if (enterExit || networkActions){
	    ss = stringstream();
	    ss << this_name << ": exit deliverMessage, node dead, msg dropped" << endl;
	    cout  << ss.str();
	}
    }

    // find the socket
    for (int i = 0; i < numSockets; i++){
        //cout << "checking socket " << (i + firstFD) << ", port number " << dstNode -> sockets[i].port_no << endl;
        if (dstNode->sockets[i].in_use && dstNode->sockets[i].port_no == ntohs(msg->dstPort)){
            // found the matching port
            dstNode ->sockets[i].messages.addMessage(msg,i+firstFD);
            if (networkActions){
                ss = stringstream();
                struct in_addr dst = {msg->dst};
                ss << this_name << ": deliverMessage, msg delivered to (" << inet_ntoa(dst) << "," << ntohs(msg->dstPort) <<")" << endl;
                cout  << ss.str();
            }
            if (enterExit){
                ss = stringstream();
                ss << this_name << ": exit deliverMessage, msg delivered to socket" << endl;
                cout  << ss.str();
            }
            return;
        }
    }
    if (enterExit){
        ss = stringstream();
        ss << this_name << ": exit deliverMessage, no delivery, msg dropped" << endl;
        cout  << ss.str();
    }
    
    return;
}

//+
// Function: getNode
//
//  This returns the Node pointer for the node
// structure associated with the currren thread.
// It is called at the beginning of almost every function.
// The node map access is guarded by a mutext.
// Since it is used to generate data for entry exit messages
// it does not particpate in enter exit.
//
//-

static shared_ptr<Node> getNode(thread::id tid, string functionName){

    stringstream ss;
    if (tid == main_id) {
       // internal error of some sort should never reach here from the main thread.
	ss << "*** cannot call getNode from main thread " << tid << endl;
	cout << ss.str();
	exit(1);
    }
       
    map<thread::id,shared_ptr<Node>>::iterator nit;

    //ss = stringstream();
    //ss << "*** finding Node for " << tid << endl;
    //cout << ss.str();

    // maps are not thread safe
    lock_guard<mutex> guard(nodes_mutex);

    // then in the Node map
    nit = nodes.find(tid);
    if (nit != nodes.end()){
        //ss = stringstream();
        //ss << "*** name is " << it->second->name << endl;
        //cout << ss.str();

        return nit->second;
    }

    //if not found, error
    ss = stringstream();
    ss << "Node for thread id " << tid << " not found - Quitting" << endl;
    cout << ss.str();

    exit(1);
}

//+
// Function: getService
//
//  This returns the Serviece pointer for the service
// structure associated with the currren thread.
// It is called at the beginning of almost every function.
// Used to see if the service is still alive
// Since it is used to generate data for entry exit messages
// it does not particpate in enter exit.
//
//-

static shared_ptr<Service> getService(thread::id tid, string functionName){

    stringstream ss;
    if (tid == main_id) {
       // internal error of some sort should never reach here from the main thread.
	ss << "*** cannot call getService from main thread " << tid << endl;
	cout << ss.str();
	exit(1);
    }
       
    map<thread::id,shared_ptr<Service>>::iterator sit;

    //ss = stringstream();
    //ss << "*** finding Serivce for " << tid << endl;
    //cout << ss.str();

    // maps are not thread safe
    lock_guard<mutex> guard(nodes_mutex);

    // then in the Node map
    sit = allservices.find(tid);
    if (sit != allservices.end()){
        //ss = stringstream();
        //ss << "*** name is " << it->second->name << endl;
        //cout << ss.str();

        return sit->second;
    }

    //if not found, means the thread is running in a node (e.g. client)
    //ss = stringstream();
    //ss << "Serivice for thread id " << tid << " not found" << endl;
    //cout << ss.str();

    return nullptr;
}

//+
// Function: getName
//
//  This returns the name attached to the thread.
// the new system model has threads attached to a node instance
// if the node has client code, and attached to a service class
// instance if for servers.
//
//-

static string getName(thread::id tid){

    map<thread::id,string>::iterator nit;

    stringstream ss;
    //ss << "*** finding name for " << tid << endl;
    //cout << ss.str();

    // maps are not thread safe
    lock_guard<mutex> guard(nodes_mutex);

    // then in the Node map
    nit = names.find(tid);
    if (nit != names.end()){
        //ss = stringstream();
        //ss << "*** name is " << it->second->name << endl;
        //cout << ss.str();

        return nit->second;
    }

    //if not found, error
    ss = stringstream();
    ss << "name for thread id " << tid << " not found - Quitting" << endl;
    cout << ss.str();

    exit(1);
}

//+
// Function: get_random_float
//
// returns a rendom float between 0 and 1
//-

static float get_random_float(){
   static default_random_engine e;
   static uniform_real_distribution<> dis(0,1);
   return dis(e);
}

//+
// Function: get_random_range
// 
//   delayDist initialized when delayMin and delayMax
// set. returns a random number in that range
//-

static int get_random_range(){
    static default_random_engine e2;
    return floor(delayDist(e2));
}
