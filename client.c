#include "defns.h"

#include <stdio.h>      /* for printf() and fprintf() */
#include <sys/socket.h> /* for socket(), connect(), sendto(), and recvfrom() */
#include <arpa/inet.h>  /* for sockaddr_in and inet_addr() */
#include <stdlib.h>     /* for atoi() and exit() */
#include <string.h>     /* for memset() */
#include <unistd.h>     /* for close() */
#include <sys/stat.h> 	//for open()
#include <fcntl.h>	//for open()
#include <time.h>	//for time()


//variables

int sock;                        /* Socket descriptor */
struct sockaddr_in servAddr[NUMSERVERS]; /* server address */
struct sockaddr_in fromAddr[NUMSERVERS];     /* Source address of echo */
unsigned short servPort[NUMSERVERS];     /* server port */
unsigned int fromSize;           /* In-out of address size for recvfrom() */
char *servIP;                    /* IP address of server */
char *scriptName;
int recvMsgSize;                 /* Size of received message */
struct request *req;             /* sent request */
struct response *resp;            /* received response */
struct message mes;
char *machineName;
int clientNum;
int requestNum = 0;
int incarnationNum;
char incarnationName[120];	//stores the name of the file used to check incarnationNum
char requestType[20];
int globRedirect = 0;
int defaultLeader = -1;


//functions

void DieWithError(const char *errorMessage);
void sendToRandomServer();
void sendToServer(int id);
void receiveFromServer(int id);
void handleResponse();
int getIncarnationNum();
void sendToAllServers();
void initResponse();
void initRequest();
int main(int argc, char *argv[]);


void DieWithError(const char *errorMessage) /* External error handling function */
{
	perror(errorMessage);
	exit(1);
}

void sendToRandomServer()
{
	//compute random number
	time_t t;
	srand((unsigned) time(&t));
	int choice = rand() % NUMSERVERS;
	int chance = rand() % 4;
	
	//bias towards defaultLeader
	if (chance == 0 || defaultLeader == -1)
	{
		//choose randomly
		printf("Randomly chose server ID %d to send to.\n", choice);
		sendToServer(choice);
	}
	else
	{
		//choose defaultLeader
		printf("Send to recent leader server ID %d to send to.\n", defaultLeader);
		sendToServer(defaultLeader);
	}
}

void sendToServer(int id)
{
	mes.type = CLIENTREQ;
	mes.clientReq = *req;
	//send the request to the server
    	if (sendto(sock, &mes, sizeof(mes), 0, (struct sockaddr *) &servAddr[id], sizeof(servAddr[id])) != sizeof(mes))
       		DieWithError("sendto() sent a different number of bytes than expected");

	printf("Client sent request %s with requestNum = %d to server ID %d.\n", mes.clientReq.operation, mes.clientReq.r, id);

	//wait for a response from the server
	receiveFromServer(id);
}

void receiveFromServer(int id)
{
	//set socket options to enforce a brief delay
	struct timeval tv;
	tv.tv_sec = 2;
	tv.tv_usec = 200000;
	if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
    		DieWithError("Could not set socket options");

	//receive a response
    	fromSize = sizeof(fromAddr[id]);
    	recvMsgSize = recvfrom(sock, &mes, sizeof(mes), 0, (struct sockaddr *) &fromAddr[id], &fromSize);
	if (recvMsgSize >= 0 && recvMsgSize != sizeof(mes))
        	DieWithError("recvfrom() received a different number of bytes than expected");

	printf("Client received message from server ID %d.\n", id);

	//check to see if the message told us to redirect towards a different server (redirect != id)
	if (mes.redirect != id)
	{
		defaultLeader = mes.redirect;
		globRedirect = 1;
		printf("Message told us to redirect to server ID %d.\n", mes.redirect);
		sendToServer(mes.redirect);
		return;		//return now because rest of message is empty
	}

	*resp = mes.clientResp;
	printf("ResponseNum = %d.\n", resp->responseNum);

	if (recvMsgSize < 0)
	{
		//request timed out, so resend to random server
		printf( "Request timed out.  Resending...\n");
		sendToRandomServer();
	}

	if (servAddr[id].sin_addr.s_addr != fromAddr[id].sin_addr.s_addr)
	{
		fprintf(stderr,"Error: received a packet from unknown source.\n");
		exit(1);
	}
}

//response is stored in *resp
void handleResponse()
{
	//first, check if the message told us to redirect towards a different server
	if (globRedirect == 1)
	{
		//we should redirect
		//so the response in *resp will be ignored
		//disable redirect
		globRedirect = 0;
		return;
	}

	if (req->r != resp->responseNum)
	{
		printf("req->r: %d\n", req->r);
		printf("resp->responseNum: %d\n", resp->responseNum);
		//probably an old, misordered packet, ignore it and receive the next one
		printf("response number does not match request number\n");
		//receiveFromServer();
		return;
	}

	//check if responseType was "open"
	if (strcmp(resp->responseType, "open") == 0)
	{
		if (resp->returnCode == -1)
		{
			printf("Error trying to open file.\n");
		}
		else
		{
			printf("Successfully opened file.\n");
		}
	}
	//check if responseType was "close"
	else if (strcmp(resp->responseType, "close") == 0)
	{
		if (resp->returnCode == -1)
		{
			printf("Error trying to close file.\n");
		}
		else
		{
			printf("Successfully closed file.\n");
		}
	}
	//check if responseType was "read"
	else if (strcmp(resp->responseType, "read") == 0)
	{
		if (resp->returnCode == -1)
		{
			printf("Error trying to read from file.\n");
		}
		else
		{
			//print out the bytes read
			printf("Successfully read %ld bytes: %s\n", strlen(resp->bytesRead), resp->bytesRead);
		}
	}
	else if (strcmp(resp->responseType, "write") == 0)
	{
		if (resp->returnCode == -1)
		{
			printf("Error trying to write to file.\n");
		}
		else
		{
			printf("Successfully wrote to file.\n");
		}
	}
	else if (strcmp(resp->responseType, "lseek") == 0)
	{
		if (resp->returnCode == -1)
		{
			printf("Error trying to lseek on file.\n");
		}
		else
		{
			printf("Successfully lseeked on file.\n");
		}
	}
}

int getIncarnationNum()
{
	//open incarnation file
	int fd;
	if ((fd = open(incarnationName, O_RDONLY, 0)) == -1)
		DieWithError("Could not open incarnation file");

	//lock incarnation file
	lockf(fd, F_LOCK, 0);

	//read incarnation number from file
	read(fd, &incarnationNum, sizeof(int));

	//unlock incarnation file by closing file
	lockf(fd, F_ULOCK, 0);
	close(fd);

	return incarnationNum;
}

void sendToAllServers()
{
	mes.client = 1;
	for (int i = 0; i < NUMSERVERS; i++)
	{
		//send the request to the server
	    	if (sendto(sock, &mes, sizeof(mes), 0, (struct sockaddr *) &servAddr[i], sizeof(servAddr[i])) != sizeof(mes))
	       		DieWithError("sendto() sent a different number of bytes than expected");
	}
	mes.client = 0;
}

void initRequest()
{
	//init request structure
	req = malloc(sizeof(struct request));
	strcpy(req->m, machineName);
	req->c = clientNum;
	req->r = 0;
	req->i = 0;
	strcpy(req->operation, "");
}

void initResponse()
{
	//init response structure
	resp = malloc(sizeof(struct response));
	strcpy(resp->responseType, "");
	resp->responseNum = 0;
	resp->returnCode = 0;
	strcpy(resp->bytesRead, "");
}

int main(int argc, char *argv[])
{
	if (argc < 6)    /* Test for correct number of arguments */
	{
		fprintf(stderr,"Usage: %s <Server IP address> <Machine Name> <Client Number> <Port Number> <Script File Name>\n", argv[0]);
		exit(1);
	}

	servIP = argv[1];           /* First arg: server IP address (dotted quad) */
	machineName = argv[2];
	clientNum = atoi(argv[3]);
	for (int i = 0; i < NUMSERVERS; i++)
	{
		servPort[i] = atoi(argv[4] + i);  /* port of server ID i */
	}
	scriptName = argv[5];

	printf( "Arguments passed: server IP %s, machine name %s, client number %d, server ID 0 port %d, script name %s\n", servIP, machineName, clientNum, servPort[0], scriptName );

	/* Create a datagram/UDP socket */

	if ((sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
	DieWithError("socket() failed");

	/* Construct the server address structure */

	memset(servAddr, 0, sizeof(struct sockaddr_in)*NUMSERVERS);    /* Zero out structure */
	for (int i = 0; i < NUMSERVERS; i++)
	{
		servAddr[i].sin_family = AF_INET;                 /* Internet addr family */
		servAddr[i].sin_addr.s_addr = inet_addr(servIP);  /* Server IP address */
		servAddr[i].sin_port   = htons(servPort[i]);     /* Server ID i port */
	}

	initRequest();

	initResponse();
	
	sendToAllServers();

	//get incarnation file name
	strcpy(incarnationName, machineName);
	strcat(incarnationName, ":incarnation");

	//check if incarnation file exists
	int fd;
	if( access( incarnationName, F_OK ) == -1 )
	{
		//file does not exist, so create it
		if ((fd = open(incarnationName, O_WRONLY | O_CREAT, 0)) == -1)
			DieWithError("Could not create incarnation file");
		//write an incarnation number of 0
		incarnationNum = 0;
		write(fd, &incarnationNum, 0);
		close(fd);
	}

	//open script file
	FILE *fp;
	if ((fp = fopen(scriptName, "r")) == NULL)
		DieWithError("Could not open script file");

	//read lines from script file
	char *line = NULL;
	size_t buffsize = 0;
	ssize_t numRead;
	while ((numRead = getline(&line, &buffsize, fp)) != -1)
	{
		//debug info
		printf("Retrieved line of length %ld:\n", numRead);
        	printf("%s", line);

		//update request structure
		req->r = requestNum;
		req->i = getIncarnationNum();
		strcpy(req->operation, line);
		requestNum++;

		//determine request type
		strcpy(requestType, strtok(line, " "));

		//this also receives the response, stored in resp
		sendToRandomServer();

		//handle the response located in resp
		handleResponse();
	}
	//close script file
	fclose(fp);
	//close socket
	close(sock);
	//free req and resp structures
	free(req);
	free(resp);

	exit(0);
}
