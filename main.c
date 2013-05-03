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

void logprint(const char *message, int error) {
    // I could open the logfile once and change the buffering to
    // line-buffering, but it would complicate the code a bit. I don't know
    // which method would be techically superiour.
    size_t errorsize = 60;
    char *errormessage = malloc(sizeof(char) * errorsize);
    FILE *logfile;

    if (error > 0) 
        strerror_r(error, errormessage, errorsize);

    // do not touch the filesystem as root
    if (getuid() == 0) {
        printf("[%d] %s\n", time(NULL), message);
        if (error > 0) {
            printf("Error #%d: %s\n", error, errormessage);
            return;
        }
    }

    logfile = fopen(LOGFILE, "a");
    //setvbuf(LOGFILE, NULL, _IOLBF, 0); // line buffering

    if (logfile == NULL) {
        printf("Failed to open log file!: %s\n", errno);
        exit(1);
    } 

    fprintf(logfile, "[%d] %s\n", time(NULL), message);
    if (error > 0) {
        fprintf(logfile, "Error #%d: %s\n", error, errormessage);
    }

    fclose(logfile);
}

int handle_request(int sockfd, char *content) {
    char *header = malloc(1024); // FIXME warning, fixed sized buffer
    char *message = malloc(1024*5);

    // I want a way to economize buffer size and ensure no buffer overflows.
    strcpy(header, "HTTP/1.1");
    strcat(header, "200 OK\n");
    strcat(header, "Location: localhost\n");
    strcat(header, "Content-Type: text/html;");
    strcat(header, "\n\n");

    //message = "I'm a server! Eeek!\n";
    message = content;

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

    FILE *mainpage;
    char *cache_mainpage;
    long mainpage_filesize;

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

    // cache pages
    mainpage = fopen("main.htm", "rb");
    if (mainpage == NULL) {
        cache_mainpage = "Server works, but has no data to display.";
        logprint("No main.htm found, using default content.\n", 0);
    } else {
        fseek(mainpage, 0, SEEK_END);
        mainpage_filesize = ftell(mainpage);
        rewind(mainpage);
        cache_mainpage = malloc(mainpage_filesize * (sizeof(char)));
        fread(cache_mainpage, sizeof(char), mainpage_filesize, mainpage);
        fclose(mainpage);
    }

    // accept connection
    while (program_status >= 1) {
        addr_size = sizeof remote_address;
        new_fd = accept(sockfd, (struct sockaddr *) &remote_address, &addr_size);

        handle_request(new_fd, cache_mainpage);

        close(new_fd);
    }

    logprint("Program ended and logfile closed.", errno);
    close(sockfd);

    return 0;
}
