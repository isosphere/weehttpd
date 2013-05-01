#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#define LISTENPORT "http"
#define LOGFILE "weehttpd.log"
#define QUEUE 10

#define USERID 1000
#define GROUPID 1000

void logprint (const char *message, int error) {
    // I could open the logfile once and change the buffering to
    // line-buffering, but it would complicate the code a bit. I don't know
    // which method would be techically superiour.
    const int errorsize = 60;
    char *errormessage;
    FILE *logfile;

    logfile = fopen(LOGFILE, "a");
    //setvbuf(LOGFILE, NULL, _IOLBF, 0); // line buffering

    if (logfile == NULL) {
        printf("Failed to open log file!");
        exit(1);
    } 

    fprintf(logfile, "[%d] %s\n", time(NULL), message);
    if (error > 0) {
        strerror_r(errno, errormessage, errorsize);
        fprintf(logfile, "Error #%d: %s\n", errno, errormessage);
    }

    fclose(logfile);
}

int handle_request(int sockfd) {
    char *header = malloc(1024); // FIXME warning, fixed sized buffer
    char *message = malloc(1024*5);

    strcpy(header, "HTTP/1.1");
    strcat(header, "200 OK\n");
    strcat(header, "Location: localhost\n");
    strcat(header, "Content-Type: text/plain;");
    strcat(header, "\n\n");

    message = "I'm a server! Eeek!\n";

    send(sockfd, header, strlen(header), 0);
    send(sockfd, message, strlen(message), 0);
}

int main(void) {
    struct sockaddr_storage remote_address;
    socklen_t addr_size;
    struct addrinfo hints, *res;

    int sockfd, new_fd;
    int program_status = 5;
    int status = 0;

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

    // accept connection
    while (program_status >= 1) {
        addr_size = sizeof remote_address;
        new_fd = accept(sockfd, (struct sockaddr *) &remote_address, &addr_size);

        handle_request(new_fd);

        close(new_fd);
    }

    logprint("Program ended and logfile closed.", errno);
    close(sockfd);

    return 0;
}
