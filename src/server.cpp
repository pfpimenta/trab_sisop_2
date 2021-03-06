#include "../include/server.hpp"

char payload[PAYLOAD_SIZE];

pthread_mutex_t termination_signal_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t packets_to_send_mutex = PTHREAD_MUTEX_INITIALIZER; // mutex for the FIFO for packets to send to backups

int next_session_id = 0;

// TODO mutex
int my_backup_id;
int num_backup_servers = 0; // never decrements. used for attributing new backup server_ids

// tables containing the server information
MasterTable* masterTable; // contains informations about the clients connected to the primary server
std::map<int, server_struct* > servers_table; // contains informations about the backup tables // TODO mutexes & TODO transformar em classe!
std::map<int, std::list<packet>> packet_to_send_table; // contains the packets to send to each backup

// Global termination flag, set by the signal handler.
bool termination_signal = false;

// Global primary flag
bool is_primary = true;

// parameters of this server (passed as arguments in the terminal console)
server_params server_parameters;


void print_server_struct(server_struct server_infos) {
    printf("\nServer id: %i \n", server_infos.server_id);
    printf("IP: %s \n", server_infos.ip);
    printf("port: %i \n", server_infos.port);
    fflush(stdout);
}

// serializes the server_struct and puts it in the buffer
void serialize_server_struct(server_struct server_infos, char* buffer)
{
  memset(buffer, 0, BUFFER_SIZE * sizeof(char));
  snprintf(buffer, BUFFER_SIZE, "%d,%s,%d\n",
          server_infos.server_id, server_infos.ip, server_infos.port);
}

// unserializes the server_struct and puts it in the buffer
server_struct unserialize_server_struct(char* buffer)
{
  server_struct server_infos;
  char payload[PAYLOAD_SIZE];
	std::cout << " DEBUG 1" << std::endl;

  int buffer_size = strlen(buffer);
	std::cout << " DEBUG 1b" << std::endl;

  buffer[buffer_size] = '\0';
  char* token;
  const char delimiter[2] = ",";
  char* rest = buffer;
	std::cout << " DEBUG 2" << std::endl;
  
  // server_id
  token = strtok_r(rest, delimiter, &rest);
  server_infos.server_id = atoi(token);

	std::cout << " DEBUG 3" << std::endl;
  // ip
  bzero(payload, PAYLOAD_SIZE); //clear payload buffer
  token = strtok_r(rest, delimiter, &rest);
  int ip_len = strlen(token);
  strncpy(payload, token, ip_len);
  payload[ip_len] = '\0';
  server_infos.ip = (char*) malloc((ip_len) * sizeof(char) + 1);
  memcpy(server_infos.ip, payload, (ip_len) * sizeof(char) + 1);
	std::cout << " DEBUG 4" << std::endl;

  // port
  token = strtok_r(rest, "", &rest);
  server_infos.port = atoi(token);

  return server_infos;
}

int get_next_session_id(){
	pthread_mutex_lock(&termination_signal_mutex);
	int session_id = next_session_id;
	next_session_id++;
	pthread_mutex_unlock(&termination_signal_mutex);
	return session_id;
}

bool get_termination_signal(){
  pthread_mutex_lock(&termination_signal_mutex);
  bool signal = termination_signal;
  pthread_mutex_unlock(&termination_signal_mutex);
  return signal;
}

void set_termination_signal(){
  pthread_mutex_lock(&termination_signal_mutex);
  termination_signal = true;
  pthread_mutex_unlock(&termination_signal_mutex);
}

// TODO put in a CPP file shared with client.cpp
void set_socket_to_non_blocking_mode(int socketfd) {
	int flags = fcntl(socketfd, F_GETFL);
	if (flags == -1) {
		printf("FAILED getting socket flags.\n");
	}
	flags = flags | O_NONBLOCK;
	if (fcntl(socketfd, F_SETFL, flags) != 0) {
		printf("FAILED setting socket to NON-BLOCKING mode.\n");
	}
}

void closeConnection(int socket, int thread_id)
{
	printf("Closing connection and exiting socket thread: %d\n", thread_id);
	if (shutdown(socket, SHUT_RDWR) !=0 ) {
		std::cout << "Failed to shutdown a connection socket." << std::endl;
	}
	close(socket);
	pthread_exit(NULL);
}

// writes a message in a socket (sends a message through it)
int write_message(int newsockfd, char* message)
{
	/* write in the socket */
    int n;
	n = send(newsockfd, message, strlen(message), MSG_NOSIGNAL);
	if (n == EPIPE) {
		std::cout << "ERROR: connection has died. n: " << n << std::endl;
		return -1;
	} else if (n < 0) {
		std::cout << "ERROR writing to socket. n: " << n << std::endl;
		return -1;
	} else {
		return 0;
	}
}

void terminate_thread_and_socket(int socketfd) {
  pthread_t thread_id = pthread_self();
  printf("Exiting socket thread: %d\n", (int)thread_id);
  if (shutdown(socketfd, SHUT_RDWR) !=0 ) {
    std::cout << "Failed to gracefully shutdown a connection socket. Forcing..." << std::endl;
  }
  close(socketfd);
  pthread_exit(NULL);
}

// socket that connects to an existing socket
int setup_socket(communication_params params)
{
  int socketfd;
  struct hostent *server;
  struct sockaddr_in serv_addr;

  server = gethostbyname(params.server_ip_address);
	if (server == NULL) {
    fprintf(stderr,"ERROR, no such host\n");
    exit(0);
  }
    
  if ((socketfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) 
    printf("ERROR opening socket\n");
    
  // assign ip and port to socket
	serv_addr.sin_family = AF_INET;     
	serv_addr.sin_port = htons(params.port);    
	serv_addr.sin_addr = *((struct in_addr *)server->h_addr);
	bzero(&(serv_addr.sin_zero), 8);     
	
  // connect client socket to server socket
	if (connect(socketfd,(struct sockaddr *) &serv_addr,sizeof(serv_addr)) < 0) 
    printf("ERROR connecting\n");

  // change socket to non-blocking mode
  fcntl(socketfd, F_SETFL, fcntl(socketfd, F_GETFL, 0) | O_NONBLOCK);
  
  return socketfd;
}

// opens a socket, binds it, and starts listening for incoming connections
int setup_listen_socket(int port)
{
	int sockfd;
    struct sockaddr_in serv_addr;

	if ((sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) == -1) 
	{
        printf("ERROR creating LISTEN socket");
        exit(-1);
	}

	int enable = 1;
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
    	printf("setsockopt(SO_REUSEADDR) failed");
	}

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(port);
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	bzero(&(serv_addr.sin_zero), 8);
    
	if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) != 0) 
	{
		printf("ERROR on binding LISTEN socket");
        exit(-1);
	}

	if (listen(sockfd, 50) != 0) {
		printf("ERROR on activating LISTEN socket");
        exit(-1);
	}

    return sockfd;
}

int accept_connection(int sockfd)
{
	int newsockfd; // socket created for the connection
	socklen_t clilen;
	struct sockaddr_in cli_addr;

	clilen = sizeof(struct sockaddr_in);
	if ((newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen)) == -1) 
		printf("ERROR on accept");

	return newsockfd;
}

// reads a message from a socket (receives a message through it) (BLOCKING VERSION)
void blocking_read_message(int socketfd, char* buffer)
{
	// make sure buffer is clear	
	bzero(buffer, BUFFER_SIZE);
	/* read from the socket */
	int n;
	do{
		n = read(socketfd, buffer, BUFFER_SIZE);
	} while(n < 0);
}

void send_ACK(int socket, int reference_seqn)
{
	char payload[PAYLOAD_SIZE];
	char buffer[BUFFER_SIZE];
	bzero(payload, PAYLOAD_SIZE); //clear payload buffer
	packet packet_to_send = create_packet(payload, TYPE_ACK, reference_seqn);
	serialize_packet(packet_to_send, buffer);
	write_message(socket, buffer);
}

void send_error_to_client(int socket, int reference_seqn, char* error_message)
{
	char buffer[BUFFER_SIZE];
	packet packet_to_send = create_packet(error_message, TYPE_ERROR, reference_seqn);
	serialize_packet(packet_to_send, buffer);
	write_message(socket, buffer);
}

void send_notification(int socket, Row* currentRow, int seqn) {
	packet packet_to_send;
	char payload[PAYLOAD_SIZE];
	char buffer[BUFFER_SIZE];
	std::string notification;

	// send notification
	notification = currentRow->getNotification();
	strncpy(payload, notification.c_str(), notification.length());
	packet_to_send = create_packet(payload, TYPE_MSG, seqn);
	serialize_packet(packet_to_send, buffer);
	write_message(socket, buffer);
}

void receive_FOLLOW(packet received_packet, int socket, std::string currentUser){
	std::string newFollowedUsername(received_packet._payload); //copying char array into proper std::string type
	char payload[PAYLOAD_SIZE];
	bzero(payload, PAYLOAD_SIZE); //clear buffer


	int status = masterTable->followUser(newFollowedUsername, currentUser);
	switch(status){
		case 0:
			send_ACK(socket, received_packet.seqn);
			std::cout << currentUser + " is now following " + newFollowedUsername + "." << std::endl; fflush(stdout);
			break;
		case -1:
			// user does not exist
			snprintf(payload, PAYLOAD_SIZE, "ERROR: user does not exist \n");
			send_error_to_client(socket, received_packet.seqn, payload);
			printf("ERROR: user does not exist.\n"); fflush(stdout);
			break;
		case -2:
			snprintf(payload, PAYLOAD_SIZE, "ERROR: a user cannot follow himself \n");
			send_error_to_client(socket, received_packet.seqn, payload);
			printf("ERROR: user cannot follow himself.\n"); fflush(stdout);
			break;
		case -3:
			snprintf(payload, PAYLOAD_SIZE, "ERROR: %s is already following %s\n", currentUser.c_str(), newFollowedUsername.c_str());
			send_error_to_client(socket, received_packet.seqn, payload);
			std::cout << currentUser + " is already following " + newFollowedUsername + "." << std::endl; fflush(stdout);
			break;
	}
}

std::string receive_CONNECT(packet received_packet, int socketfd, pthread_t thread_id, int seqn, int session_id){
	char payload[PAYLOAD_SIZE];
	Row* currentRow;

	// parse username and port
	char* token;
	const char delimiter[2] = ",";
	char* rest = received_packet._payload;
	int payload_size = strlen(received_packet._payload);
	received_packet._payload[payload_size] = '\0';
	// parse current_user
	token = strtok_r(rest, delimiter, &rest);
	std::string current_user(token);
	// parse client listen port
	token = strtok_r(rest, delimiter, &rest);
	int client_listen_port = atoi(token);

	// check if map already has the username in there before inserting
	masterTable->addUserIfNotExists(current_user);
	std::cout << "User " + current_user + " is connecting...";

	currentRow = masterTable->getRow(current_user);
	
	// create session struct
	struct sockaddr addr;
	socklen_t len = sizeof(addr);
	getpeername(socketfd, &addr, &len);
	struct sockaddr_in *addr_in = (struct sockaddr_in *)&addr;
	char* ip = inet_ntoa(addr_in->sin_addr);
	session_struct new_session = create_session(session_id, ip, client_listen_port, seqn, (int)received_packet.seqn);

	if(currentRow->connectUser(new_session))
	{
		// TODO hot UPDATE_ 
		send_ACK(socketfd, received_packet.seqn);
		std::cout << " connected." << std::endl;
	} else{
		bzero(payload, PAYLOAD_SIZE); //clear buffer
		snprintf(payload, PAYLOAD_SIZE, "ERROR: user already connected with 2 sessions! (Limit reached)\n");
		send_error_to_client(socketfd, received_packet.seqn, payload);
		printf("\n denied: there are already 2 active sessions!\n"); fflush(stdout);
		closeConnection(socketfd, (int)thread_id);
	}
	return current_user;
}

void receive_DISCONNECT(std::string currentUser, int socket, pthread_t thread_id, int session_id){
	Row* currentRow = masterTable->getRow(currentUser);
	currentRow->closeSession(session_id);
	std::cout << currentUser + " has disconnected. Session closed. Terminating thread " << (int)thread_id << std::endl;
	if (shutdown(socket, SHUT_RDWR) !=0 ) {
		std::cout << "Failed to gracefully shutdown a connection socket. Forcing..." << std::endl;
	}
	close(socket);
	pthread_exit(NULL);
}

int receive_SET_ID(int socket){
	int backup_id;
	char message_received[BUFFER_SIZE];
	char buffer[BUFFER_SIZE];
	int size = 0;
	packet received_packet;
	bzero(buffer, BUFFER_SIZE); //clear buffer
	bzero(message_received, BUFFER_SIZE); //clear buffer

	// receive message
	while(size < 1){
		// try to read until it receives the message
		size = recv(socket, message_received, BUFFER_SIZE-1, 0);
	}
	message_received[size] = '\0';

	// parse socket buffer: get several messages, if there are more than one
	char* token_end_of_packet;
	char* rest_packet = message_received;
	while((token_end_of_packet = strtok_r(rest_packet, "\n", &rest_packet)) != NULL)
	{
		strcpy(buffer, token_end_of_packet); // put token_end_of_packet in buffer
		received_packet = buffer_to_packet(buffer);
		backup_id = atoi(received_packet._payload);
	}

	send_ACK(socket, received_packet.seqn);

	return backup_id;
}

// treats ACK received from client
int receive_ACK(packet received_packet, std::string currentUser, int seqn) {
	Row* currentRow;
	if(received_packet.seqn == seqn){
		currentRow = masterTable->getRow(currentUser);
		int activeSessions = currentRow->getActiveSessions();
		if(activeSessions == 1) {
			currentRow->popNotification();
		} else { // 2 active sessions
			// TODO consertar problema de mutex
			bool was_notification_delivered = currentRow->get_notification_delivered();
			if(was_notification_delivered) {
				// one notification was delivered for now
				currentRow->popNotification();
			} else {
				// no notification was delivered yet
				currentRow->set_notification_delivered(true);
				bool timeout_condition = false;
				std::time_t start_timestamp = std::time(nullptr);
				// wait until the other thread sends notification
				while(currentRow->get_notification_delivered() == true && !timeout_condition){
					std::time_t loop_timestamp = std::time(nullptr);
					timeout_condition = !(loop_timestamp - start_timestamp <= 3);
				}
			}
		}
		return 0;
	} else {
		return -1;
	}
}

server_struct receive_CONNECT_SERVER(int socketfd){
	// TODO : se o primary nao receber nada, essa thread vai ficar rodando em loop aqui
	char received_message[BUFFER_SIZE];		
	char buffer[BUFFER_SIZE];		
	int size = 0;
	packet received_packet;
	server_struct server_info;

	// receive CONNECT_SERVER message
	bzero(received_message, BUFFER_SIZE); //clear buffer
	while(size < 1){
		// try to read until it receives the message
		size = recv(socketfd, received_message, BUFFER_SIZE-1, 0);
	}
	received_message[size] = '\0';

	// parse socket buffer: get several messages, if there are more than one
	char* token_end_of_packet;
	char* rest_packet = received_message;
	while((token_end_of_packet = strtok_r(rest_packet, "\n", &rest_packet)) != NULL)
	{
		bzero(buffer, BUFFER_SIZE); //clear buffer
		strcpy(buffer, token_end_of_packet); // put token_end_of_packet in buffer
		received_packet = buffer_to_packet(buffer);
		if(received_packet.type != TYPE_CONNECT_SERVER){
			// packet is discarded
			printf("ERROR: received wrong packet type from incoming server connection.\n");fflush(stdout);
			terminate_thread_and_socket(socketfd);
		} else{
			// parse port and backup_id
			char* token;
			const char delimiter[2] = ",";
			char* rest = received_packet._payload;
			int payload_size = strlen(received_packet._payload);
			received_packet._payload[payload_size] = '\0';
			// parse port
			token = strtok_r(rest, delimiter, &rest);
			server_info.port = atoi(token);
			// parse backup_id
			token = strtok_r(rest, delimiter, &rest);
			server_info.server_id = atoi(token);
		}
	}
	send_ACK(socketfd, received_packet.seqn);

	// get backup IP from socket
	struct sockaddr addr;
	socklen_t len = sizeof(addr);
	getpeername(socketfd, &addr, &len);
	struct sockaddr_in *addr_in = (struct sockaddr_in *)&addr;
	server_info.ip = inet_ntoa(addr_in->sin_addr);

	return server_info;
}

int send_UPDATE_BACKUP(int receiving_server_id, int seqn, int socketfd, int backup_id){
	int size;
	int send_tries = 0;
	// char* payload = (char*) malloc((PAYLOAD_SIZE) * sizeof(char) + 1);
	packet packet_to_send, received_packet;
	server_struct backup_infos;
	server_struct* backup_infos_ptr;

	// get server_struct
	// check if this server_struct exists at the table
	if(servers_table.find(receiving_server_id) == servers_table.end()) {
		std::cout << "DEBUG nao existe " << std::endl;
	}
	backup_infos_ptr = servers_table.find(receiving_server_id)->second;
	print_server_struct(*backup_infos_ptr);
	backup_infos = *backup_infos_ptr;

	// create UPDATE_BACKUP
	bzero(payload, PAYLOAD_SIZE); //clear payload buffer
	serialize_server_struct(backup_infos, payload);
	packet_to_send = create_packet(payload, TYPE_UPDATE_BACKUP, seqn);
	std::cout << "DEBUG packet_to_send " << std::endl;
	print_packet(packet_to_send);
	// put UPDATE_BACKUP packet in the FIFO
	packet_to_send_table.find(backup_id)->second.push_back(packet_to_send);

	return 0;
}

// cold send all server_struct inside servers_table
int send_all_servers_table(int socketfd, int seqn, int backup_id) {
	int status;

	// TODO lock na servers table
	for (auto const& x : servers_table) {
		int server_id = x.first;
		status = send_UPDATE_BACKUP(server_id, seqn, socketfd, backup_id);
		if(status != 0) {
			return -1;
		}
		seqn++;
	}
	return seqn;
}

int send_UPDATE_ROW(std::string username, int seqn, int socketfd, int backup_id){
	int size;
	int send_tries = 0;
	// char* payload = (char*) malloc((PAYLOAD_SIZE) * sizeof(char) + 1);
	packet packet_to_send;

	// TODO lock na row
	Row* row = masterTable->getRow(username);

	// put UPDATE_ROW packet in the FIFO
	bzero(payload, PAYLOAD_SIZE); //clear payload buffer
	row->serialize_row(payload, username);
	packet_to_send = create_packet(payload, TYPE_UPDATE_ROW, seqn);
	// put UPDATE_ROW packet in the FIFO
	packet_to_send_table.find(backup_id)->second.push_back(packet_to_send);
	
	return 0;
}

// cold send all server_struct inside servers_table
int send_all_rows(int socketfd, int seqn, int backup_id){
	int status;

	// TODO lock na master table
	for (auto const& x : masterTable->getTable()) {
		std::string username = x.first;

		status = send_UPDATE_ROW(username, seqn, socketfd, backup_id);
		if(status != 0) {
			return -1;
		}
		seqn++;
	}
	return seqn;
}

int send_message_to_client(std::string currentUser, int seqn, int socket){
	char payload[PAYLOAD_SIZE];
	char buffer[BUFFER_SIZE];
	std::string notification;
	packet packet_to_send;
	if(currentUser != "not-connected") // if the user has already connected
	{
		Row* currentRow = masterTable->getRow(currentUser);
		
		if(currentRow->hasNewNotification())
		{
			int activeSessions = currentRow->getActiveSessions();
			if(activeSessions == 1) {
				// consume notification and remove it from the FIFO
				send_notification(socket, currentRow, seqn);
				return 0;
			} else if(activeSessions == 2) {
				// TODO consertar aqui !!! botar um lock
				bool was_notification_delivered = currentRow->get_notification_delivered();
				if(was_notification_delivered) {
					// consume notification and remove it from the FIFO
					send_notification(socket, currentRow, seqn);
					return 0;
				} else {
					// consume notification but DO NOT remove it from the FIFO
					notification = currentRow->getNotification();
					strncpy(payload, notification.c_str(), notification.length());
					packet_to_send = create_packet(payload, 0, seqn);
					serialize_packet(packet_to_send, buffer);
					write_message(socket, buffer);
					return 0;
				}
			}
		}
	}
	return -1;
}

// send CONNECT_SERVER and wait for ACK. returns 0 if success
int send_CONNECT_SERVER(int socketfd, int seqn, int port) {
	char payload[PAYLOAD_SIZE];
	char buffer[BUFFER_SIZE];
	char message_received[BUFFER_SIZE];
	int size;
	int send_tries = 0;
	packet received_packet, packet_to_send;

	// send CONNECT_SERVER packet to primary server
	bzero(payload, PAYLOAD_SIZE); //clear payload buffer
	snprintf(payload, PAYLOAD_SIZE, "%d,%d", port, my_backup_id);
	packet_to_send = create_packet(payload, TYPE_CONNECT_SERVER, seqn);
	serialize_packet(packet_to_send, buffer);
	write_message(socketfd, buffer);

	// receive ACK from primary
	bzero(buffer, BUFFER_SIZE); //clear payload buffer
	bzero(message_received, BUFFER_SIZE); //clear payload buffer
	do {
		// receive message
		size = recv(socketfd, message_received, BUFFER_SIZE-1, 0);
		if (size > 0) {
			message_received[size] = '\0';

			// parse socket buffer: get several messages, if there are more than one
			char* token_end_of_packet;
			char* rest_packet = message_received;
			while((token_end_of_packet = strtok_r(rest_packet, "\n", &rest_packet)) != NULL)
			{
				strcpy(buffer, token_end_of_packet); // put token_end_of_packet in buffer
				received_packet = buffer_to_packet(buffer);
				if(received_packet.type == TYPE_ACK){
					std::cout << "DEBUG send_CONNECT_SERVER received ACK" << std::endl;
					return 0;
				}
			}
		}
		write_message(socketfd, buffer);
		send_tries++;
		sleep(3);
	} while(send_tries <= MAX_TRIES);
	std::cout << "DEBUG send_CONNECT_SERVER did NOT receive ACK !!!!!!!!!!!!" << std::endl;
	return -1;
}

void send_ping_to_primary(int socketfd, int seqn){
	char payload[PAYLOAD_SIZE];
	char buffer[BUFFER_SIZE];
	bzero(payload, PAYLOAD_SIZE); //clear payload buffer
	packet packet_to_send = create_packet(payload, TYPE_HEARTBEAT, seqn);
	bzero(buffer, BUFFER_SIZE); //clear buffer
	serialize_packet(packet_to_send, buffer);
	write_message(socketfd, buffer);
	std::cout << "DEBUG sent HEARTBEAT" << std::endl;
}

// tries to send message if there is any in the FIFO
int send_UPDATE(int backup_id, int seqn, int socketfd) {
	// returns 0 if message was sent
	// returns -1 if message was not sent
	// returns -2 if there no message to send
	int status;
	char buffer[BUFFER_SIZE];
  	packet packet_to_send;
	pthread_mutex_lock(&packets_to_send_mutex);
	std::list<packet> packets_to_send_fifo = packet_to_send_table.find(backup_id)->second;
	if(packets_to_send_fifo.empty()) {
		status = -2;
	} else {
		// serialize and send the packet
		packet_to_send = packets_to_send_fifo.front();
		std::cout << "DEBUG packet to send: " << std::endl;
		print_packet(packet_to_send);
		serialize_packet(packet_to_send, buffer);
		std::cout << "DEBUG sending buffer: " << buffer << std::endl;
		status = write_message(socketfd, buffer);
	}
	pthread_mutex_unlock(&packets_to_send_mutex);

	return status;
}

// send SET_ID and wait for ACK. returns 0 if success
int send_SET_ID(int socketfd, int seqn, int backup_id){
	char payload[PAYLOAD_SIZE];
	char buffer[BUFFER_SIZE];
	char message_received[BUFFER_SIZE];
	int size;
	int send_tries = 0;
	packet received_packet, packet_to_send;

	// send SET_ID packet to backup server
	bzero(payload, PAYLOAD_SIZE); //clear payload buffer
	bzero(message_received, BUFFER_SIZE); //clear payload buffer
	snprintf(payload, PAYLOAD_SIZE, "%d", backup_id);
	packet_to_send = create_packet(payload, TYPE_SET_ID, seqn);
	serialize_packet(packet_to_send, buffer);
	write_message(socketfd, buffer);

	// receive ACK from backup
	do {
		// receive message
		size = recv(socketfd, message_received, BUFFER_SIZE-1, 0);
		if (size > 0) {
			message_received[size] = '\0';

			// parse socket buffer: get several messages, if there are more than one
			char* token_end_of_packet;
			char* rest_packet = message_received;
			while((token_end_of_packet = strtok_r(rest_packet, "\n", &rest_packet)) != NULL)
			{
				bzero(buffer, BUFFER_SIZE); //clear payload buffer
				strcpy(buffer, token_end_of_packet); // put token_end_of_packet in buffer
				received_packet = buffer_to_packet(buffer);
				if(received_packet.type == TYPE_ACK){
					std::cout << "DEBUG send_SET_ID received ACK" << std::endl;
					return 0;
				}
			}
		}
		write_message(socketfd, buffer);
		send_tries++;
		sleep(3);
	} while(send_tries <= MAX_TRIES);
	return -1;
}

int cold_replication(int socketfd, int seqn, int backup_id) {
	std::cout << "Starting cold replication..." << std::endl;

	seqn = send_all_servers_table(socketfd, seqn, backup_id);
	if(seqn == -1) {
		printf("ERROR: could not send cold UPDATE_BACKUP packets to backup.\n"); fflush(stdout);
		terminate_thread_and_socket(socketfd);
	}
	std::cout << "DEBUG seqn: " << seqn << std::endl;
	seqn = send_all_rows(socketfd, seqn, backup_id);
	if(seqn == -1) {
		printf("ERROR: could not send cold UPDATE_ROW packets to backup.\n"); fflush(stdout);
		terminate_thread_and_socket(socketfd);
	}
	
	return seqn;
}

// Thread designated to communicate with the primary server (thread roda no backup)
void * primary_communication_thread(void *arg) {
	int socket = *((int *)arg);
	int send_tries = 0;
	int size = 0;
	int seqn = 0;
	int status;
	int max_reference_seqn = -1; // TODO temos que enviar isso pras replicas backup
	char buffer[BUFFER_SIZE];
	char backup_server_message[BUFFER_SIZE];		
	packet received_packet;
	time_t t0, t1;

	// setting socket to NON-BLOCKING mode
	set_socket_to_non_blocking_mode(socket);

	// send CONNECT_SERVER
	status = send_CONNECT_SERVER(socket, seqn, server_parameters.local_servers_listen_port);
	if(status == 0){
		seqn++;
	} else {
		printf("ERROR: could not connect to primary server.\n"); fflush(stdout);
		exit(status);
	}

	// receber SET_ID
	int new_backup_id = receive_SET_ID(socket);
	if(new_backup_id != my_backup_id){
		my_backup_id = new_backup_id;
		printf("Assigned id: %d\n", my_backup_id); fflush(stdout);
	}

	// inicia timer e manda primeiro ping
	t0 = std::time(0); // get timestamp
	send_ping_to_primary(socket, seqn);
	seqn++;

	do {
		// receive message
		size = recv(socket, backup_server_message, BUFFER_SIZE-1, 0);
		if (size > 0) {
			std::cout << "DEBUG received buffer: " << backup_server_message << std::endl;
			backup_server_message[size] = '\0';

			// parse socket buffer: get several messages, if there are more than one
			char* token_end_of_packet;
  			char* rest_packet = backup_server_message;
			while((token_end_of_packet = strtok_r(rest_packet, "\n", &rest_packet)) != NULL)
			{
				strcpy(buffer, token_end_of_packet); // put token_end_of_packet in buffer
				received_packet = buffer_to_packet(buffer);

				if(received_packet.seqn <= max_reference_seqn && received_packet.type != TYPE_ACK){
					std::cout << "DEBUG  already received this message - seqn: " << received_packet.seqn << std::endl;
					// already received this message
					send_ACK(socket, received_packet.seqn);
				} else {
					switch (received_packet.type) {
						case TYPE_UPDATE_ROW:
						{
							max_reference_seqn = received_packet.seqn;
							std::cout << "DEBUG received UPDATE_ROW ... payload: " << received_packet._payload << std::endl;
							Row* newRow = new Row;
							std::string username = newRow->unserialize_row(received_packet._payload);
							masterTable->addRow(newRow, username);
							send_ACK(socket, received_packet.seqn);
							break;
						}
						case TYPE_UPDATE_BACKUP:
						{
							max_reference_seqn = received_packet.seqn;
							std::cout << "DEBUG received UPDATE_BACKUP ... payload: " << received_packet._payload << std::endl;
							server_struct backup_infos = unserialize_server_struct(received_packet._payload);
							std::cout << "DEBUG 11111111 " << std::endl;
							server_struct* backup_infos_ptr = (server_struct*)malloc(sizeof(server_struct));
							std::cout << "DEBUG 22222222222 " << std::endl;
							*backup_infos_ptr = backup_infos;
							std::cout << "DEBUG 333333333333 " << std::endl;
							servers_table.insert(std::make_pair(backup_infos.server_id, backup_infos_ptr));
							std::cout << "DEBUG sending UPDATE_BACKUP ACK..." << std::endl;
							send_ACK(socket, received_packet.seqn);
							std::cout << "DEBUG sent UPDATE_BACKUP ACK" << std::endl;
							break;
						}
						case TYPE_ACK:
						{
							std::cout << "DEBUG received ACK" << std::endl;
							send_tries = 0;
							break;
						}
					}
				}
			}
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(50));

		// mandar pings pro primario
		t1 = std::time(0); // get timestamp atual
		if(t1 - t0 > 3) // every 3 seconds
		{
			std::cout << "DEBUG reset timer" << std::endl;
			t0 = t1; // reset timer
			send_tries++;
			send_ping_to_primary(socket, seqn);
			seqn++;
		}

		// detect if primary server has died
		if(send_tries > MAX_TRIES) {
			std::cout << "Primary server has died!!!" << std::endl;
			// TODO primario morreu! tratar
			// TODO eleicao
			// TODO mandar pro cliente SERVER_CHANGE
		}
  	}while (get_termination_signal() == false && send_tries <= MAX_TRIES);
	if(get_termination_signal() == true){
		std::cout << "Got termination signal. Closing thread and socket." << std::endl;
	} else if(send_tries > MAX_TRIES){
		std::cout << "Primary server does not respond! exiting thread (TODO!)" << std::endl;
	}
	if (shutdown(socket, SHUT_RDWR) !=0 ) {
		std::cout << "Failed to shutdown a connection socket." << std::endl;
	}
	close(socket);
	pthread_exit(NULL);
}

// Thread designated for the connected backup server (thread roda no primario)
void * servers_socket_thread(void *arg) {
	int socket = *((int *)arg);
	int send_tries = 0;
	int size = 0;
	int seqn = 0;
	int status;
	int max_reference_seqn = -1; // TODO temos que enviar isso pras replicas backup
	char buffer[BUFFER_SIZE];
	char backup_server_message[BUFFER_SIZE];		
	int current_server_id = -1;
	packet received_packet;

	// setting socket to NON-BLOCKING mode
	set_socket_to_non_blocking_mode(socket);

	// receive CONNECT_SERVER
	server_struct backup_infos = receive_CONNECT_SERVER(socket);

	// set server_id if it is not set yet
	if(backup_infos.server_id == UNDEFINED_ID){
		num_backup_servers++;
		backup_infos.server_id = num_backup_servers;
	}
	current_server_id = backup_infos.server_id;

	// check if this backup server is already in servers_table
	if(servers_table.find(backup_infos.server_id) == servers_table.end()){
		// this backup server is not yet in the table... adding it!
		server_struct* backup_infos_ptr = (server_struct*)malloc(sizeof(server_struct));
		*backup_infos_ptr = backup_infos;
		servers_table.insert(std::make_pair(backup_infos.server_id, backup_infos_ptr));
	}
	// send SET_ID (attribute a new unique incremented ID to the server)
	status = send_SET_ID(socket, seqn, backup_infos.server_id);
	if(status == 0){
		seqn++;
		// create FIFO for this backup
		std::list<packet> empty_list;
		packet_to_send_table.insert(std::make_pair(current_server_id, empty_list));
	} else {
		printf("ERROR: could not send ID to backup.\n"); fflush(stdout);
		terminate_thread_and_socket(socket);
	}

	// cold replication of masterTable and servers_table
	seqn = cold_replication(socket, seqn, current_server_id);

	std::cout << "Starting backup-primary communication loop with backup " << backup_infos.server_id << std::endl;
	do {
		// receive message
		size = recv(socket, backup_server_message, BUFFER_SIZE-1, 0);
		if (size > 0) {
			std::cout << "DEBUG received backup_server_message: " << backup_server_message << std::endl;
			backup_server_message[size] = '\0';

			// parse socket buffer: get several messages, if there are more than one
			char* token_end_of_packet;
  			char* rest_packet = backup_server_message;
			while((token_end_of_packet = strtok_r(rest_packet, "\n", &rest_packet)) != NULL)
			{
				strcpy(buffer, token_end_of_packet); // put token_end_of_packet in buffer
				received_packet = buffer_to_packet(buffer);
				print_packet(received_packet);

				if(received_packet.seqn <= max_reference_seqn && received_packet.type != TYPE_ACK){
					// already received this message
					std::cout << "DEBUG sent ACK" << std::endl;
					send_ACK(socket, received_packet.seqn);
				} else {
					switch (received_packet.type) {
						case TYPE_HEARTBEAT:
						{
							max_reference_seqn = received_packet.seqn;
							std::cout << "DEBUG received HEARTBEAT ... seqn: " << received_packet.seqn << std::endl;
							send_ACK(socket, received_packet.seqn);
							break;
						}
						case TYPE_ACK:
						{
							std::cout << "DEBUG received ACK ... seqn: " << received_packet.seqn << std::endl;
							std::list<packet> packets_to_send_fifo = packet_to_send_table.find(backup_infos.server_id)->second;
							packets_to_send_fifo.pop_front();
							send_tries = 0;
							break;
						}
					}
				}
			}
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		if(send_tries > 0){
			sleep(3);
		}

		// send update, if there is any
		status = send_UPDATE(current_server_id, seqn, socket);
		if(status == 0){
			// success
			std::cout << "DEBUG sent a message!!! to backup with id: " << current_server_id << std::endl;
			send_tries++;
			seqn++;
		} else if (status == -1){
			// there is a message but it wasn't sent
			std::cout << "Failed to send a message to backup with id: " << current_server_id << std::endl;
			send_tries++;
			sleep(1);
		} else {		 // else: nao tinha mensaegm pra mandar
			std::cout << "DEBUG empty FIFO for backup with id: " << current_server_id << std::endl;
		}
  	}while (get_termination_signal() == false && send_tries <= MAX_TRIES);
	if(get_termination_signal() == true){
		std::cout << "Got termination signal. Closing thread and socket." << std::endl;
	} else if(send_tries > MAX_TRIES){
		std::cout << "Backup server does not respond. Closing thread and socket." << std::endl;
	}
	if (shutdown(socket, SHUT_RDWR) !=0 ) {
		std::cout << "Failed to shutdown a connection socket." << std::endl;
	}
	close(socket);
	pthread_exit(NULL);
}

// Thread designated for the connected client
void * socket_thread(void *arg) {
	int socket = *((int *)arg);
	int send_tries = 0;
	int size = 0;
	int seqn = 0;
	int session_id;
	int status;
	int max_reference_seqn = -1; // TODO temos que enviar isso pras replicas backup
	char client_message[BUFFER_SIZE];
	char buffer[BUFFER_SIZE];
	Row* currentRow;
	std::string currentUser = "not-connected";
	std::string notification;
	
	packet received_packet;

	// print pthread id
	pthread_t thread_id = pthread_self();

	// setting socket to NON-BLOCKING mode
	set_socket_to_non_blocking_mode(socket);

	// zero-fill the buffer
	memset(client_message, 0, sizeof client_message);

	do{
		// send message, if there is any
		status = send_message_to_client(currentUser, seqn, socket);
		if(status == 0){
			send_tries++;
		}

		// receive message
		size = recv(socket, client_message, BUFFER_SIZE-1, 0);
		if (size > 0) {
			client_message[size] = '\0';

			// parse socket buffer: get several messages, if there are more than one
			char* token_end_of_packet;
  			char* rest_packet = client_message;
			while((token_end_of_packet = strtok_r(rest_packet, "\n", &rest_packet)) != NULL)
			{
				strcpy(buffer, token_end_of_packet); // put token_end_of_packet in buffer
				received_packet = buffer_to_packet(buffer);

				if(received_packet.seqn <= max_reference_seqn && received_packet.type != TYPE_ACK){
					// already received this message
					send_ACK(socket, received_packet.seqn);
				} else {
					switch (received_packet.type) {
						case TYPE_CONNECT:
						{
							max_reference_seqn = received_packet.seqn;
							session_id = get_next_session_id();
							currentUser = receive_CONNECT(received_packet, socket, thread_id, seqn, session_id);
							// TODO por na fila de updates pros backups
							break;
						}
						case TYPE_FOLLOW:
						{
							max_reference_seqn = received_packet.seqn;
							receive_FOLLOW(received_packet, socket, currentUser);
							// TODO por na fila de updates pros backups
							break;
						}
						case TYPE_SEND:
						{
							max_reference_seqn = received_packet.seqn;
							std::string message(received_packet._payload); //copying char array into proper std::string type
							masterTable->sendMessageToFollowers(currentUser, message);
							send_ACK(socket, received_packet.seqn);
							break;
						}
						case TYPE_DISCONNECT:
						{
							max_reference_seqn = received_packet.seqn;
							receive_DISCONNECT(currentUser, socket, thread_id, session_id);
							break;
						}
						case TYPE_ACK:
						{
							status = receive_ACK(received_packet, currentUser, seqn);
							if(status == 0){
								send_tries = 0; // reset this counter
								seqn++;
							}
							break;
						}
					}
				}
			}
		}
		sleep(2);
  	}while (get_termination_signal() == false && send_tries <= MAX_TRIES);
	if(get_termination_signal() == true){
		std::cout << "Got termination signal. Closing thread and socket." << std::endl;
	} else if(send_tries > MAX_TRIES){
		std::cout << "Client " << currentUser << " does not respond. Closing thread and socket." << std::endl;
	}
	currentRow = masterTable->getRow(currentUser);
	currentRow->closeSession(session_id);
	if (shutdown(socket, SHUT_RDWR) !=0 ) {
		std::cout << "Failed to shutdown a connection socket." << std::endl;
	}
	close(socket);
	pthread_exit(NULL);
}

void exit_hook_handler(int signal) {
	std::cout<< std::endl << "Signal received: code is " << signal << std::endl;

	set_termination_signal();
}

int main(int argc, char *argv[])
{
	int clients_sockfd;
	int servers_sockfd;
	int newsockfd;
	int* newsockptr;
	socklen_t clilen;
	struct sockaddr_in cli_addr;
	std::list<pthread_t> threads_list;
	pthread_t newthread;
	std::list<int*> socketptrs_list;

	// Read arguments
	if (argc == 4 || argc == 6) {

		server_parameters.type = argv[1]; // Server type: primary or backup?

    	server_parameters.local_clients_listen_port = atoi(argv[2]);
    	server_parameters.local_servers_listen_port = atoi(argv[3]);

		if ((strcmp(server_parameters.type, "backup") == 0) && (argc == 6)) {
			// Optional args
			server_parameters.remote_primary_server_ip_address = argv[4];
    		server_parameters.remote_primary_server_port = atoi(argv[5]);

			is_primary = false;

		} else if ((strcmp(server_parameters.type, "backup") == 0) && (argc != 6)) {
			std::cout << "Remote primary server IP and port were not informed." << std::endl;
			std::cout << "Usage: <type: 'backup' or 'primary'> <listen port for clients> <listen port for servers> <remote primary IP> <remote primary port>" << std::endl;
			exit(1);
		}
	}
	else {
		std::cout << "Usage: <type: 'backup' or 'primary'> <listen port for clients> <listen port for servers> <remote primary IP> <remote primary port>" << std::endl;
		exit(0);
	}

	// Install handler (assign handler to signal)
    std::signal(SIGINT, exit_hook_handler);
	std::signal(SIGTERM, exit_hook_handler);
	std::signal(SIGABRT, exit_hook_handler);

	// Initialize global MasterTable instance
	masterTable = new MasterTable;

	// load backup table if it exists
	masterTable->load_backup_table();

	// setup LISTEN sockets
    clients_sockfd = setup_listen_socket(server_parameters.local_clients_listen_port);
	servers_sockfd = setup_listen_socket(server_parameters.local_servers_listen_port);

	printf("Now listening for incoming connections... \n\n");

	clilen = sizeof(struct sockaddr_in);

	if(is_primary){
		my_backup_id = 0;
	} else {
		std::cout << "I'm a backup server!" << std::endl;
		// If server is of type BACKUP, then open connection to the primary and spawn a thread

		my_backup_id = UNDEFINED_ID; // will solicitate ID to primary

		// criar socket e conectar ao primary
		communication_params params;
		params.port = server_parameters.remote_primary_server_port;
		params.server_ip_address = server_parameters.remote_primary_server_ip_address;
		newsockfd = setup_socket(params);

		if((newsockptr = (int*)malloc(sizeof(int))) == NULL) {
			std::cout << "Failed to allocate enough memory for the newsockptr. (servers)" << std::endl;
			exit(-1);
		}

		// create thread for receiving and sending packets to primary
		*newsockptr = newsockfd;
		if (pthread_create(&newthread, NULL, primary_communication_thread, newsockptr) != 0 ) {
			printf("Failed to create thread\n"); fflush(stdout);
			exit(-1);
		}	else {
			threads_list.push_back(newthread);
			socketptrs_list.push_back(newsockptr);
		}
	} 

	// loop that accepts new connections and allocates threads to deal with them
	while(1) {
		
		// If new incoming CLIENTS connection, accept. Then continue as normal.
		if ((newsockfd = accept(clients_sockfd, (struct sockaddr *) &cli_addr, &clilen)) != -1) {
			
			if((newsockptr = (int*)malloc(sizeof(int))) == NULL) {
				std::cout << "Failed to allocate enough memory for the newsockptr. (clients)" << std::endl;
				exit(-1);
			}

			*newsockptr = newsockfd;
			if (pthread_create(&newthread, NULL, socket_thread, newsockptr) != 0 ) {
				printf("Failed to create clients socket thread\n");
				exit(-1);
			} else {
				threads_list.push_back(newthread);
				socketptrs_list.push_back(newsockptr);
			}
		}

		// If new incoming SERVERS connection, accept. Then continue as normal.
		if ((newsockfd = accept(servers_sockfd, (struct sockaddr *) &cli_addr, &clilen)) != -1) {
			
			if((newsockptr = (int*)malloc(sizeof(int))) == NULL) {
				std::cout << "Failed to allocate enough memory for the newsockptr. (servers)" << std::endl;
				exit(-1);
			}	
			
			*newsockptr = newsockfd;
			if (pthread_create(&newthread, NULL, servers_socket_thread, newsockptr) != 0 ) {
				printf("Failed to create servers socket thread\n");
				exit(-1);
			} else {
				threads_list.push_back(newthread);
				socketptrs_list.push_back(newsockptr);
			}
		}

		// Cleanup code for main server thread
		if(get_termination_signal() == true) {
			std::cout << "Server is now gracefully shutting down..." << std::endl;
			// TODO informar aos clientes q o servidor esta desligando
			std::cout << "Closing passive socket..." << std::endl;
			if (shutdown(clients_sockfd, SHUT_RDWR) !=0 ) {
				std::cout << "Failed to shutdown the clients connection socket." << std::endl;
			}
			close(clients_sockfd);
			if (shutdown(servers_sockfd, SHUT_RDWR) !=0 ) {
				std::cout << "Failed to shutdown the servers connection socket." << std::endl;
			}
			close(servers_sockfd);
			
			for (auto const& thread_id : threads_list) {
				pthread_join(thread_id, NULL);
			}

			std::cout << "Freeing allocated socket pointers memory..." << std::endl;
			for (auto const& socket_id : socketptrs_list) {
				free(socket_id);
			}

			std::cout << "Deleting instantiated objects..." << std::endl;
			masterTable->deleteRows();
			delete(masterTable);

			std::cout << "Shutdown routine successfully completed." << std::endl;
			exit(0);
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
}
