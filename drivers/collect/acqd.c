#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <pthread.h>
#include <syslog.h>
#include "acquisition_ioctl.h"

#define LOG_HEAD "acqd: "
#define SIGREDY 59
#define PORT 7777
#define BACKLOG 10
#define BUFFER_SIZE 1024
#define RESPONSE_STRING "Server: Invalid cmd!\n"

// client infos
typedef struct {
    int fd;
    struct sockaddr_in addr;
} client_info_t;

// global variables
static int fdev = -1;
static volatile int keep_running = 1;
pthread_mutex_t signo_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;

// signal handler
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        pthread_mutex_lock(&print_mutex);
        syslog(LOG_DEBUG,LOG_HEAD"\nReceived signal %d, shutting down...\n", sig);
        pthread_mutex_unlock(&print_mutex);
        keep_running = 0;
    }else if ( sig == SIGREDY ){
        pthread_mutex_unlock(&signo_mutex);
    }
}

int setup_device(void){
    fdev = open("/dev/data_acquisition",O_RDWR);
    if( fdev <= 0){
        syslog(LOG_ERR,LOG_HEAD"open device node error!\n");
        return -1;
    };
    int ret = ioctl(fdev,IOCTL_SET_SIGNO,SIGREDY);
    if( ret < 0 )
        close(fdev);
    return ret;
};

void cleanup_device(void){
    close(fdev);
}

int parse_cmd(char* cmd,int *cmd_type, int* cmd_length){
    char *token;
    const char* s = " ";
    token = strtok(cmd, s);

    while( token != NULL ) {
        if( strncasecmp(token,"GET",strlen("GET")) == 0 ){
            // encounterd with cmd 
            token = strtok(NULL, s);
            break;
        };
        token = strtok(NULL, s);
    };

    if( token == NULL ){
        return -1;
    };

    // choose which type
    int type = 0;
    for( type = 0; type < TYPE_MAX; type++ ){
        if( strncasecmp(token,types[type],strlen(types[type])) == 0 ){
            break;
        };
    };

    if( type >= TYPE_MAX ){
        return -1; // invalid types
    };
    *cmd_type = type;

    // got requires nums
    char *endptr = NULL;
    token = strtok(NULL, s);
    if( token == NULL ){
        return -1; // incomplete cmd
    };

    long len = strtol(token,&endptr,10);
    if( len <= 0 || len > INT_MAX ){
        return -1;
    };
    *cmd_length = len;

    return 0;
};

char* do_cmd(int* cmd_type, int* cmd_length){
    // set data type
    int ret = ioctl(fdev,IOCTL_DATA_TYPE, *cmd_type);
    if( ret != *cmd_type ){
        return NULL;
    };
    // set buffer length
    ret = ioctl(fdev,IOCTL_SET_BUFFER, *cmd_length);
    if( ret < *cmd_length ){
        return NULL;
    };
    // set transfer length
    ret = ioctl(fdev,IOCTL_SET_LENGTH, *cmd_length);
    if( ret != *cmd_length ){
        return NULL;
    };
    // trigger transfer 
    ret = ioctl(fdev,IOCTL_START_TRANSFER);
    if( ret != 0 ){
        return NULL;
    };
    // wait on signal
    pthread_mutex_lock(&signo_mutex);
    // signal caught
    char* buf = malloc(sizeof(char)*(*cmd_length+1));
    if( buf == NULL ){
        return NULL;
    };
    ret = read(fdev,buf,*cmd_length);
    buf[ret]='\0';
    pthread_mutex_unlock(&signo_mutex);
    return buf;
}
        

// run thread, do client cmds
void* client_thread(void* arg) {
    client_info_t* client = (client_info_t*)arg;
    int client_fd = client->fd;
    struct sockaddr_in client_addr = client->addr;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;
    
    // free client memory
    free(client);
    
    // get client infos
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    int client_port = ntohs(client_addr.sin_port);
    
    // receive bytes
    bytes_received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received < 0) {
        pthread_mutex_lock(&print_mutex);
        syslog(LOG_INFO,LOG_HEAD"recv client failed\n");
        pthread_mutex_unlock(&print_mutex);
        close(client_fd);
        return NULL;
    } else if (bytes_received == 0) {
        pthread_mutex_lock(&print_mutex);
        syslog(LOG_INFO,LOG_HEAD"[Thread %lu] Client disconnected\n", pthread_self());
        pthread_mutex_unlock(&print_mutex);
        close(client_fd);
        return NULL;
    }
    
    // ensure tail 
    buffer[bytes_received] = '\0';
    // start cmd 
    char* resp = RESPONSE_STRING;
    int type = 0;
    int len  = 0;
    int ret = parse_cmd(buffer,&type,&len);
    if( ret == 0 ){// do cmd
        resp = do_cmd(&type,&len);
        if( !resp ){
            kill(getpid(),SIGREDY);
            resp = RESPONSE_STRING;
            len = strlen(RESPONSE_STRING);
	    };
    };

    // send response
    ssize_t bytes_sent = send(client_fd, resp, len, 0);
    
    // close connect
    close(client_fd);
    
    pthread_mutex_lock(&print_mutex);
    syslog(LOG_INFO,LOG_HEAD"[Thread %lu] Sent response (%d bytes)\n", pthread_self(), bytes_sent);
    pthread_mutex_unlock(&print_mutex);
    
    return NULL;
}

int main(int argc, char* argv[]) {
    openlog("acqd", LOG_PID|LOG_CONS, LOG_LOCAL0);

    int ret = setup_device();
    if( ret < 0 ){
        return ret;
    };

    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    pthread_t thread_id;
    
    // create signal handler
    signal(SIGINT , signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGREDY, signal_handler);
    
    // create socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        syslog(LOG_ERR, LOG_HEAD"socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    // set SO_REUSEADDR
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        syslog(LOG_ERR, LOG_HEAD"setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    
    // bind addr and port
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        syslog(LOG_ERR, LOG_HEAD"bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    
    // listen port
    if (listen(server_fd, BACKLOG) < 0) {
        syslog(LOG_ERR, LOG_HEAD"listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    
    syslog(LOG_INFO,LOG_HEAD"Multithreaded TCP Server started on port %d\n", PORT);
    syslog(LOG_INFO,LOG_HEAD"Waiting for connections...\n");
    syslog(LOG_INFO,LOG_HEAD"Press Ctrl+C to stop the server.\n");
    
    // main loop
    while (keep_running) {
        // accept connect
        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (keep_running) {
                syslog(LOG_ERR, LOG_HEAD"accept failed");
            }
            continue;
        }
        // block client thread
        pthread_mutex_lock(&signo_mutex);
        // new client thread
        client_info_t* client = malloc(sizeof(client_info_t));
        if (!client) {
            syslog(LOG_ERR, LOG_HEAD"malloc failed");
            close(client_fd);
            continue;
        }
        
        client->fd = client_fd;
        client->addr = client_addr;
        
        if (pthread_create(&thread_id, NULL, client_thread, client) != 0) {
            syslog(LOG_ERR, LOG_HEAD"pthread_create failed");
            free(client);
            close(client_fd);
            continue;
        }
        
        // detach thread infos
        pthread_detach(thread_id);
    }
    
    // cleanup
    syslog(LOG_INFO,LOG_HEAD"Shutting down server...\n");
    close(server_fd);
    pthread_mutex_destroy(&print_mutex);
    syslog(LOG_INFO,LOG_HEAD"Server stopped successfully.\n");
    closelog();
    cleanup_device();
    return 0;
}
