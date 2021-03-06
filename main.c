// Basic
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Net
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

// Extra
#include <pcre.h>
#include <libconfig.h>

// Configuration
#define LONGESTERRORMESSAGE 60 // in glibc-2.7 the longest error is 50 chars. I still don't like this.
#define MAXRECV 1024*4

char *logfile;

struct cached_file {
	char *alias;
	char *contenttype;
	char *data;
	char *sizestring;
	char *statuscode;
	int sizelength;
	long size;
};

void logprint(const char *message, int error) {
	// I could open the logfile once and change the buffering to
	// line-buffering, but it would complicate the code a bit. I don't know
	// which method would be techically superiour.
	size_t errorsize = LONGESTERRORMESSAGE;
	char *errormessage = malloc(sizeof(char) * errorsize+1);
	FILE *logfileh;
	extern char *logfile;

	if (error > 0) 
		strerror_r(error, errormessage, errorsize);

	// do not touch the filesystem as root
	if (getuid() == 0) {
		printf("[%d] %s\n", time(NULL), message);
		if (error > 0)
			printf("Error #%d: %s\n", error, errormessage);
	} else {
		logfileh = fopen(logfile, "a");
		//setvbuf(LOGFILE, NULL, _IOLBF, 0); // line buffering

		if (logfileh == NULL) {
			printf("Failed to open log file!: %d\n", errno);
			exit(1);
		} 

		fprintf(logfileh, "[%d] %s\n", time(NULL), message);
		if (error > 0)
			fprintf(logfileh, "Error #%d: %s\n", error, errormessage);

		fclose(logfileh);
	}
	free(errormessage);
}

void catch_regex_error(int error_number) {
	switch(error_number) {
		case 0:
			logprint("Not enough space in substring to store result", 0);
			break;
		case PCRE_ERROR_NOMATCH : 
			logprint("String did not match the pattern", 0);
			break;
		case PCRE_ERROR_NULL : 
			logprint("Something was null", 0);
			break;
		case PCRE_ERROR_BADOPTION :
			logprint("A bad option was passed", 0); 
			break;
		case PCRE_ERROR_BADMAGIC : 
			logprint("Magic number bad (compiled re corrupt?)", 0); 
			break;
		case PCRE_ERROR_UNKNOWN_NODE : 
			logprint("Something kooky in the compiled re", 0); 
			break;
		case PCRE_ERROR_NOMEMORY : 
			logprint("Ran out of memory", 0); 
			break;
	}
}

int handle_request(int sockfd, struct cached_file content) {
	char buffer[1024]; // FIXME hardcoded arbitrary buffer size for header

	unsigned int sent_bytes;
	unsigned int total = 0;
	unsigned int bytes_left = content.size;

	snprintf(buffer, 1024, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %s\r\nConnection: close\r\n\r\n", content.statuscode, content.contenttype, content.sizestring);
	sent_bytes = send(sockfd, buffer, strlen(buffer), MSG_MORE);
	if (sent_bytes == -1)
		return -1;
	total = sent_bytes;

	while (total < strlen(buffer)) {
		sent_bytes = send(sockfd, buffer+total, strlen(buffer) - total, 0); 
		if (sent_bytes == -1)
			break;

		total += sent_bytes;
	}

	total = 0;

	while (total < bytes_left) {
		sent_bytes = send(sockfd, content.data+total, bytes_left, 0);
		if (sent_bytes == -1) { 
			break; 
		}

		total      += sent_bytes;
		bytes_left -= sent_bytes;
	}

	if (sent_bytes == -1) {
		logprint("socket error!", errno);
		return -1;
	}
	
	return 0;
}

void main() {
	int i, status;
	int yes = 1;
	int program_status = 1;
	
	// Config
	config_t cfg;
	config_setting_t *setting;
	const char *configbuffer;

	config_init(&cfg);

	// System
	int userid = 1000;
	int groupid = 1000;

	// Cache
	FILE *fhandle;
	int loaded_files = 0;
	struct cached_file *storage;
	struct cached_file served_file;

	// Networking
	struct sockaddr_storage remote_address;
	socklen_t addr_size;
	struct addrinfo hints, *res;
	int sockfd, new_fd;
	char *recv_buffer;
	unsigned int recv_bytes = 0;

	char *listenport;
	int queue;

	char *request;	  // The entire request from the user
	char *cacherequest; // The requested URI, so long as it's [a-z0-9.-_]

	// Load configuration
	if (!config_read_file(&cfg, "weehttpd.cfg")) {
		printf("%s\n", config_error_text(&cfg));
		config_destroy(&cfg);
		exit(1);
	}

	const char *logfilepath;
	config_lookup_string(&cfg, "logfile", &logfilepath); 
	logfile = malloc(sizeof(char)*strlen(logfilepath)+1);
	strcpy(logfile, logfilepath);

	const char *listenporttemp;
	config_lookup_string(&cfg, "port", &listenporttemp);
	listenport = malloc(sizeof(char)*strlen(listenporttemp)+1);
	strcpy(listenport, listenporttemp);

	config_lookup_int(&cfg, "queue", &queue);

	setting = config_lookup(&cfg, "files");
	if (setting != NULL) {
		int count = config_setting_length(setting);
		storage = malloc(sizeof(struct cached_file)*count);

		for (i = 0; i < count; ++i) {
			config_setting_t *filedef = config_setting_get_elem(setting, i);

			const char *alias, *path, *statuscode, *contenttype;

			if (! (config_setting_lookup_string(filedef, "path", &path) && 
				   config_setting_lookup_string(filedef, "alias", &alias)))
				continue; // path and alias are mandatory

			if (!config_setting_lookup_string(filedef, "statuscode", &statuscode))
				statuscode = "200 OK";

			if (!config_setting_lookup_string(filedef, "contenttype", &contenttype))
				contenttype = "text/plain";

			storage[i].alias = malloc(sizeof(char)*strlen(alias)+1);
			strcpy(storage[i].alias, alias);

			fhandle = fopen(path, "rb");
			if (fhandle == NULL) {
				logprint(alias, 0);
				logprint("Failed to open file referenced in config file", errno);
				exit(1);
			} else {
				fseek(fhandle, 0, SEEK_END);
				storage[i].size = ftell(fhandle);
				rewind(fhandle);
				storage[i].data = malloc(sizeof(char)*storage[i].size);
				fread(storage[i].data, sizeof(char), storage[i].size, fhandle);
				fclose(fhandle);

				// magic to convert long to string without buffer overflows
				storage[i].sizelength = snprintf(NULL, 0, "%lu", storage[i].size);
				char sizestring[storage[i].sizelength+1];
				int c = snprintf(sizestring, storage[i].sizelength+1, "%lu", storage[i].size);

				storage[i].sizestring = malloc(sizeof(char)*storage[i].sizelength+1);
				strcpy(storage[i].sizestring, sizestring);

				storage[i].statuscode = "200 OK";
			}

			storage[i].contenttype = malloc(sizeof(char)*strlen(contenttype)+1);
			strcpy(storage[i].contenttype, contenttype);
			storage[i].statuscode = malloc(sizeof(char)*strlen(statuscode)+1);
			strcpy(storage[i].statuscode, statuscode);
			loaded_files++;
		}
	}
	config_destroy(&cfg);

	// Perl-compatible Regex
	pcre *reCompiled;
	pcre_extra *pcreExtra;
	const char *pcreErrorStr;
	const char *psubStrMatchStr;
	char *RequestMatchRegex;
	int pcreErrorOffset;
	int pcreExecRet;
	int subStrings[6]; // FIXME arbitrary hardcoded value

	// My implementation of request URIs is not to standard
	RequestMatchRegex = "^GET /([a-zA-Z0-9.\\-_]*) HTTP/1\\.[01]$";
	reCompiled = pcre_compile(RequestMatchRegex, PCRE_MULTILINE, &pcreErrorStr, &pcreErrorOffset, NULL);
	
	if (reCompiled == NULL) {
		logprint("Failed to compile request regex.", 0);
		exit(1);
	}

	pcreExtra = pcre_study(reCompiled, 0, &pcreErrorStr);

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	logprint("Program started.", 0);

	status = getaddrinfo(NULL, listenport, &hints, &res);
	if (status != 0) {
		logprint(gai_strerror(status), errno);
		exit(1);
	}

	sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (sockfd == -1) {
		logprint("Failed to create socket descriptor.", errno);
		exit(1);
	}

	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
		logprint("Failed to set socket options.", errno);
		exit(1);
	}

	if (bind(sockfd, res->ai_addr, res->ai_addrlen) == -1) {
		logprint("Failed to bind socket.", errno);
		exit(1);
	}
	
	if (listen(sockfd, queue) != 0) {
		logprint("Failed to listen for connections.", errno);
		exit(1);
	}

	logprint("Listening for connections.", 0);

	// drop privileges
	if (getuid() == 0) {
		logprint("Root privileges detected. Dropping.", 0);

		if (setgid(groupid) != 0) {
			logprint("setgid: Failed to drop group privileges.", errno);
			exit(1);
		}

		if (setuid(userid) != 0) {
			logprint("setuid: Failed to drop user privileges.", errno);
			exit(1);
		}

		logprint("Root privileges dropped.", 0);
	}

	recv_buffer = malloc(sizeof(char)*MAXRECV);

	// accept connection
	while (program_status >= 1) {
		addr_size = sizeof remote_address;
		new_fd = accept(sockfd, (struct sockaddr *) &remote_address, &addr_size);

		// interpret request - what do they want?
		memset(recv_buffer, 0, MAXRECV);
		status = recv(new_fd, recv_buffer, MAXRECV, 0);
		recv_bytes += status;

		if (strncmp("\r\n\r\n", recv_buffer+status-4, 4) != 0) {
			// Client isn't done sending data, and we aren't at the limit yet.
			while (recv_bytes < MAXRECV && status > 0) {
				status = recv(new_fd, recv_buffer + status, MAXRECV - status, 0);
				recv_bytes += status;
			}

			if (status > 0 && recv_bytes > MAXRECV) {
				logprint("Client exceeded maximum header size.\n", 0);
			}
		} 

		if (status < 0) {
			logprint("Socket error, closing socket.", errno);
		} else if (status == 0) {
			logprint("Client sent no request, closing socket.", 0);
		} else {
			if (strncmp("GET ", recv_buffer, 4) == 0) {
				logprint("Recieved a GET request.", 0);

				// "Validate" the request - our implementation is not standard,
				// it's crippled
				pcreExecRet = pcre_exec(reCompiled, pcreExtra, recv_buffer, MAXRECV, 0, PCRE_NEWLINE_ANY, subStrings, 6);
				
				if (pcreExecRet < 0) {
					// doesn't validate
					logprint("The GET request is not valid.", 0);

					catch_regex_error(pcreExecRet);
					cacherequest = malloc(sizeof(char)*3+1);
					strcpy(cacherequest, "400");
				} else {
					pcre_get_substring(recv_buffer, subStrings, pcreExecRet, 1, &psubStrMatchStr);

					cacherequest = malloc(sizeof(char)*strlen(psubStrMatchStr)+1);
					strcpy(cacherequest, psubStrMatchStr);
					pcre_free_substring(psubStrMatchStr);

					if (strcmp("", cacherequest) == 0) {
						free(cacherequest);
						cacherequest = malloc(sizeof(char)*5+1);
						strcpy(cacherequest, "index");
					}

					logprint("Valid, requested resource:", 0);
					logprint(cacherequest, 0);
				}
			} else {
				logprint("Bad request.", 0);
				cacherequest = malloc(sizeof(char)*3+1);
				strcpy(cacherequest, "400");
			}

			logprint("Sending response.", 0);

			// respond to request - what will we give them?
			memset(&served_file, 0, sizeof(served_file));
			served_file = storage[0]; // default to 404

			for (i = 1; i < loaded_files; i++) {
				if (strcmp(cacherequest, storage[i].alias) == 0 && storage[i].size > 0) {
					served_file = storage[i];
					logprint("Located requested resource in cache.", 0);
					break;
				}
			}
			free(cacherequest);
			status = handle_request(new_fd, served_file);
			if (status != 0) {
				logprint("handle_request returned error", 0);
			}
		}
		close(new_fd);
		logprint("Connection complete.", 0);
	}
	close(sockfd);

	free(recv_buffer);
	free(listenport);
	freeaddrinfo(res);

	logprint("Program shutdown.", errno);
	free(logfile);

	pcre_free(reCompiled);
	pcre_free(pcreExtra);

	for (i = 0; i < loaded_files; i++) {
		free(storage[i].contenttype);
		free(storage[i].statuscode);
		free(storage[i].sizestring);
		free(storage[i].data);
		free(storage[i].alias);
	}
	free(storage);
}
