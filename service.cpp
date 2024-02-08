
#include "service.hpp"

#ifdef __APPLE__
#define MSG_CONFIRM 0
#endif

using namespace std;
using namespace string_literals;

#define close mclose

void KeyValueService::start(){

    cerr << "in ServiceServer::start" << endl;

    struct sockaddr_in servaddr, cliaddr;

    // get a socket to recieve messges
    if ( (sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0 ) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    // clear variables before initializing
    memset(&servaddr, 0, sizeof(servaddr));
    memset(&cliaddr, 0, sizeof(cliaddr));

    // Port and Interface information for binding the socket
    servaddr.sin_family    = AF_INET;        // IPv4
    servaddr.sin_addr.s_addr = INADDR_ANY;   // all interfaces
    servaddr.sin_port = htons(PORT);

    // Bind the socket with the server address
    if (::bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0 ) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    socklen_t len;
    int n;
    char clientStrBuffer[20];

    while(alive){ 
	cerr << "waiting for call from client" << endl;

        // wait for a message from a client
	len = sizeof(cliaddr);  //len is value/result
	n = recvfrom(sockfd, (uint8_t *)udpMessage, MAXMSG, 
		    MSG_WAITALL, ( struct sockaddr *) &cliaddr,
		    &len);
        cerr << "server received " << n << " bytes" << endl;
 	const char * clientstr = inet_ntop(AF_INET,&(cliaddr.sin_addr), clientStrBuffer,20);
	if (clientstr != nullptr){
        cerr << "  from address " << clientstr << endl;
	}
	std::cerr << HexDump{udpMessage,(uint32_t)n} << endl;

    }

    close(sockfd);
}
