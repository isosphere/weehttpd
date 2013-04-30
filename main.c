#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#define LISTENPORT "8080"
#define LOGFILE "weehttpd.log"
#define QUEUE 10

void logprint (FILE *logfile, char *message, int error) {
    const int errorsize = 60;
    char *errormessage;

    fprintf(logfile, "[%d] %s\n", time(NULL), message);
    if (error > 0) {
        strerror_r(errno, errormessage, errorsize);
        fprintf(logfile, "Error #%d: %s\n", errno, errormessage);
    }
}

int main(void) {
    struct sockaddr_storage remote_address;
    socklen_t addr_size;
    struct addrinfo hints, *res;
    int sockfd, new_fd;
    int program_status = 1;

    char *message;
    FILE *logfile;

    logfile = fopen(LOGFILE, "a");
    if (logfile == NULL) {
        logprint(logfile, "Failed to open log file!", errno);
        exit(1);
    } else {
        logprint(logfile, "Program started and logfile opened.", 0);
    }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    getaddrinfo(NULL, LISTENPORT, &hints, &res);

    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd == -1) {
        logprint(logfile, "Failed to create socket descriptor.", errno);
        exit(1);
    }

    if (bind(sockfd, res->ai_addr, res->ai_addrlen) == -1) {
        logprint(logfile, "Failed to bind socket.", errno);
        exit(1);
    }
    listen(sockfd, QUEUE);

    // accept connection
    while (program_status == 1) {
        addr_size = sizeof remote_address;
        new_fd = accept(sockfd, (struct sockaddr *) &remote_address, &addr_size);

        message = "I'm a server! Eeek!\n\n";
        send(new_fd, message, strlen(message), 0);
        close(new_fd);
    }

    logprint(logfile, "Program ended and logfile closed.", errno);
    close(logfile);

    return 0;
}
