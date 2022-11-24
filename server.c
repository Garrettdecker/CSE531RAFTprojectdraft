//#define _BSD_SOURCE

#include "defns.h"
#include <stdio.h>      /* for printf() and fprintf() */
#include <sys/socket.h> /* for socket() and bind() */
#include <sys/types.h>	// for open()
#include <sys/stat.h>	// for open()
#include <fcntl.h>	// for open()
#include <arpa/inet.h>  /* for sockaddr_in and inet_ntoa() */
#include <stdlib.h>     /* for atoi() and exit() */
#include <string.h>     /* for memset() */
#include <unistd.h>     /* for close() and execvp() */
#include <time.h>	// for time()
#include <math.h>	// for floor()
#include <sys/time.h>	// for timercmp(), timersub(), etc

#define CLIENTTABLEMAXSIZE 200
#define FILELOCKTABLEMAXSIZE 50
#define MAJORITY 2
#define LOGMAXSIZE 200
#define PROBFAILURE 0.00
#define RECOVERYTIME 5
#define BASETIMEOUTLENGTH 3
#define HEARTBEATINTERVAL 0.5
#define TICKINTERVAL 0.2

#define findMax(a,b) ((a) > (b) ? (a) : (b))
#define findMin(a,b) ((a) > (b) ? (b) : (a))


//variables

int sock;                        /* Socket */
struct sockaddr_in servAddr; /* Local address */
struct sockaddr_in clntAddr; /* Client address (or address of another server)*/
unsigned int cliAddrLen;         /* Length of incoming message */
struct sockaddr_in currClnt = {0};	//current clientaddr
struct request *req;             /* received client request */
struct response *resp;		//sent response to client
struct message mesRecv;		//received message
struct message mesResp;		//message response
unsigned short servPort;     /* Server port */
int servID;			//server id
int recvMsgSize;             /* Size of received message */
struct reqRespPair *clientTable[CLIENTTABLEMAXSIZE];
struct fileLock *fileLockTable[FILELOCKTABLEMAXSIZE];
int clientTableSize = 0;
int fileLockTableSize = 0;

//added for Raft:

int leaderID = -1;

struct timeval electionStartTime;	//time since start of leader election round
struct timeval electionTimeoutLength;	//length of leader election before timing out (currently chosen value)
					// ^^^ this is randomized in the range: (BASETIMEOUTLENGTH, 2*BASETIMEOUTLENGTH)
					// ^^^ actual value is re-chosen each time we reset the election timer
struct timeval lastHeartbeatTime;	//time at which last heartbeat was sent
struct timeval heartbeatInterval;	//time between sending heartbeats

//persistent state (write to disk before changing these)
//also read them from disk upon restart
int currentTerm;
int votedFor;
//this log will be loaded into from disk upon calling getLog()
//empty log entries shall have their terms = -1
struct logEntry servLog[LOGMAXSIZE] = {0};	//zero out

//volatile state
int commitIndex = -1;
int lastApplied = -1;
enum serverState state = FOLLOWER;

//reinitialize these after each election
//for each server...
int nextIndex[NUMSERVERS] = {0};	//index of next log entry to send to that server
int matchIndex[NUMSERVERS] = {-1};	//index of highest log entry known to be replicated on that server log

int aquiredVotes[NUMSERVERS] = {0};	//keeps track of which servers voted for this server
int numAquiredVotes = 0;		//keeps track of how many votes this server has aquired (including self)


//functions

void DieWithError(const char *errorMessage);
int getLastLogIndex();
void handleOpen(char *filename, char *mode);
void handleClose(char *filename);
void handleRead(char *filename, int n);
void handleWrite(char *filename, char *s);
void handleLseek(char *filename, int n);
struct reqRespPair *computeLastRequestHandled();
void sendClientResponse();
void sendServerResponse();
void sendServerRequest(int id);
void addToClientTable();
void addToLockTable(char *filename, int lockType, int fileDesc);
void garbageCollectClientTable();
void garbageCollectLockTable();
void performClientRequest();
void handleClientRequest(char *argv[]);
void handleMessage();
void handleRequestVoteRPC(struct requestVoteRPC rpc);
void handleRequestVoteResponse(struct requestVoteResponse resp);
void handleAppendEntriesRPC(struct appendEntriesRPC rpc);
void handleAppendEntriesResponse(struct appendEntriesResponse resp);
void addToLog(struct logEntry entries[ENTRIESLENGTH], int numEntries, int start);
void resetLeaderVariables();
void resetElectionTimer();
void getCurrentTerm();
void setCurrentTerm(int term);
void getVotedFor();
void setVotedFor(int val);
void getLog();
void setLog();
int compare (const void *x, const void *y);
int main(int argc, char *argv[]);


void DieWithError(const char *errorMessage) /* External error handling function */
{
	perror(errorMessage);
	exit(1);
}

//assumes log has already been loaded from disk (and zeroed out before loading)
int getLastLogIndex()
{
	int i = 0;
	while (i < LOGMAXSIZE)
	{
		if (servLog[i].term == -1)
		{
			return i-1;
		}
		i++;
	}
	printf("Error: Log capacity exceeded.");
	exit(5);
};

//fill out resp structure
void handleOpen(char *filename, char *mode)
{
	//compute full filename
	char fullFilename[100];
	strcpy(fullFilename, req->m);
	strcat(fullFilename, ":");
	strcat(fullFilename, filename);

	//check for conflicting lock
	int i = 0;
	while (i < fileLockTableSize)
	{
		
		//test whether file is locked
		if (strcmp(fullFilename, fileLockTable[i]->filename) == 0)
		{
			//file is read locked
			if (fileLockTable[i]->lockType == 1)
			{
				//check for conflict
				if (strcmp(mode, "write") == 0)
					goto conflict;
			}
			//file is write locked
			else if (fileLockTable[i]->lockType == 2)
			{
				goto conflict;
			}
		}
		i++;
	}

	
	//open file
	int fd;
	if (strcmp(mode, "write") == 0)
	{
		if ((fd = open(fullFilename, O_WRONLY, 0)) != -1)
		{
			addToLockTable(fullFilename, 2, fd);
			printf("File successfully opened for writing.\n");
		}
	}
	else if (strcmp(mode, "read") == 0)
	{
		if ((fd = open(fullFilename, O_RDONLY, 0)) != -1)
		{
			addToLockTable(fullFilename, 1, fd);
			printf("File successfully opened for reading.\n");
		}
		else
		{
			printf("Error opening file %s for reading. fd: %d\n", fullFilename, fd);
		}
	}
	else if (strcmp(mode, "readwrite") == 0)
	{
		if ((fd = open(fullFilename, O_RDWR, 0)) != -1)
		{
			addToLockTable(fullFilename, 2, fd);
			printf("File successfully opened for reading and writing.\n");
		}
	}
	else
	{
		DieWithError("Unknown file open mode");
	}
	
	resp->returnCode = fd;
	strcpy(resp->bytesRead, "");
	
	addToClientTable();
	sendClientResponse();

	return;

	conflict:
		printf("Error: File is locked.\n");
		resp->returnCode = -2;	//-2 signifies locked
		strcpy(resp->bytesRead, "");
}

void handleClose(char *filename)
{
	int returnCode = -1;	//in case there is no lock, in which case close() is never called

	//compute full filename
	char fullFilename[100];
	strcpy(fullFilename, req->m);
	strcat(fullFilename, ":");
	strcat(fullFilename, filename);

	//remove lock from lock table
	int i = 0;
	while (i < fileLockTableSize)
	{
		
		//find entries with same filename and clientNum
		if (strcmp(fullFilename, fileLockTable[i]->filename) == 0 && fileLockTable[i]->clientNum == req->c)
		{
			//close file
			returnCode = close(fileLockTable[i]->fileDesc);

			//future passes will skip this entry
			fileLockTable[i]->clientNum = -1;
			strcpy(fileLockTable[i]->filename, "");	
		}
		i++;
	}

	if (returnCode == -1)
	{
		printf("Error closing file.\n");
	}
	else if (returnCode == 0)
	{
		printf("File successfully closed.\n");
	}

	resp->returnCode = returnCode;
	strcpy(resp->bytesRead, "");
	
	addToClientTable();
	sendClientResponse();
}

void handleRead(char *filename, int n)
{
	//buffer for read data
	char data[80];

	int returnCode = -1;	//in case there is no lock, in which case read() is never called

	//compute full filename
	char fullFilename[100];
	strcpy(fullFilename, req->m);
	strcat(fullFilename, ":");
	strcat(fullFilename, filename);

	//check if this client owns a read or write lock on this file
	int i = 0;
	printf("locktable size: %d\n", fileLockTableSize);
	while (i < fileLockTableSize)
	{
		printf("locktable filename: %s\nlocktable clientNum: %d\n", fileLockTable[i]->filename, fileLockTable[i]->clientNum);
		//find entries with same filename and clientNum
		if (strcmp(fullFilename, fileLockTable[i]->filename) == 0 && fileLockTable[i]->clientNum == req->c)
		{
			//perform the read
			printf("Attempting to read from fd: %d\n", fileLockTable[i]->fileDesc);
			returnCode = read(fileLockTable[i]->fileDesc, data, n);
		}
		i++;
	}


	if (returnCode == -1)
	{
		printf("Error during read from file %s.\n", fullFilename);
	}
	else
	{
		printf("Read %s from file %s.\n", data, fullFilename);
	}

	resp->returnCode = returnCode;
	strcpy(resp->bytesRead, data);
	
	addToClientTable();
	sendClientResponse();
}

void handleWrite(char *filename, char *s)
{
	int returnCode = -1;	//in case there is no lock, in which case write() is never called

	//compute full filename
	char fullFilename[100];
	strcpy(fullFilename, req->m);
	strcat(fullFilename, ":");
	strcat(fullFilename, filename);

	int fd;
	//check if this client owns a write lock on this file
	int i = 0;
	while (i < fileLockTableSize)
	{
		//find entries with same filename and clientNum
		if (strcmp(fullFilename, fileLockTable[i]->filename) == 0 && fileLockTable[i]->clientNum == req->c)
		{
			//check if file is write locked
			if (fileLockTable[i]->lockType == 2)
			{
				//perform the write
				returnCode = write(fileLockTable[i]->fileDesc, s, strlen(s));
			}
		}
		i++;
	}


	if (returnCode == -1)
	{
		printf("Error during write to file.\n");
	}
	else
	{
		printf("Wrote %s to file.\n", s);
	}

	resp->returnCode = returnCode;
	strcpy(resp->bytesRead, "");
	
	addToClientTable();
	sendClientResponse();
}

void handleLseek(char *filename, int n)
{
	int returnCode = -1;	//in case there is no lock, in which case write() is never called

	//compute full filename
	char fullFilename[100];
	strcpy(fullFilename, req->m);
	strcat(fullFilename, ":");
	strcat(fullFilename, filename);

	int fd;
	//check if this client owns a write lock on this file
	int i = 0;
	while (i < fileLockTableSize)
	{
		//find entries with same filename and clientNum
		if (strcmp(fullFilename, fileLockTable[i]->filename) == 0 && fileLockTable[i]->clientNum == req->c)
		{
			//perform the lseek
			returnCode = lseek(fileLockTable[i]->fileDesc, n, SEEK_SET);
		}
		i++;
	}


	if (returnCode == -1)
	{
		printf("Error during lseek on file.\n");
	}
	else
	{
		printf("lseeked to offset %d from beginning of file\n", n);
	}

	resp->returnCode = returnCode;
	strcpy(resp->bytesRead, "");
	
	addToClientTable();
	sendClientResponse();
}

//computes the last request handled for the client that sent the current request
struct reqRespPair *computeLastRequestHandled()
{
	struct reqRespPair *lastPair;
	int maxR = -1;
	int i = 0;
	if (clientTableSize == 0)
	{
		return NULL;
	}
	while (i < clientTableSize)
	{
		//test whether the client at this index is the same as the client in the current request
		if (strcmp(clientTable[i]->req->m, req->m) == 0 && clientTable[i]->req->c == req->c)
		{
			//update maxRequest if necessary
			if (clientTable[i]->req->r > maxR)
			{
				maxR = clientTable[i]->req->r;
				lastPair = clientTable[i];
			}		
		}
		i++;
	}
	return lastPair;
}

void sendClientResponse()
{
	printf("Sending response to client.\n");
	
	//test if we need to set redirect
	if (leaderID != servID)
	{
		mesResp.redirect = leaderID;
	}

	mesResp.type = CLIENTRESP;
	mesResp.clientResp = *resp;

	printf("sizeof(mesResp) = %d.\n", (int) sizeof(mesResp));
	printf("about to send CLIENTRESP. currClnt port is: %d.\n", ntohs(currClnt.sin_port));
	printf("about to send CLIENTRESP. clntAddr port is: %d.\n", ntohs(clntAddr.sin_port));
	

	//send response datagram back to the client
	int numBytesSent = sendto(sock, &mesResp, sizeof(mesResp), 0, (struct sockaddr *) &currClnt, sizeof(currClnt));
	printf("numBytesSent = %d.\n", numBytesSent);
	if (numBytesSent != sizeof(mesResp))
		DieWithError("sendto() sent a different number of bytes than expected");
}

//clntAddr contains the address of the entity that we received our latest message from
//so we send our response back to clntAddr (even if it's a server instead of a client)
void sendServerResponse()
{
	printf("Sending a response to the server that sent us the current message.\n");

	//send message datagram reply back to server
	//clntAddr is the address of the entity that sent the received message, so this is not a mistake
	if (sendto(sock, &mesResp, sizeof(mesResp), 0, (struct sockaddr *) &clntAddr, sizeof(clntAddr)) != sizeof(mesResp))
		DieWithError("sendto() sent a different number of bytes than expected");
}

//send a request to server id
void sendServerRequest(int id)
{
	printf("Sending a request to server ID: %d.\n", id);

	//change clntAddr to the address of server id
	memset(&clntAddr, 0, sizeof(clntAddr));   /* Zero out structure */
	clntAddr.sin_family = AF_INET;                /* Internet address family */
	clntAddr.sin_addr.s_addr = htonl(INADDR_ANY); /* Any incoming interface */
	clntAddr.sin_port = htons(servPort - servID + id);      /* Port of server id */

	/* Set the size of the in-out parameter */
	cliAddrLen = sizeof(clntAddr);

	//send message datagram request to server id
	if (sendto(sock, &mesResp, sizeof(mesResp), 0, (struct sockaddr *) &clntAddr, sizeof(clntAddr)) != sizeof(mesResp))
		DieWithError("sendto() sent a different number of bytes than expected");
}

void addToClientTable()
{
	struct reqRespPair *pair = malloc(sizeof(struct reqRespPair));
	pair->req = req;
	pair->resp = resp;

	clientTable[clientTableSize] = pair;
	clientTableSize++;
}

//use req structure to help fill out lock table entry
void addToLockTable(char *filename, int lockType, int fileDesc)
{
	struct fileLock *lock = malloc(sizeof(struct fileLock));

	strcpy(lock->filename, filename);
	strcpy(lock->machineName, req->m);
	lock->clientNum = req->c;
	lock->incarnationNum = req->i;
	lock->lockType = lockType;
	lock->fileDesc = fileDesc;

	fileLockTable[fileLockTableSize] = lock;
	fileLockTableSize++;
}

void garbageCollectClientTable()
{
	int currentIncarnation = req->i;
	int i = 0;
	while (i < clientTableSize)
	{
		//test whether the client at this index is the same as the client in the current request
		if (strcmp(clientTable[i]->req->m, req->m) == 0 && clientTable[i]->req->c == req->c)
		{
			//nullify entries with lower incarnation number
			if (clientTable[i]->req->i < currentIncarnation)
			{
				//future passes will skip this entry
				clientTable[i]->req->c = -1;
			}		
		}
		i++;
	}
}

void garbageCollectLockTable()
{
	int currentIncarnation = req->i;
	int i = 0;
	while (i < fileLockTableSize)
	{
		//test whether the client at this index is the same as the client in the current request
		if (strcmp(fileLockTable[i]->machineName, req->m) == 0 && fileLockTable[i]->clientNum == req->c)
		{
			//nullify entries with lower incarnation number
			if (fileLockTable[i]->incarnationNum < currentIncarnation)
			{
				//future passes will skip this entry
				fileLockTable[i]->clientNum = -1;
				strcpy(fileLockTable[i]->filename, "");
				//close file
				close(fileLockTable[i]->fileDesc);
			}
		}
		i++;
	}
}

void performClientRequest()
{
	//garbage collect based on incarnation number before performing request
	//purge entries with same client and lesser incarnation number
	garbageCollectClientTable();
	garbageCollectLockTable();

	//parse request operation
	char line[80];
	strcpy(line, req->operation);
	char first[10];
	char second[80];
	char third[80];

	printf("Parsing operation: %s\n", line);

	//get first word of request operation
	strcpy(first, strtok(line, " "));

	if (strcmp(first, "close") != 0)
	{
		//get second word
		strcpy(second, strtok(NULL, " "));

		//get third word
		strcpy(third, strtok(NULL, "\n"));
	}
	else
	{
		//get second word
		strcpy(second, strtok(NULL, "\n"));
	}

	//fill out part of resp structure
	strcpy(resp->responseType, first);
	resp->responseNum = req->r;

	if (strcmp(first, "open") == 0)
	{
		handleOpen(second, third);
	}
	else if (strcmp(first, "close") == 0)
	{
		handleClose(second);
	}
	else if (strcmp(first, "read") == 0)
	{
		handleRead(second, atoi(third));
	}
	else if (strcmp(first, "write") == 0)
	{
		handleWrite(second, third);
	}
	else if (strcmp(first, "lseek") == 0)
	{
		handleLseek(second, atoi(third));
	}
}

void handleClientRequest(char *argv[])
{
	//malloc request and response structures
	//req = malloc(sizeof(struct request));
	//resp = malloc(sizeof(struct response));
	//*req = mesRecv.clientReq;

	printf("Server handling client request operation: %s\n", req->operation );

	struct reqRespPair *lastPair = malloc(sizeof(struct reqRespPair));
	lastPair = computeLastRequestHandled();
	int R;
	if (lastPair == NULL)
	{
		R = -1;
	}
	else
	{
		R = lastPair->req->r;
	}
	printf("R = %d\n", R);
	printf("req->r = %d\n", req->r);

	if (req->r < R)
	{
		//ignore the request, already completed
		printf("Ignoring the request since it was already completed.\n");

		free(req);
		free(resp);
		return;
	}
	else if (req->r == R)
	{
		//handle repeated request
		printf("Request not new. Response is stored.\n");

		//send stored response to client
		free(req);
		free(resp);
		resp = lastPair->resp;
		
		printf("Sending stored response to client: %s\nReturn code: %d\n", resp->bytesRead, resp->returnCode);

		sendClientResponse();
	}
	else if (req->r > R)
	{
		//handle new request
		printf("Request is new.\n");

		/* remove intentional udp packet drop
		// replaced by chance of server failure after each command

		//compute random number
		time_t t;
		srand((unsigned) time(&t));
		int choice = rand() % 3;

		
		//one third of the time, drop the request, do nothing
		if (choice == 0)
		{
			printf("Simulating communication failure by dropping request.\n");
			free(req);
			free(resp);
			return;
		}
		//perform the request, drop the reply
		else if (choice == 1)
		{
			printf("Simulating communication failure by performing request and dropping reply.\n");
			performClientRequest();
		}
		else if (choice == 2)
		*/
		{
			//printf("No communication failure.\n");
			performClientRequest();
			sendClientResponse();
		}
		
		//chance for server to fail
		//compute random number
		time_t t;
		srand((unsigned) time(&t));
		int num = rand() % 100000;
		if (num < PROBFAILURE * 100000)
		{
			close(sock);
			//wait for a bit to simulate failed server
			printf("Sleeping for %d seconds to simulate server failure.\n", RECOVERYTIME);
			fflush(stdout);
			sleep(RECOVERYTIME);
			//restart this program using the same arguments
			printf("Restarting program using same arguments.\n");
			fflush(stdout);
        		execvp(argv[0],argv);
		}
	}
}

void handleMessage()
{
	switch (mesRecv.type)
	{
		case CLIENTREQ:
			printf("Handling a client request.\n");
			
			printf("BEFORE: currClnt port is: %d.\n", ntohs(currClnt.sin_port));
			printf("BEFORE: clntAddr port is: %d.\n", ntohs(clntAddr.sin_port));

			//memset(&currClnt, 0, sizeof(currClnt));   //zero out
			//currClnt = clntAddr;

			printf("AFTER: currClnt port is: %d.\n", ntohs(currClnt.sin_port));
			printf("AFTER: clntAddr port is: %d.\n", ntohs(clntAddr.sin_port));
			

			//check server state
			//only give full response to client request if we are leader
			if (state == LEADER)
			{
				//malloc request structure
				req = malloc(sizeof(struct request));
				*req = mesRecv.clientReq;
				printf("Server receives client request operation (storing in log): %s\n", req->operation );

				int index = getLastLogIndex() + 1;	//get index of where we will store into log
				if (index >= LOGMAXSIZE)
				{
					printf("Error: Log capacity exceeded.");
					exit(8);
				}

				//store into log
				servLog[index].term = currentTerm;
				servLog[index].clientReq = mesRecv.clientReq;
				//write log to disk (persistent state)
				setLog();
			}
			//if we are a follower, redirect to leader 
			else if (state == FOLLOWER)
			{
				printf("Replying to redirect to leader.\n");
				//so that we do not error at the ... = *resp in sendClientResponse()
				resp = malloc(sizeof(struct response));
				mesResp.redirect = votedFor;
			}
			else if (state == CANDIDATE)
			{
				printf("Refusing to respond to client because we are a candidate.\n");
			}
			break;
		case CLIENTRESP:
			//server will never receive CLIENTRESP, so skip
			printf("Error: server received message intended for client.");
			exit(3);
		case VOTERPC:
			printf("Handling a requestVoteRPC.\n");

			handleRequestVoteRPC(mesRecv.voteRPC);
			break;
		case VOTERESP:
			printf("Handling a requestVoteResponse.\n");

			handleRequestVoteResponse(mesRecv.voteResp);
			//make sure we are still in candidate state
			if (state != CANDIDATE)
			{
				printf("We are no longer in candidate state, so we will ignore this requestVoteResponse.\n");
				return;
			}
			//check whether we now have the majority vote
			//we check for candidate state in case we already became leader (do not send double heartbeats)
			//actually, candidate state is checked for in handleRequestVoteResponse() before incrementing votes
			if (numAquiredVotes >= MAJORITY)
			{
				//convert to leader
				printf("Converting to leader.\n");
				state = LEADER;
				leaderID = servID;

				//initialize nextIndex[] and matchIndex[]
				int lastLogIndex = getLastLogIndex();
				for (int i = 0; i < NUMSERVERS; i++)
				{
					if (i != servID)
					{
						nextIndex[i] = lastLogIndex + 1;
						matchIndex[i] = -1;
					}
				}
				//memset(nextIndex, lastLogIndex + 1, sizeof(int)*NUMSERVERS);
				//memset(matchIndex, 0, sizeof(int)*NUMSERVERS);

				//ensure heartbeats will be sent out on next iteration of for loop in main()
				lastHeartbeatTime.tv_sec = (int) (HEARTBEATINTERVAL * 1000);
				lastHeartbeatTime.tv_usec = 0;
			}
			printf("Still waiting for more votes.\n");
			//otherwise, just keep waiting for votes
			break;
		case ENTRIESRPC:
			printf("Handling an appendEntriesRPC.\n");

			handleAppendEntriesRPC(mesRecv.entriesRPC);
			break;
		case ENTRIESRESP:
			printf("Handling an appendEntriesResponse.\n");

			handleAppendEntriesResponse(mesRecv.entriesResp);
			break;
	}
}

void handleRequestVoteRPC(struct requestVoteRPC rpc)
{
	mesResp.type = VOTERESP;
	mesResp.voteResp.serverID = servID;

	//if requesting server is on a lower term
	if (rpc.term < currentTerm)
	{
		//respond false and with the currentTerm so other server can update itself
		mesResp.voteResp.term = currentTerm;
		mesResp.voteResp.voteGranted = 0;
		printf("Rejecting vote for candidate ID %d because we are on a higher term.\n", rpc.candidateID);
	}
	//if requesting server is on same or higher term
	else if (rpc.term >= currentTerm)
	{
		//if requesting server is on a higher term
		if (rpc.term > currentTerm)
		{
			printf("Found a requestVoteRPC with a higher term. Updating.\n");
			//then update our currentTerm
			currentTerm = rpc.term;

			//write currentTerm to disk (persistent state)
			setCurrentTerm(currentTerm);

			//reset election timer
			printf("Resetting election timer.\n");
			resetElectionTimer();

			//reset some other variables related to leaders
			resetLeaderVariables();

			//change to follower state
			printf("Converting to follower.\n");
			state = FOLLOWER;
			leaderID = -1;
		}

		//if we have not yet voted (or already voted for the requesting server)
		//or our vote was reset due to requesting server having higher term
		if (votedFor == -1 || votedFor == rpc.candidateID)
		{
			//check if candidate's log is at least as up to date as ours
			int lastLogIndex = getLastLogIndex();
			if (lastLogIndex == -1)	//empty log
			{
				//then candidate's log is at least as up to date as ours
				mesResp.voteResp.term = currentTerm;
				mesResp.voteResp.voteGranted = 1;	//grant vote
				votedFor = rpc.candidateID;
				printf("Granting vote to server ID: %d.\n", votedFor);

				//write votedFor to disk (persistent state)
				setVotedFor(votedFor);

				//reset election timer
				printf("Resetting election timer.\n");
				resetElectionTimer();
			}
			else
			{
				if (rpc.lastLogTerm > servLog[lastLogIndex].term)
				{
					//then candidate's log is more up to date than ours
					mesResp.voteResp.term = currentTerm;
					mesResp.voteResp.voteGranted = 1;	//grant vote
					votedFor = rpc.candidateID;
					printf("Granting vote to server ID: %d.\n", votedFor);

					//write votedFor to disk (persistent state)
					setVotedFor(votedFor);

					//reset election timer
					printf("Resetting election timer.\n");
					resetElectionTimer();
				}
				else if (rpc.lastLogTerm == servLog[lastLogIndex].term)
				{
					//then check which log is longer	
					if (rpc.lastLogIndex >= lastLogIndex)
					{
						//then candidate's log is at least as up to date as ours
						mesResp.voteResp.term = currentTerm;
						mesResp.voteResp.voteGranted = 1;	//grant vote
						votedFor = rpc.candidateID;
						printf("Granting vote to server ID: %d.\n", votedFor);

						//write votedFor to disk (persistent state)
						setVotedFor(votedFor);

						//reset election timer
						printf("Resetting election timer.\n");
						resetElectionTimer();
					}
					else
					{
						//then candidate's log is less up to date than ours
						mesResp.voteResp.term = currentTerm;
						mesResp.voteResp.voteGranted = 0;	//reject vote
						printf("Rejecting vote for candidate ID %d because our log is more recent.\n", rpc.candidateID);
					}
				}
				else
				{
					//then candidate's log is less up to date than ours
					mesResp.voteResp.term = currentTerm;
					mesResp.voteResp.voteGranted = 0;	//reject vote
					printf("Rejecting vote for candidate ID %d because our log is more recent.\n", rpc.candidateID);
				}
			}
		}
		//we have already voted (including if we are leader)
		else
		{
			//then we reject the candidate's request for a vote
			mesResp.voteResp.term = currentTerm;
			mesResp.voteResp.voteGranted = 0;	//reject vote
			printf("Rejecting vote for candidate ID %d because we already voted in this term.\n", rpc.candidateID);
		}
	}

	sendServerResponse();
}

void handleRequestVoteResponse(struct requestVoteResponse resp)
{
	//if responding server is on a higher term
	if (resp.term > currentTerm)
	{
		printf("Received a requestVoteResponse with a higher term. Updating.\n");
		//update term (and on disk)
		currentTerm = resp.term;
		setCurrentTerm(currentTerm);

		//reset election timer
		printf("Resetting election timer.\n");
		resetElectionTimer();

		//reset some other variables related to leaders
		resetLeaderVariables();

		//convert to follower
		printf("Converting to follower.\n");
		state = FOLLOWER;
		leaderID = -1;
	}
	else if (resp.term == currentTerm)
	{
		//make sure we are still in candidate state
		if (state != CANDIDATE)
		{
			return;
		}
		//check if we aquired the vote
		if (resp.voteGranted == 1)
		{
			//check whether this vote is a new vote
			if (aquiredVotes[resp.serverID] == 0)
			{
				numAquiredVotes++;
			}
			aquiredVotes[resp.serverID] = 1;
		}
	}
	//otherwise, this response is from an older term, so ignore

	//whether we have the majority vote or not will be checked once we return back to handleMessage()
}

void handleAppendEntriesRPC(struct appendEntriesRPC rpc)
{
	mesResp.type = ENTRIESRESP;
	mesResp.entriesResp.serverID = servID;

	//if requesting server is on a lower term
	if (rpc.term < currentTerm)
	{
		printf("Received appendEntriesRPC with on a lower term. Responding with current term.\n");
		//respond false and with the currentTerm so other server can update itself
		mesResp.entriesResp.term = currentTerm;
		mesResp.entriesResp.success = 0;
	}
	//if requesting server is on same or higher term
	else if (rpc.term >= currentTerm)
	{
		//if requesting server is on a higher term
		if (rpc.term > currentTerm)
		{
			printf("Received an appendEntriesRPC with a higher term. Updating.\n");
			//then update our currentTerm
			currentTerm = rpc.term;

			//write currentTerm to disk (persistent state)
			setCurrentTerm(currentTerm);
		}

		//reset election timer
		printf("Resetting election timer.\n");
		resetElectionTimer();

		//reset some other variables related to leaders
		resetLeaderVariables();

		//change to follower state
		printf("Converting to follower.\n");
		state = FOLLOWER;
		leaderID = rpc.leaderID;

		//check if rpc.entries[] is empty (check rpc.numEntries)
		if (rpc.numEntries == 0)
		{
			printf("Received heartbeat from server ID: %d.\n", rpc.leaderID);
			//if empty, then no need to respond to heartbeat
			//it is only to maintain leader authority by preventing new elections
			return;
		}

		//check if log[] contains an entry at prevLogIndex whose term matches prevLogTerm

		if (rpc.prevLogIndex == -1)	//to avoid accessing servLog[-1]
		{
			goto skip1;
		}
		int lastLogIndex = getLastLogIndex();
		if (rpc.prevLogIndex <= lastLogIndex && servLog[rpc.prevLogIndex].term == rpc.prevLogTerm)
		{
			skip1:

			//logs match up until at least prevLogIndex
			//combine rpc.entries[] with servLog[], maintaining consistency
			//returns index of last new entry
			addToLog(rpc.entries, rpc.numEntries, rpc.prevLogIndex + 1);

			int lastNewEntryIndex = rpc.prevLogIndex + rpc.numEntries;
			if (rpc.leaderCommit > commitIndex)
			{
				commitIndex = findMin(rpc.leaderCommit, lastNewEntryIndex);
			}
			
			//reply success
			mesResp.entriesResp.term = currentTerm;
			mesResp.entriesResp.success = 1;
		}
		//servLog[] does NOT contain an entry at prevLogIndex and prevLogTerm
		else
		{
			//reply false
			mesResp.entriesResp.term = currentTerm;
			mesResp.entriesResp.success = 0;
			//include this so leader can decrement nextIndex and resend
			mesResp.entriesResp.nextIndex = rpc.prevLogIndex;	//this is already decremented
		}
	}
	
	sendServerResponse();
}

void handleAppendEntriesResponse(struct appendEntriesResponse resp)
{
	//if responding server is on a higher term
	if (resp.term > currentTerm)
	{
		//update term (and on disk)
		currentTerm = resp.term;
		setCurrentTerm(currentTerm);

		//reset election timer
		resetElectionTimer();

		//reset some other variables related to leaders
		resetLeaderVariables();

		//convert to follower
		printf("Converting to follower.\n");
		state = FOLLOWER;
		leaderID = -1;
	}
	else if (resp.term == currentTerm)
	{
		//make sure we are still in leader state
		if (state != LEADER)
		{
			return;
		}
		
		//check if appendEntriesRPC was successful
		if (resp.success == 1)
		{
			//update nextIndex and matchIndex for follower
			nextIndex[resp.serverID] = findMax(nextIndex[resp.serverID], resp.nextIndex);
			matchIndex[resp.serverID] = findMax(matchIndex[resp.serverID], resp.matchIndex);
		}
		//otherwise, unsuccessful, so resend with decremented nextIndex
		else
		{
			nextIndex[resp.serverID] = resp.nextIndex;	//this has already been decremented
			
			mesResp.type = ENTRIESRPC;
			mesResp.entriesRPC.term = currentTerm;
			mesResp.entriesRPC.leaderID = servID;
			mesResp.entriesRPC.prevLogIndex = nextIndex[resp.serverID] - 1;
			mesResp.entriesRPC.prevLogTerm = servLog[mesResp.entriesRPC.prevLogIndex].term;
			int lastLogIndex = getLastLogIndex();
			int num = findMin(ENTRIESLENGTH, lastLogIndex - mesResp.entriesRPC.prevLogIndex);
			mesResp.entriesRPC.numEntries = num;
			memcpy(mesResp.entriesRPC.entries, &servLog[nextIndex[resp.serverID]], sizeof(struct logEntry)*num);
			mesResp.entriesRPC.leaderCommit = commitIndex;

			goto resend;
		}
	}
	//otherwise, this response is from an older term, so ignore
	return;

	resend:
	sendServerResponse();
}

//go through the servLog[], starting at prevLogIndex + 1,
//making sure the indices and terms match with rpc.entries[].
//if an entry does not match, delete it and all that follow from servLog[],
//then append any new entries not already in the servLog[]
void addToLog(struct logEntry entries[ENTRIESLENGTH], int numEntries, int start)
{
	printf("Combining servLog[] with %d entries.", numEntries);
	if (start + numEntries >= LOGMAXSIZE)
	{
		printf("Error: Log capacity exceeded.");
		exit(7);
	}

	int i = 0;
	for (i = 0; i < numEntries; i++)
	{
		//if an entry does not match
		if (entries[i].term != servLog[start + i].term)
		{
			//delete it and all that follow from servLog[]
			int j = i;
			while (servLog[start + j].term != -1)
			{
				servLog[start + j].term = -1;
				j++;
			}
			break;
		}
	}
	//replace them with entries[] after we delete
	while (i < numEntries)
	{
		servLog[start + i] = entries[i];
		i++;
	}
	
	//write log to disk (persistent state)
	setLog();
}

//reset variables related to leader election
void resetLeaderVariables()
{
	memset(aquiredVotes, 0, sizeof(int)*NUMSERVERS);
	numAquiredVotes = 0;
	votedFor = -1;
	setVotedFor(votedFor);
	for (int i = 0; i < NUMSERVERS; i++)
	{
		if (i != servID)
		{
			nextIndex[i] = -1;
			matchIndex[i] = -1;
		}
	}
	//memset(nextIndex, -1, sizeof(int)*NUMSERVERS);	//set to leader lastLogIndex + 1 if we become leader again
	//memset(matchIndex, -1, sizeof(int)*NUMSERVERS);	//set to 0 if we become leader again
}

//resets the election timer by recording the electionStartTime
//also re-chooses the electionTimeoutLength
void resetElectionTimer()
{
	//reset electionStartTime
	gettimeofday(&electionStartTime, NULL);
	//re-choose electionTimeoutLength randomly in range: (BASETIMEOUTLENGTH, 2*BASETIMEOUTLENGTH)
	double r = (double) rand() / RAND_MAX;
	r = BASETIMEOUTLENGTH + r * BASETIMEOUTLENGTH;
	electionTimeoutLength.tv_sec = (int) r;
	electionTimeoutLength.tv_usec = 1000000 * (r - ((int) r));
}

//reads currentTerm from disk
void getCurrentTerm()
{
	FILE *in, *out; 
	char filename[20] = "currentTerm";
	char id[2];
	sprintf(id, "%d", servID);
	strcat(filename, id);

	if( access( filename, F_OK ) != -1 )
	{
		//file exists
		in = fopen(filename, "r"); 
		if (in == NULL)
		{
			printf("\nError opening %s\n", filename); 
			exit(1); 
		}

		if (fscanf(in, "%d", &currentTerm) != 0)
		{
			printf("\nSuccessfully read from %s\n", filename);
		}
		else
		{
			printf("\nError reading from %s\n", filename);
			exit(2);
		}

		fclose(in);
	}
	else
	{
		//file does not exist, create new one with currentTerm = 0
		currentTerm = 0;

		out = fopen(filename, "w"); 
		if (out == NULL)
		{
			printf("\nError opening %s\n", filename); 
			exit(1); 
		}

		//write currentTerm = 0 to disk
		if (fprintf(out, "%d", currentTerm) != 0)
		{
			printf("\nSuccessfully wrote to %s\n", filename);
		}
		else
		{
			printf("\nError writing to %s\n", filename);
			exit(2);
		}

		fclose(out);
	}
}

//writes currentTerm to disk
void setCurrentTerm(int term)
{
	FILE *out; 
	char filename[20] = "currentTerm";
	char id[2];
	sprintf(id, "%d", servID);
	strcat(filename, id);

	out = fopen(filename, "w"); 
	if (out == NULL)
	{
		printf("\nError opening %s\n", filename); 
		exit(1); 
	}

	if (fprintf(out, "%d", term) != 0)
	{
		printf("\nSuccessfully wrote to %s\n", filename);
	}
	else
	{
		printf("\nError writing to %s\n", filename);
		exit(2);
	}

	fclose(out);
}

//reads votedFor from disk
void getVotedFor()
{
	FILE *in, *out; 
	char filename[20] = "votedFor";
	char id[2];
	sprintf(id, "%d", servID);
	strcat(filename, id);

	if( access( filename, F_OK ) != -1 )
	{
		//file exists
		in = fopen(filename, "r"); 
		if (in == NULL)
		{
			printf("\nError opening %s\n", filename); 
			exit(1); 
		}

		if (fscanf(in, "%d", &votedFor) != 0)
		{
			printf("\nSuccessfully read from %s\n", filename);
		}
		else
		{
			printf("\nError reading from %s\n", filename);
			exit(2);
		}

		fclose(in);
	}
	else
	{
		//file does not exist, create new one with votedFor = -1
		votedFor = -1;

		out = fopen(filename, "w"); 
		if (out == NULL)
		{
			printf("\nError opening %s\n", filename); 
			exit(1); 
		}

		//write votedFor = -1 to disk
		if (fprintf(out, "%d", votedFor) != 0)
		{
			printf("\nSuccessfully wrote to %s\n", filename);
		}
		else
		{
			printf("\nError writing to %s\n", filename);
			exit(2);
		}

		fclose(out);
		
	}
}

//writes votedFor to disk
void setVotedFor(int val)
{
	FILE *out; 
	char filename[20] = "votedFor";
	char id[2];
	sprintf(id, "%d", servID);
	strcat(filename, id);

	out = fopen(filename, "w"); 
	if (out == NULL)
	{
		printf("\nError opening %s\n", filename); 
		exit(1); 
	}

	if (fprintf(out, "%d", val) != 0)
	{
		printf("\nSuccessfully wrote to %s\n", filename);
	}
	else
	{
		printf("\nError writing to %s\n", filename);
		exit(2);
	}

	fclose(out);
}

//reads log from disk
//if no log is on disk, initialize an empty log (with term = -1)
void getLog()
{
	FILE *in, *out;
	char filename[20] = "log";
	char id[2];
	sprintf(id, "%d", servID);
	strcat(filename, id);

	if( access( filename, F_OK ) != -1 )
	{
		//file exists
		in = fopen(filename, "r"); 
		if (in == NULL)
		{
			printf("\nError opening %s\n", filename); 
			exit(1); 
		}

		if (fread(servLog, sizeof(struct logEntry), LOGMAXSIZE, in) != 0)
		{
			printf("\nSuccessfully read from %s\n", filename);
		}
		else
		{
			printf("\nError reading from %s\n", filename);
			exit(2);
		}

		fclose(in);
	}
	else
	{
		//file does not exist, create new one with empty log (with term = -1)
		//servLog is already zeroed out (at top of program)
		for (int i = 0; i < LOGMAXSIZE; i++)
		{
			servLog[i].term = -1;
		}

		out = fopen(filename, "w");
		if (out == NULL)
		{
			printf("\nError opening %s\n", filename); 
			exit(1); 
		}

		//write empty log to disk
		if (fwrite(servLog, sizeof(struct logEntry), LOGMAXSIZE, out) != 0)
		{
			printf("\nSuccessfully wrote to %s\n", filename);
		}
		else
		{
			printf("\nError writing to %s\n", filename);
			exit(2);
		}

		fclose(out);
	}
}

//writes servLog[] to disk
void setLog()
{
	FILE *out; 
	char filename[20] = "log";
	char id[2];
	sprintf(id, "%d", servID);
	strcat(filename, id);

	out = fopen(filename, "w");
	if (out == NULL)
	{
		printf("\nError opening %s\n", filename); 
		exit(1); 
	}

	if (fwrite(servLog, sizeof(struct logEntry), LOGMAXSIZE, out) != 0)
	{
		printf("\nSuccessfully wrote to %s\n", filename);
	}
	else
	{
		printf("\nError writing to %s\n", filename);
		exit(2);
	}

	fclose(out);
}

//for use with qsort
int compare (const void *x, const void *y)
{
	return (*(int*)x - *(int*)y);
}


int main(int argc, char *argv[])
{

	if (argc != 3)         /* Test for correct number of parameters */
	{
		fprintf(stderr,"Usage:  %s <UDP PORT OF SERVER ID 0> <SERVER ID>\n", argv[0]);
		exit(1);
	}

	/* First arg:  local port of server 0 */
	/* Second arg: server ID of this server */
	servPort = atoi(argv[1]) + atoi(argv[2]);	//port of this server
	servID = atoi(argv[2]);

	/* Create socket for sending/receiving datagrams */
	if ((sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
		DieWithError("socket() failed");

	/* Construct local address structure */
	memset(&servAddr, 0, sizeof(servAddr));   /* Zero out structure */
	servAddr.sin_family = AF_INET;                /* Internet address family */
	servAddr.sin_addr.s_addr = htonl(INADDR_ANY); /* Any incoming interface */
	servAddr.sin_port = htons(servPort);      /* Local port */

	/* Bind to the local address */
	if (bind(sock, (struct sockaddr *) &servAddr, sizeof(servAddr)) < 0)
		DieWithError("bind() failed");

	//try putting sock into non-blocking mode
	if(fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK) < 0)
	{
		// handle error
		printf("Error: failed to put socket into non-blocking mode.");
		exit(6);
	}

	//initialize persistent state variables from disk
	printf("Initializing persistent state variables from disk.\n");
	getCurrentTerm();
	getVotedFor();
	getLog();


	for (;;) /* Run forever */
	{
		usleep((int)(TICKINTERVAL*1000000));

		//printf("Does recvfrom block?\n");
		printf("CurrentTerm is %d.\n", currentTerm);
		printf("LastLogIndex is %d.\n", getLastLogIndex());
		printf("CommitIndex is %d.\n", commitIndex);
		printf("LastApplied is %d.\n", lastApplied);
		printf("LeaderID is %d.\n", leaderID);
		if (state == LEADER)
		{
			printf("Current state is leader.\n");
			for (int i = 0; i < NUMSERVERS; i++)
			{
				printf("NextIndex[%d] = %d.\n", i, nextIndex[i]);
			}
		}
		else if (state == FOLLOWER)
		{
			printf("Current state is follower.\n");
		}
		else if (state == CANDIDATE)
		{
			printf("Current state is candidate.\n");
		}


		struct timeval currentTime, diffTime;
		gettimeofday(&currentTime, NULL);
		timersub(&currentTime, &electionStartTime, &diffTime);
		//if we are not leader, check if electionTimeout should occur
		if (state != LEADER && timercmp(&diffTime, &electionTimeoutLength, >) != 0)
		{
			//electionTimeout occurring
			printf("Election timeout reached.\n");
			//convert to candidate state
			printf("Converting to candidate.\n");
			state = CANDIDATE;
			resetLeaderVariables();
			currentTerm++;
			setCurrentTerm(currentTerm);
			votedFor = servID;
			setVotedFor(votedFor);
			aquiredVotes[servID] = 1;
			numAquiredVotes++;
			resetElectionTimer();

			//construct requestVoteRPC
			mesResp.type = VOTERPC;
			mesResp.voteRPC.term = currentTerm;
			mesResp.voteRPC.candidateID = servID;
			int index = getLastLogIndex();
			mesResp.voteRPC.lastLogIndex = index;
			mesResp.voteRPC.lastLogTerm = servLog[index].term;

			//send requestVoteRPCs to all other servers
			for (int i = 0; i < NUMSERVERS; i++)
			{
				if (i != servID)
				{
					sendServerRequest(i);
				}
			}
		}


		struct timeval heartbeatInterval;
		heartbeatInterval.tv_sec = 0;
		heartbeatInterval.tv_usec = (int) (HEARTBEATINTERVAL * 1000000);
		//if we are leader, check if heartbeats should be sent
		if (state == LEADER && timercmp(&lastHeartbeatTime, &heartbeatInterval, >) != 0)
		{
			//construct appendEntriesRPCs (heartbeats) with numEntries = 0
			mesResp.type = ENTRIESRPC;
			mesResp.entriesRPC.term = currentTerm;
			mesResp.entriesRPC.leaderID = servID;
			//prevLogIndex, prevLogTerm, and entries[] do not matter if numEntries is 0
			mesResp.entriesRPC.numEntries = 0;
			
			//send appendEntriesRPCs (heartbeats) to all other servers
			for (int i = 0; i < NUMSERVERS; i++)
			{
				if (i != servID)
				{
					printf("Sending heartbeat to server ID: %d.\n", i);
					sendServerRequest(i);
				}
			}
		}

		//if we are leader, update commitIndex (and matchIndex[servID]) if needed
		if (state == LEADER)
		{
			int median = -1;
			int vals[NUMSERVERS];
			matchIndex[servID] = getLastLogIndex();
		
			//copy matchIndex[] to vals[]
			memcpy(vals, matchIndex, sizeof(int)*NUMSERVERS);
			//sort vals[] array in order
			qsort(vals, NUMSERVERS, sizeof(int), compare);
			median = vals[NUMSERVERS/2];

			//median represents the highest N such that a majority of matchIndex[i] >= N
			//check if median is greater than commitIndex and servLog[N].term == currentTerm (for safety)
			//count down from median to commitIndex to see if any N satisfies both conditions
			while(median > commitIndex)
			{
				if (servLog[median].term == currentTerm)
				{
					//update commitIndex
					commitIndex = median;
					break;
				}
				median--;
			}
		}

		
		//if we are leader, check if any followers can be sent log entries (based on nextIndex[])
		if (state == LEADER)
		{
			int lastLogIndex = getLastLogIndex();
			for (int i = 0; i < NUMSERVERS; i++)
			{
				if (lastLogIndex >= nextIndex[i] && i != servID)
				{
					//we found a follower that we can send log entries to
					//construct appendEntriesRPC
					mesResp.type = ENTRIESRPC;
					mesResp.entriesRPC.term = currentTerm;
					mesResp.entriesRPC.leaderID = servID;
					int index = nextIndex[i] - 1;
					mesResp.entriesRPC.prevLogIndex = index;
					mesResp.entriesRPC.prevLogTerm = servLog[index].term;
					int num = findMin(ENTRIESLENGTH, lastLogIndex - mesResp.entriesRPC.prevLogIndex);
					mesResp.entriesRPC.numEntries = num;
					memcpy(mesResp.entriesRPC.entries, &servLog[nextIndex[i]], sizeof(struct logEntry)*num);
					mesResp.entriesRPC.leaderCommit = commitIndex;

					//send appendEntriesRPC to server
					sendServerRequest(i);
				}
			}
		}


		//check if we are able to apply any committed entries to the state machine
		while (commitIndex > lastApplied)
		{
			lastApplied++;
			//assign request structure according to next log entry to be applied
			req = malloc(sizeof(struct request));
			*req = servLog[lastApplied].clientReq;
			//memcpy(req, &(servLog[lastApplied].clientReq), sizeof(struct request));
			resp = malloc(sizeof(struct response));	//also prepare response structure

			printf("Apply the client request to the state machine.\n");
			handleClientRequest(argv);	//apply the client request to the state machine
		}


		//malloc request and response structures
		//now doing this in handleMessage()
		//req = malloc(sizeof(struct request));
		//resp = malloc(sizeof(struct response));

		/* Set the size of the in-out parameter */
		cliAddrLen = sizeof(clntAddr);

		//actually, we use non-blocking now
		if ((recvMsgSize = recvfrom(sock, &mesRecv, sizeof(mesRecv), 0, (struct sockaddr *) &clntAddr, &cliAddrLen)) < 0)
		{
			printf("No messages for recvfrom() to gather at the moment.\n");
		}
		else
		{
			printf("Server handling message from %s:%d\n", inet_ntoa( clntAddr.sin_addr ), (int) ntohs( clntAddr.sin_port ));

			if (mesRecv.client == 1)
			{
				currClnt = clntAddr;
				sleep(4);
				printf("Starting communication.\n");
				goto rest;
			}

			/*
			if (mesRecv.type == CLIENTREQ)
			{
				*req = mesRecv.clientReq;
				printf("got here.\n");
				//memcpy(req, &(mesRecv.clientReq), sizeof(struct request));
			}
			*/

			handleMessage();
			rest:
			usleep(5);
		}
	}
	/* NOT REACHED */
}
