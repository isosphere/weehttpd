#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <pcre.h>

#define LISTENPORT "8080"
#define LOGFILE "weehttpd.log"
#define QUEUE 10 // arbitrary selection
#define LONGESTERRORMESSAGE 60 // in glibc-2.7 the longest error is 50 chars. I still don't like this.

#define USERID 1000
#define GROUPID 1000

#define MAXFILES 20
#define BUFFERSIZE 100

struct cached_file {
    char *alias;
    char *data;
    long size;
    char *sizestring;
    int sizelength;
    char *contenttype;
};

struct cached_file loadfile(const char *path, const char *alias) {
    struct cached_file result;
    FILE *fhandle;

    result.alias = malloc(strlen(alias));
    strcpy(result.alias, alias);
    
    fhandle = fopen(path, "rb");
    if (fhandle == NULL) {
        result.data = "";
        result.size = 0;
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

int handle_request(int sockfd, char *header, struct cached_file content) {
    send(sockfd, header, strlen(header), 0);
    send(sockfd, "Content-Type: ", strlen("Content-Type: "), 0);
    send(sockfd, content.contenttype, strlen(content.contenttype), 0);
    send(sockfd, "\nContent-Length: ", strlen("Content-Length: "), 0);
    send(sockfd, content.sizestring, content.sizelength, 0);
    send(sockfd, "\n\n", 2, 0);
    send(sockfd, content.data, content.size, 0);
}

void main() {
    struct sockaddr_storage remote_address;
    socklen_t addr_size;
    struct addrinfo hints, *res;

    int i;
    int yes = 1;

    int sockfd, new_fd;
    int program_status = 5;
    int status = 0;

    char *header = malloc(1024);

    struct cached_file storage[MAXFILES];
    struct cached_file served_file;
    int loaded_files = 0;

    char *recv_buffer = malloc(BUFFERSIZE);

    char *request;
    char *cacherequest;

    pcre *reCompiled;
    pcre_extra *pcreExtra;
    const char *pcreErrorStr;
    const char *psubStrMatchStr;
    char *RequestMatchRegex;
    int pcreErrorOffset;
    int pcreExecRet;
    int subStrings[10]; // FIXME arbitrary hardcoded value

    // My implementation of request URIs is not to standard
    RequestMatchRegex = "^GET /([a-zA-Z0-9.\\-_]*) HTTP/\\d\\.\\d$";
    reCompiled = pcre_compile(RequestMatchRegex, PCRE_MULTILINE, &pcreErrorStr, &pcreErrorOffset, NULL);
    pcreExtra = pcre_study(reCompiled, 0, &pcreErrorStr);

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

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
        logprint("Failed to set socket options.", errno);
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
    strcpy(header, "200 OK\n");
    strcat(header, "Location: localhost\n");

    storage[0] = loadfile("files/404.htm", "404");
    storage[0].contenttype = "text/html";
    loaded_files++;

    storage[1] = loadfile("files/index.htm", "index");
    storage[1].contenttype = "text/html";
    loaded_files++;

    storage[2] = loadfile("files/image.jpg", "image.jpg");
    storage[2].contenttype = "image/jpeg";
    loaded_files++;

    // accept connection
    while (program_status >= 1) {
        addr_size = sizeof remote_address;
        new_fd = accept(sockfd, (struct sockaddr *) &remote_address, &addr_size);

        // interpret request - what do they want?
        memset(recv_buffer, 0, BUFFERSIZE);
        status = recv(new_fd, recv_buffer, BUFFERSIZE, 0);

        if (status <= 0) {
            logprint("Client sent no request, closing socket.", errno);
        } else {
            if (strncmp("GET ", recv_buffer, 4) == 0) {
                printf("A GET request!\n");
                printf("%s\n", recv_buffer);

                // "Validate" the request - our implementation is not standard,
                // it's crippled
                pcreExecRet = pcre_exec(reCompiled, pcreExtra, recv_buffer, BUFFERSIZE, 0, PCRE_NEWLINE_ANY, subStrings, 10);
                
                if (pcreExecRet < 0) {
                    // doesn't validate
                    catch_regex_error(pcreExecRet);
                } else {
                    pcre_get_substring(recv_buffer, subStrings, pcreExecRet, 1, &psubStrMatchStr);

                    cacherequest = malloc(strlen(psubStrMatchStr));
                    strcpy(cacherequest, psubStrMatchStr);

                    if (strcmp("", cacherequest) == 0) {
                        cacherequest = malloc(strlen("index"));
                        strcpy(cacherequest, "index");
                    }

                    printf("Request: '%s'\n", cacherequest);
                }
            }

            while (status = recv(new_fd, recv_buffer, 1, MSG_DONTWAIT) > 0) {
                // ignore the rest of the request because we are very rude
            }

            send(new_fd, "HTTP/1.1 ", 9, 0);

            // respond to request - what will we give them?

            memset(&served_file, 0, sizeof(served_file));
            served_file = storage[0]; // default to 404

            for (i = 1; i < loaded_files; i++) {
                if (strcmp(cacherequest, storage[i].alias) == 0 && storage[i].size > 0) {
                    served_file = storage[i];
                }
            }
            handle_request(new_fd, header, served_file);
        }
        close(new_fd);
    }

    logprint("Program ended and logfile closed.", errno);
    close(sockfd);
}
