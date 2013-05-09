#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#define LISTENPORT "8080"
#define LOGFILE "weehttpd.log"
#define QUEUE 10 // arbitrary selection
#define LONGESTERRORMESSAGE 60 // in glibc-2.7 the longest error is 50 chars. I still don't like this.

#define USERID 1000
#define GROUPID 1000

#define MAXFILES 20
#define BUFFERSIZE 50

struct cached_file {
    char *alias;
    char *data;
    long size;
    char *sizestring;
    int sizelength;
};

struct cached_file loadfile(const char *path, const char *alias) {
    struct cached_file result;
    FILE *fhandle;

    result.alias = malloc(sizeof(char)*strlen(alias));
    strcpy(result.alias, alias);
    
    fhandle = fopen(path, "rb");
    if (fhandle == NULL) {
        result.data = NULL;
        result.size = 0;
        result.alias = NULL;
    } else {
        fseek(fhandle, 0, SEEK_END);
        result.size = ftell(fhandle);
        rewind(fhandle);
        result.data = malloc(result.size * (sizeof(char)));
        fread(result.data, sizeof(char), result.size, fhandle);
        fclose(fhandle);
        
        // magic to convert long to string without buffer overflows
        result.sizelength = snprintf(NULL, 0, "%lu", result.size);
        char sizestring[result.sizelength+1];
        int c = snprintf(sizestring, result.sizelength+1, "%lu", result.size);


        result.sizestring = malloc(result.sizelength);
        strcpy(result.sizestring, sizestring);
    }

    return result;
}

void logprint(const char *message, int error) {
    // I could open the logfile once and change the buffering to
    // line-buffering, but it would complicate the code a bit. I don't know
    // which method would be techically superiour.
    size_t errorsize = LONGESTERRORMESSAGE;
    char *errormessage = malloc(sizeof(char) * errorsize);
    FILE *logfile;

    if (error > 0) 
        strerror_r(error, errormessage, errorsize);

    // do not touch the filesystem as root
    if (getuid() == 0) {
        printf("[%d] %s\n", time(NULL), message);
        if (error > 0) {
            printf("Error #%d: %s\n", error, errormessage);
        }
    } else {
        logfile = fopen(LOGFILE, "a");
        //setvbuf(LOGFILE, NULL, _IOLBF, 0); // line buffering

        if (logfile == NULL) {
            printf("Failed to open log file!: %d\n", errno);
            exit(1);
        } 

        fprintf(logfile, "[%d] %s\n", time(NULL), message);
        if (error > 0)
            fprintf(logfile, "Error #%d: %s\n", error, errormessage);

        fclose(logfile);
    }
}

int handle_request(int sockfd, char *header, struct cached_file content) {
    strcat(header, "Content-Length: ");
    strcat(header, content.sizestring);
    strcat(header, "\n\n");
    send(sockfd, header, strlen(header), 0);
    send(sockfd, content.data, content.size, 0);
}

void main() {
    struct sockaddr_storage remote_address;
    socklen_t addr_size;
    struct addrinfo hints, *res;

    int sockfd, new_fd;
    int program_status = 5;
    int status = 0;

    char *header = malloc(1024);

    struct cached_file storage[MAXFILES];
    struct cached_file filenotfound;
    int loaded_files = 0;

    char recv_buffer[BUFFERSIZE];
    
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    logprint("Program started.", 0);

    status = getaddrinfo(NULL, LISTENPORT, &hints, &res);
    if (status != 0) {
        logprint(gai_strerror(status), errno);
        exit(1);
    }

    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd == -1) {
        logprint("Failed to create socket descriptor.", errno);
        exit(1);
    }

    if (bind(sockfd, res->ai_addr, res->ai_addrlen) == -1) {
        logprint("Failed to bind socket.", errno);
        exit(1);
    }
    
    if (listen(sockfd, QUEUE) != 0) {
        logprint("Failed to listen for connections.", errno);
        exit(1);
    }

    // drop privileges
    if (getuid() == 0) {
        logprint("Root privileges detected. Dropping.", 0);

        if (setgid(GROUPID) != 0) {
            logprint("setgid: Failed to drop group privileges.", errno);
            exit(1);
        }

        if (setuid(USERID) != 0) {
            logprint("setuid: Failed to drop user privileges.", errno);
            exit(1);
        }

        logprint("Root privileges dropped.", 0);
    }

    // cache headers, pages
    //strcpy(header, "HTTP/1.1 ");
    //strcat(header, "200 OK\n");
    strcpy(header, "200 OK\n");
    strcat(header, "Location: localhost\n");
    strcat(header, "Content-Type: text/html\n");

    // built in pages
    filenotfound.alias = "Not found.";
    filenotfound.data = "<HTML><BODY>Not found.</BODY></HTML>";
    filenotfound.size = strlen(filenotfound.data);

    storage[0] = loadfile("main.htm", "index");
    if (storage[0].size == 0) {
        storage[0] = filenotfound;
    }
    loaded_files++;

    // accept connection
    while (program_status >= 1) {
        addr_size = sizeof remote_address;
        new_fd = accept(sockfd, (struct sockaddr *) &remote_address, &addr_size);

        // interpret request - what do they want?
        recv(new_fd, recv_buffer, BUFFERSIZE, 0);
        if (strncmp("GET ", recv_buffer, 4) == 0) {
            printf("A GET request!\n");
        }

        send(new_fd, "HTTP/1.1 ", 9, 0);

        // respond to request - what will we give them?
        handle_request(new_fd, header, storage[0]);

        close(new_fd);
    }

    logprint("Program ended and logfile closed.", errno);
    close(sockfd);
}
