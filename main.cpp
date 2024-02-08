/*+
 *  File:   main.cpp
 *
 *  Purpose:
 *      This module is the start driver for several of the ELEC 477 assignments.
 *      It initializes the
-*/
#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>
#include <string>


#include "network.hpp"
#include "server.hpp"
// #include "e1client.hpp"

extern std::map<std::thread::id,shared_ptr<Node>> nodes;
extern std::map<std::thread::id,string> names;

int main(int argc, char * argv[]){
    // handle command line arguments...
    int res = network_init(argc, argv);
    std::stringstream ss;

    // start all of the servers first. This will let them get up
    // and running before the client attempts to communicste
    std::cout << "Main: ************************************" << std::endl;
    std::cout << "Main: starting server" << std::endl;

    shared_ptr<Node> kvServer = make_shared<KeyValueServer>("kvServer");
    kvServer -> setAddress("10.0.0.2");
    kvServer -> startServices();

    std::cout << "Main: Server Started" << std::endl;
    // wait for servers to get up and running...
    std::this_thread::sleep_for(std::chrono::milliseconds(5000));


    std::cout << "Main: ************************************" << std::endl;
    std::cout << "Main: calling stop services on server" << std::endl;
    kvServer -> stopServices();
 
    return 0;
}

