#define ENTRIESLENGTH 30
#define NUMSERVERS 3

enum mtype {CLIENTREQ, CLIENTRESP, VOTERPC, VOTERESP, ENTRIESRPC, ENTRIESRESP};
enum serverState {FOLLOWER, CANDIDATE, LEADER};

struct request;
struct response;
struct reqRespPair;
struct logEntry;
struct fileLock;
struct requestVoteRPC;
struct requestVoteResponse;
struct appendEntriesRPC;
struct appendEntriesResponse;
struct message;

struct request
{
	char m[24]; /* Name of machine on which client is running */
	int c; /* Client number */
	int r; /* Request number of client */
	int i; /* Incarnation number of client's machine */
	char operation[80]; /* File operation (actual request) client sends to server */
};

struct response
{
	char responseType[10];	//should be identical to the first word in the corresponding request
	int responseNum;	//equal to the request number of the corresponding request
	int returnCode;		//the return code of the file operation
	char bytesRead[80];	//in the case of a read request, the actual bytes read from file
};

struct reqRespPair
{
	struct request *req;
	struct response *resp;
};

struct logEntry
{
	int term;
	struct request clientReq;
};

struct fileLock
{
	char filename[100];
	char machineName[24];
	int clientNum;
	int incarnationNum;
	int lockType;	//1 = read, 2 = write
	int fileDesc;
};

struct requestVoteRPC
{
	int term;
	int candidateID;
	int lastLogIndex;
	int lastLogTerm;
};

struct requestVoteResponse
{
	int term;
	int voteGranted;
	int serverID;
};

struct appendEntriesRPC
{
	int term;
	int leaderID;
	int prevLogIndex;
	int prevLogTerm;
	struct logEntry entries[ENTRIESLENGTH];
	int numEntries;
	int leaderCommit;
};

struct appendEntriesResponse
{
	int term;
	int success;
	int serverID;
	int nextIndex;
	int matchIndex;
};

struct message
{
	enum mtype type;
	int redirect;
	int client;
	union
	{
		struct request clientReq;
		struct response clientResp;
		struct requestVoteRPC voteRPC;
		struct requestVoteResponse voteResp;
		struct appendEntriesRPC entriesRPC;
		struct appendEntriesResponse entriesResp;
	};
};

